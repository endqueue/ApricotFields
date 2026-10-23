
#include "render/aprendpipeline.h"
#include "aprend_internal.hpp"
#include "aprendimages_internal.hpp"
#include <spudfiles.h>
#include <algorithm>
#include <cstring>
#include <utility>

// Maps a vertex element type onto the SpudGPU format used to describe it in
// a vertex_attribute_desc. Every type has one: the pipeline refuses any
// other value before this is reached.
static SPUDGPU_FORMAT aprend_vertex_element_type_to_format(APREND_VERTEX_ELEMENT_TYPE type) {
	switch (type) {
	case APREND_VERTEX_ELEMENT_TYPE_FLOAT:
		return SPUDGPU_FORMAT_R32_FLOAT;
	case APREND_VERTEX_ELEMENT_TYPE_VEC2:
		return SPUDGPU_FORMAT_R32G32_FLOAT;
	case APREND_VERTEX_ELEMENT_TYPE_VEC3:
		return SPUDGPU_FORMAT_R32G32B32_FLOAT;
	case APREND_VERTEX_ELEMENT_TYPE_VEC4:
		return SPUDGPU_FORMAT_R32G32B32A32_FLOAT;
	case APREND_VERTEX_ELEMENT_TYPE_INT:
		return SPUDGPU_FORMAT_R32_SINT;
	case APREND_VERTEX_ELEMENT_TYPE_INT2:
		return SPUDGPU_FORMAT_R32G32_SINT;
	case APREND_VERTEX_ELEMENT_TYPE_INT3:
		return SPUDGPU_FORMAT_R32G32B32_SINT;
	case APREND_VERTEX_ELEMENT_TYPE_INT4:
		return SPUDGPU_FORMAT_R32G32B32A32_SINT;
	case APREND_VERTEX_ELEMENT_TYPE_UINT:
		return SPUDGPU_FORMAT_R32_UINT;
	case APREND_VERTEX_ELEMENT_TYPE_UINT2:
		return SPUDGPU_FORMAT_R32G32_UINT;
	case APREND_VERTEX_ELEMENT_TYPE_UINT3:
		return SPUDGPU_FORMAT_R32G32B32_UINT;
	case APREND_VERTEX_ELEMENT_TYPE_UINT4:
		return SPUDGPU_FORMAT_R32G32B32A32_UINT;
	default:
		return SPUDGPU_FORMAT_UNKNOWN;
	}
}

aprend_shader_t::~aprend_shader_t() { spudgpu_destroy_shader_module(this->shader_module); }
aprend_graphics_pipeline_t::~aprend_graphics_pipeline_t() {
	// A pipeline that failed to create has nothing to destroy.
	if (this->pipeline)
		spudgpu_destroy_shader_pipeline(this->pipeline);
	free(this->entry_points);
	free(this->vertex_elements);
}
aprend_compute_pipeline_t::~aprend_compute_pipeline_t() {
	// A pipeline that failed to create has nothing to destroy.
	if (this->pipeline)
		spudgpu_destroy_compute_pipeline(this->pipeline);
	free(this->entry_point);
}
void aprend_binding_layout_core_release(aprend_binding_layout_core *core) {
	if (!core)
		return;
	if (--core->holders > 0)
		return;
	// The last holder: no layout or set of these bindings is left, so the
	// next layout made with them starts a core of its own.
	std::vector<aprend_binding_layout_core *> &cores = core->instance->binding_layout_cores;
	for (size_t i = 0; i < cores.size(); ++i) {
		if (cores[i] == core) {
			cores.erase(cores.begin() + (std::ptrdiff_t)i);
			break;
		}
	}
	// Pools, then the layout their sets were allocated against.
	for (uint32_t i = 0; i < core->pool_count; ++i)
		spudgpu_destroy_descriptor_pool(core->pools[i]);
	free(core->pools);
	free(core->free_sets);
	if (core->layout)
		spudgpu_destroy_descriptor_set_layout(core->layout);
	free(core);
}

aprend_binding_layout_t::~aprend_binding_layout_t() {
	// NULL for a layout that failed before it found its core.
	aprend_binding_layout_core_release(this->core);
}
aprend_binding_set_t::~aprend_binding_set_t() {
	free(this->entries);
	free(this->image_uses);
	free(this->storage_stores);
	// A set that failed to create gives back what it got as far as acquiring,
	// and lets go of the core only once it has: the core may go with it.
	for (uint32_t frame = 0; frame < this->frame_set_count; ++frame)
		this->core->free_sets[this->core->free_set_count++] = this->sets[frame];
	aprend_binding_layout_core_release(this->core);
}

static_assert(APREND_DESCRIPTOR_TYPE_COUNT <= SPUDGPU_MAX_DESCRIPTOR_POOL_SIZES);

// Adds one more pool to [layout], and room on its free list for the sets that
// pool will hold.
static SPUDRESULT aprend_binding_layout_add_pool(aprend_binding_layout_core *layout) {
	const uint32_t pool_count = layout->pool_count + 1;

	spudgpu_descriptor_pool *pools = (spudgpu_descriptor_pool *)realloc(layout->pools, sizeof(spudgpu_descriptor_pool) * pool_count);
	if (!pools)
		return SPUDRESULT_OUT_OF_MEMORY;
	layout->pools = pools;

	spudgpu_descriptor_set *free_sets =
	    (spudgpu_descriptor_set *)realloc(layout->free_sets, sizeof(spudgpu_descriptor_set) * pool_count * APREND_BINDING_SETS_PER_POOL);
	if (!free_sets)
		return SPUDRESULT_OUT_OF_MEMORY;
	layout->free_sets = free_sets;

	// One pool size per descriptor type the layout uses.
	uint32_t per_set[APREND_DESCRIPTOR_TYPE_COUNT]{};
	for (uint32_t i = 0; i < layout->binding_count; ++i)
		per_set[layout->bindings[i]._type] += layout->bindings[i]._count;

	spudgpu_descriptor_pool_desc pool_desc{};
	pool_desc.base.type = SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC;
	pool_desc.max_sets = APREND_BINDING_SETS_PER_POOL;
	for (uint32_t type = 0; type < APREND_DESCRIPTOR_TYPE_COUNT; ++type) {
		if (per_set[type] == 0)
			continue;
		pool_desc.pool_sizes[pool_desc.pool_size_count].descriptor_type = type;
		pool_desc.pool_sizes[pool_desc.pool_size_count].count           = APREND_BINDING_SETS_PER_POOL * per_set[type];
		++pool_desc.pool_size_count;
	}
	SPUDRESULT sr = spudgpu_create_descriptor_pool(layout->instance->desc.device, &pool_desc, &layout->pools[pool_count - 1]);
	if (SPUDFAIL(sr))
		return sr;

	layout->pool_count        = pool_count;
	layout->sets_in_last_pool = 0;
	return SPUD_SUCCESS;
}

// A descriptor set of [layout]: one given back by a destroyed
// aprend_binding_set if there is one, else a new one from the last pool,
// adding a pool when that is full.
static SPUDRESULT aprend_binding_layout_acquire_set(aprend_binding_layout_core *layout, spudgpu_descriptor_set *out_set) {
	if (layout->free_set_count > 0) {
		*out_set = layout->free_sets[--layout->free_set_count];
		return SPUD_SUCCESS;
	}
	if (layout->pool_count == 0 || layout->sets_in_last_pool == APREND_BINDING_SETS_PER_POOL) {
		SPUDRESULT sr = aprend_binding_layout_add_pool(layout);
		if (SPUDFAIL(sr))
			return sr;
	}

	spudgpu_descriptor_set_desc set_desc{};
	set_desc.base.type = SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC;
	set_desc.pool           = layout->pools[layout->pool_count - 1];
	set_desc.set_layouts[0] = layout->layout;
	set_desc.set_count      = 1;
	SPUDRESULT sr = spudgpu_create_descriptor_sets(layout->instance->desc.device, &set_desc, out_set);
	if (SPUDFAIL(sr))
		return sr;
	++layout->sets_in_last_pool;
	return SPUD_SUCCESS;
}

static const aprend_binding_desc *aprend_binding_layout_find_desc(const aprend_binding_layout_t *layout, uint32_t binding) {
	for (uint32_t i = 0; i < layout->binding_count; ++i)
		if (layout->bindings[i]._binding == binding)
			return &layout->bindings[i];
	return nullptr;
}

static bool aprend_descriptor_type_reads_view(SPUDGPU_DESCRIPTOR_TYPE type) {
	return type == SPUDGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE || type == SPUDGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
	       type == SPUDGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}
static bool aprend_descriptor_type_reads_sampler(SPUDGPU_DESCRIPTOR_TYPE type) {
	return type == SPUDGPU_DESCRIPTOR_TYPE_SAMPLER || type == SPUDGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}

// Whether [offset] and [range] (0 = to the end) stay inside a buffer of
// [size] bytes and cover at least one of them.
static bool aprend_binding_range_valid(uint64_t offset, uint64_t range, uint64_t size) {
	if (offset >= size)
		return false;
	if (range > size - offset)
		return false;
	return true;
}

// Whether entry [index] of a set holds what its _type reads. The type has
// already been checked against the layout's.
static bool aprend_binding_set_entry_resource_valid(const char *who, aprend_instance instance, const aprend_binding_set_entry &entry, uint32_t index) {
	switch (entry._type) {
	case SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER: {
		const auto &r = entry._resource._uniform;
		if (!r._buffer && !r._set) {
			printf("apricot: %s: entry %u (binding %u) has no uniform buffer and no uniform buffer set\n", who, index, entry._binding);
			return false;
		}
		if (r._buffer && r._set) {
			printf("apricot: %s: entry %u (binding %u) has both a uniform buffer and a uniform buffer set\n", who, index, entry._binding);
			return false;
		}
		if (r._set && r._set->instance != instance) {
			printf("apricot: %s: entry %u (binding %u) has a uniform buffer set of another instance\n", who, index, entry._binding);
			return false;
		}
		// The device's rule for where a uniform buffer range may start.
		if (r._offset % instance->uniform_buffer_offset_alignment != 0) {
			printf("apricot: %s: entry %u (binding %u) has a uniform buffer _offset of %llu, which is not a multiple of the device's %u\n", who,
			       index, entry._binding, (unsigned long long)r._offset, instance->uniform_buffer_offset_alignment);
			return false;
		}
		// Every copy of a set is the same size.
		const aprend_uniform_buffer buffer = r._set ? r._set->buffers[0] : r._buffer;
		if (!aprend_binding_range_valid(r._offset, r._range, buffer->size)) {
			printf("apricot: %s: entry %u (binding %u) has an _offset and _range outside its buffer\n", who, index, entry._binding);
			return false;
		}
		return true;
	}
	case SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER: {
		const auto &r = entry._resource._storage;
		if (!r._buffer && !r._set) {
			printf("apricot: %s: entry %u (binding %u) has no storage buffer and no storage buffer set\n", who, index, entry._binding);
			return false;
		}
		if (r._buffer && r._set) {
			printf("apricot: %s: entry %u (binding %u) has both a storage buffer and a storage buffer set\n", who, index, entry._binding);
			return false;
		}
		if (r._set && r._set->instance != instance) {
			printf("apricot: %s: entry %u (binding %u) has a storage buffer set of another instance\n", who, index, entry._binding);
			return false;
		}
		// The device's rule for where a storage buffer range may start.
		if (r._offset % instance->storage_buffer_offset_alignment != 0) {
			printf("apricot: %s: entry %u (binding %u) has a storage buffer _offset of %llu, which is not a multiple of the device's %u\n", who,
			       index, entry._binding, (unsigned long long)r._offset, instance->storage_buffer_offset_alignment);
			return false;
		}
		const aprend_storage_buffer buffer = r._set ? r._set->buffers[0] : r._buffer;
		if (!aprend_binding_range_valid(r._offset, r._range, buffer->size)) {
			printf("apricot: %s: entry %u (binding %u) has an _offset and _range outside its buffer\n", who, index, entry._binding);
			return false;
		}
		return true;
	}
	default:
		break;
	}

	const auto &r = entry._resource._image;
	if (aprend_descriptor_type_reads_view(entry._type)) {
		if (!r._view) {
			printf("apricot: %s: entry %u (binding %u) has no texture view\n", who, index, entry._binding);
			return false;
		}
		const APREND_TEXTURE_VIEW_TYPE wanted = entry._type == SPUDGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE
		                                            ? APREND_TEXTURE_VIEW_TYPE_UNORDERED_ACCESS
		                                            : APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE;
		if (r._view->view_type != wanted) {
			printf("apricot: %s: entry %u (binding %u) has a view of type %u, its slot takes %u\n", who, index, entry._binding,
			       (unsigned)r._view->view_type, (unsigned)wanted);
			return false;
		}
	}
	if (aprend_descriptor_type_reads_sampler(entry._type)) {
		if (!r._sampler) {
			printf("apricot: %s: entry %u (binding %u) has no sampler\n", who, index, entry._binding);
			return false;
		}
	}
	return true;
}

// Whether two blend entries blend the same way, by SpudGPU's rule: two
// that don't blend are the same whatever their factors. Entries that differ
// need SPUDGPU_FEATURE_INDEPENDENT_BLEND.
static bool aprend_blends_equal(const spudgpu_blend_attachment_desc &a, const spudgpu_blend_attachment_desc &b) {
	if (a.blend_enable != b.blend_enable)
		return false;
	if (!a.blend_enable)
		return true;
	return a.src_color_blend_factor == b.src_color_blend_factor && a.dst_color_blend_factor == b.dst_color_blend_factor &&
	       a.color_blend_op == b.color_blend_op && a.src_alpha_blend_factor == b.src_alpha_blend_factor &&
	       a.dst_alpha_blend_factor == b.dst_alpha_blend_factor && a.alpha_blend_op == b.alpha_blend_op;
}

static bool aprend_shader_stage_supported(SPUDGPU_SHADER_STAGE shader_stage) {
	switch (shader_stage) {
	case SPUDGPU_SHADER_STAGE_VERTEX:
	case SPUDGPU_SHADER_STAGE_FRAGMENT:
	case SPUDGPU_SHADER_STAGE_COMPUTE:
	case SPUDGPU_SHADER_STAGE_MESH:
		return true;
	default:
		printf("apricot: only vertex, fragment, compute and mesh shaders are supported now!\n");
		return false;
	}
}

// False, printed for [who], if [shader_stage] needs a feature [instance]'s
// device was created without. A mesh shader module is refused there on every
// backend: on Vulkan the SPIR-V itself needs VK_EXT_mesh_shader enabled.
static bool aprend_shader_stage_feature_ok(aprend_instance instance, SPUDGPU_SHADER_STAGE shader_stage, const char *who) {
	if (shader_stage == SPUDGPU_SHADER_STAGE_MESH && !(instance->features & SPUDGPU_FEATURE_MESH_SHADING)) {
		printf("apricot: %s: a mesh shader and the device was created without SPUDGPU_FEATURE_MESH_SHADING\n", who);
		return false;
	}
	return true;
}

// Reflects [size] bytes of SPIR-V into [shader]'s reflection. False, printed
// for [who], if it can't be. [code] is the caller's and may not be aligned to
// 4, so the words are read through a copy where it isn't.
static bool aprend_shader_reflect(
    aprend_shader_t *shader,
    const void *code,
    uint64_t size,
    SPUDGPU_SHADER_STAGE shader_stage,
    const char *who) {
	std::string error;
	bool ok = false;
	if (((uintptr_t)code % alignof(uint32_t)) == 0) {
		ok = aprend_reflect_spirv((const uint32_t *)code, (size_t)(size / 4), shader_stage, &shader->reflection, &error);
	} else {
		uint32_t *words = (uint32_t *)malloc(size);
		if (!words) {
			printf("apricot: %s: out of memory\n", who);
			return false;
		}
		memcpy(words, code, size);
		ok = aprend_reflect_spirv(words, (size_t)(size / 4), shader_stage, &shader->reflection, &error);
		free(words);
	}
	if (!ok)
		printf("apricot: %s: the shader can't be reflected: %s\n", who, error.c_str());
	return ok;
}

// SpudGPU copies the SPIR-V into its own shader module, so [code] only has to
// live for this call.
static SPUDRESULT aprend_shader_create_module(
    aprend_shader_t *shader,
    const void *code,
    uint64_t size,
    SPUDGPU_SHADER_STAGE shader_stage,
    const char *debug_name) {
	spudgpu_shader_module_desc smd = {};
	smd.base.type = SPUD_STRUCT_TYPE_GPU_SHADER_MODULE_DESC;
	smd.stage                      = shader_stage;
	smd.spirv_code                 = code;
	smd.spirv_size                 = size;
	smd.debug_name = debug_name;
	shader->stage = shader_stage;
	return spudgpu_create_shader_module(shader->instance->desc.device, &smd, &shader->shader_module);
}

// A stage's name, for messages.
static const char *aprend_stage_name(SPUDGPU_SHADER_STAGE stage) {
	switch (stage) {
	case SPUDGPU_SHADER_STAGE_VERTEX:
		return "vertex";
	case SPUDGPU_SHADER_STAGE_FRAGMENT:
		return "fragment";
	case SPUDGPU_SHADER_STAGE_COMPUTE:
		return "compute";
	case SPUDGPU_SHADER_STAGE_MESH:
		return "mesh";
	default:
		return "unknown";
	}
}

// Checks [shader] against what a pipeline declares for it: [entry_point],
// the [layout_count] [layouts] and the [range_count] push constant [ranges].
// Adds the images it uses to [images] and the storage bindings it writes to
// [writes]. False, printed for [who], at the first mismatch. Throws
// std::bad_alloc from [images] or [writes].
/* Whether [desc]'s fragment shader writes each of its color targets, and
 * only those, as the kind of number each target's format holds. A target
 * the shader doesn't write gets undefined values on every API (SpudGPU has
 * no write mask to leave it alone); a location with no target is refused
 * by Metal; and a float written to an integer target, or the other way
 * round, is undefined. False, printed for [who], if not. */
static bool aprend_pipeline_check_outputs(const char *who, const aprend_graphics_pipeline_desc &desc) {
	const aprend_reflection_store &fragment = desc.fragment_shader->reflection;
	const uint32_t outputs                  = fragment.view.color_output_count;
	if (outputs > desc.color_attachment_count) {
		printf("apricot: %s: the fragment shader writes color location %u, and the pipeline has %u color targets\n", who, outputs - 1,
		       desc.color_attachment_count);
		return false;
	}
	for (uint32_t i = 0; i < desc.color_attachment_count; ++i) {
		const APREND_SHADER_SCALAR_TYPE written = i < fragment.output_types.size() ? fragment.output_types[i] : APREND_SHADER_SCALAR_TYPE_NONE;
		if (written == APREND_SHADER_SCALAR_TYPE_NONE) {
			printf("apricot: %s: the fragment shader doesn't write color location %u, and the pipeline has a target there, which would be left "
			       "undefined\n",
			       who, i);
			return false;
		}
		const APREND_SHADER_SCALAR_TYPE holds = aprend_format_scalar_type(desc.color_attachment_formats[i]);
		if (holds != APREND_SHADER_SCALAR_TYPE_NONE && written != holds) {
			printf("apricot: %s: the fragment shader writes %s to color location %u, and its target's format %u holds %s\n", who,
			       aprend_scalar_type_name(written), i, (unsigned)desc.color_attachment_formats[i], aprend_scalar_type_name(holds));
			return false;
		}
	}
	return true;
}

static bool aprend_pipeline_check_shader(
    const char *who,
    aprend_shader shader,
    const char *entry_point,
    const aprend_binding_layout *layouts,
    uint32_t layout_count,
    const spudgpu_push_constant_range_desc *ranges,
    uint32_t range_count,
    std::vector<aprend_image_expectation> &images,
    std::vector<aprend_storage_write> &writes) {
	const aprend_shader_reflection &reflection = shader->reflection.view;
	const char *stage                          = aprend_stage_name(shader->stage);
	if (reflection.entry_point && strcmp(reflection.entry_point, entry_point) != 0) {
		printf("apricot: %s: the %s shader's entry point is \"%s\", and the pipeline names \"%s\"\n", who, stage, reflection.entry_point,
		       entry_point);
		return false;
	}

	for (uint32_t r = 0; r < reflection.resource_count; ++r) {
		const aprend_shader_resource &resource = reflection.resources[r];
		if (!resource.used)
			continue;
		if (resource.set >= layout_count) {
			printf("apricot: %s: the %s shader uses \"%s\" at set %u binding %u, and the pipeline declares %u sets\n", who, stage, resource.name,
			       resource.set, resource.binding, layout_count);
			return false;
		}
		const aprend_binding_desc *binding = aprend_binding_layout_find_desc(layouts[resource.set], resource.binding);
		if (!binding) {
			printf("apricot: %s: the %s shader uses \"%s\" at set %u binding %u, which the layout at set %u doesn't declare\n", who, stage,
			       resource.name, resource.set, resource.binding, resource.set);
			return false;
		}
		if (binding->_type != resource.type) {
			printf("apricot: %s: the %s shader reads set %u binding %u (\"%s\") as type %u, and the layout declares type %u\n", who, stage,
			       resource.set, resource.binding, resource.name, (unsigned)resource.type, (unsigned)binding->_type);
			return false;
		}
		if (binding->_count < resource.count) {
			printf("apricot: %s: the %s shader reads %u descriptors at set %u binding %u (\"%s\"), and the layout declares %u\n", who, stage,
			       resource.count, resource.set, resource.binding, resource.name, binding->_count);
			return false;
		}
		if (!(binding->_stages & shader->stage)) {
			printf("apricot: %s: the %s shader uses set %u binding %u (\"%s\"), and the layout doesn't give the binding to that stage\n", who,
			       stage, resource.set, resource.binding, resource.name);
			return false;
		}
		if (resource.image_shape != APREND_TEXTURE_VIEW_SHAPE_NONE) {
			SPUDGPU_FORMAT format = SPUDGPU_FORMAT_UNKNOWN;
			for (const aprend_reflection_store::storage_format &declared : shader->reflection.storage_formats)
				if (declared.set == resource.set && declared.binding == resource.binding)
					format = declared.format;
			images.push_back({resource.set, resource.binding, resource.image_shape, resource.image_scalar_type, format});
		}
		const bool storage = resource.type == SPUDGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE || resource.type == SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		if (storage && !resource.read_only && !aprend_storage_writes_hold(writes, resource.set, resource.binding))
			writes.push_back({resource.set, resource.binding});
	}

	if (reflection.has_push_constants) {
		// Each used member's bytes, a word at a time, in a range for this
		// stage.
		for (uint32_t m = 0; m < reflection.push_constant_member_count; ++m) {
			const aprend_shader_member &member = reflection.members[reflection.push_constant_first_member + m];
			// Only what this stage touches needs a range of this stage: two
			// stages may share a block and each read part of it.
			if (!member.used)
				continue;
			for (uint32_t byte = member.offset; byte < member.offset + member.size; byte += 4) {
				bool covered = false;
				for (uint32_t i = 0; i < range_count && !covered; ++i)
					covered = (ranges[i].stage_flags & shader->stage) && byte >= ranges[i].offset && byte - ranges[i].offset < ranges[i].size;
				if (!covered) {
					printf("apricot: %s: the %s shader reads push constant \"%s\" at byte %u, which no push constant range for that stage covers\n",
					       who, stage, member.name, byte);
					return false;
				}
			}
		}
	}
	return true;
}

// The kind of number an element of [type] gives a vertex shader.
static APREND_SHADER_SCALAR_TYPE aprend_element_scalar_type(APREND_VERTEX_ELEMENT_TYPE type) {
	switch (type) {
	case APREND_VERTEX_ELEMENT_TYPE_FLOAT:
	case APREND_VERTEX_ELEMENT_TYPE_VEC2:
	case APREND_VERTEX_ELEMENT_TYPE_VEC3:
	case APREND_VERTEX_ELEMENT_TYPE_VEC4:
		return APREND_SHADER_SCALAR_TYPE_FLOAT;
	case APREND_VERTEX_ELEMENT_TYPE_INT:
	case APREND_VERTEX_ELEMENT_TYPE_INT2:
	case APREND_VERTEX_ELEMENT_TYPE_INT3:
	case APREND_VERTEX_ELEMENT_TYPE_INT4:
		return APREND_SHADER_SCALAR_TYPE_INT;
	case APREND_VERTEX_ELEMENT_TYPE_UINT:
	case APREND_VERTEX_ELEMENT_TYPE_UINT2:
	case APREND_VERTEX_ELEMENT_TYPE_UINT3:
	case APREND_VERTEX_ELEMENT_TYPE_UINT4:
		return APREND_SHADER_SCALAR_TYPE_UINT;
	default:
		return APREND_SHADER_SCALAR_TYPE_NONE;
	}
}

// One vertex element and the location it is given, UINT32_MAX for one the
// shader doesn't read.
struct aprend_vertex_attribute_plan {
	uint32_t binding;
	const aprend_vertex_element *element;
	uint32_t location;
};

// Gives each element of [desc]'s vertex bindings its location, by position
// or by the names of [vertex_shader]'s inputs, and checks every input the
// shader uses is fed at each of its locations by an element of its kind of
// number. False, printed for [who], if not. Throws std::bad_alloc.
static bool aprend_pipeline_plan_vertex_input(
    const char *who,
    const aprend_graphics_pipeline_desc &desc,
    aprend_shader vertex_shader,
    std::vector<aprend_vertex_attribute_plan> &plan) {
	const aprend_shader_reflection &reflection = vertex_shader->reflection.view;
	uint32_t position                          = 0;
	for (uint32_t binding = 0; binding < desc._vertex_binding_count; ++binding) {
		const aprend_vertex_layout &layout = desc._vertex_bindings[binding]._layout;
		for (uint32_t i = 0; i < layout.count; ++i) {
			const aprend_vertex_element &element = layout.elements[i];
			uint32_t location                    = position++;
			if (desc._vertex_inputs_by_name) {
				if (!element.name) {
					printf("apricot: %s: _vertex_inputs_by_name is set and element %u of _vertex_bindings[%u] has no name\n", who, i, binding);
					return false;
				}
				// The input of that name, and for a matrix the next of its
				// locations not yet given to an earlier element of the name.
				location = UINT32_MAX;
				for (uint32_t k = 0; k < reflection.input_count && location == UINT32_MAX; ++k) {
					const aprend_shader_input &input = reflection.inputs[k];
					if (strcmp(input.name, element.name) != 0)
						continue;
					uint32_t taken = 0;
					for (const aprend_vertex_attribute_plan &earlier : plan)
						if (earlier.location != UINT32_MAX && earlier.location >= input.location &&
						    earlier.location < input.location + input.location_count)
							++taken;
					if (taken < input.location_count)
						location = input.location + taken;
				}
			}
			plan.push_back({binding, &element, location});
		}
	}

	for (uint32_t k = 0; k < reflection.input_count; ++k) {
		const aprend_shader_input &input = reflection.inputs[k];
		if (!input.used)
			continue;
		const bool fed_kind = input.scalar_type == APREND_SHADER_SCALAR_TYPE_FLOAT || input.scalar_type == APREND_SHADER_SCALAR_TYPE_INT ||
		                      input.scalar_type == APREND_SHADER_SCALAR_TYPE_UINT;
		if (!fed_kind) {
			printf("apricot: %s: vertex input \"%s\" is read as a kind of number no vertex element type gives (float, int or uint)\n", who,
			       input.name);
			return false;
		}
		for (uint32_t location = input.location; location < input.location + input.location_count; ++location) {
			const aprend_vertex_attribute_plan *fed = nullptr;
			for (const aprend_vertex_attribute_plan &attribute : plan)
				if (attribute.location == location)
					fed = &attribute;
			if (!fed) {
				printf("apricot: %s: vertex input \"%s\" reads location %u, and no vertex element is given that location\n", who, input.name,
				       location);
				return false;
			}
			if (aprend_element_scalar_type(fed->element->type) != input.scalar_type) {
				printf("apricot: %s: vertex input \"%s\" at location %u is read as %s, and its element is %s\n", who, input.name, location,
				       aprend_scalar_type_name(input.scalar_type), aprend_scalar_type_name(aprend_element_scalar_type(fed->element->type)));
				return false;
			}
		}
	}
	return true;
}

bool aprend_binding_set_matches_images(
    const aprend_binding_set_t *set,
    uint32_t slot,
    const std::vector<aprend_image_expectation> &expected,
    const char *who) {
	for (uint32_t i = 0; i < set->entry_count; ++i) {
		const aprend_binding_set_entry &entry = set->entries[i];
		if (!aprend_descriptor_type_reads_view(entry._type))
			continue;
		for (const aprend_image_expectation &expectation : expected) {
			if (expectation.set != slot || expectation.binding != entry._binding)
				continue;
			const aprend_texture_view view = entry._resource._image._view;
			if (view->desc.shape != expectation.shape) {
				printf("aprend: %s: set %u binding %u element %u is a view of shape %u, and the shader declares shape %u there\n", who, slot,
				       entry._binding, entry._array_element, (unsigned)view->desc.shape, (unsigned)expectation.shape);
				return false;
			}
			const bool stencil = view->desc.aspect == APREND_TEXTURE_VIEW_ASPECT_STENCIL;
			if (stencil && expectation.scalar_type != APREND_SHADER_SCALAR_TYPE_UINT) {
				printf("aprend: %s: set %u binding %u element %u is a stencil, and the shader reads it as other than an unsigned integer\n", who,
				       slot, entry._binding, entry._array_element);
				return false;
			}
			if (view->desc.aspect == APREND_TEXTURE_VIEW_ASPECT_DEPTH && expectation.scalar_type != APREND_SHADER_SCALAR_TYPE_FLOAT) {
				printf("aprend: %s: set %u binding %u element %u is a depth, and the shader reads it as other than a float\n", who, slot,
				       entry._binding, entry._array_element);
				return false;
			}
			const SPUDGPU_FORMAT format = aprend_texture_view_format(view);
			// What a read gives follows the format's kind of number on every
			// API: a float or normalized one reads as float, an integer one as
			// its own. Read as another, the result is undefined on Vulkan and
			// D3D12 and refused by Metal's validation.
			const APREND_SHADER_SCALAR_TYPE reads = aprend_format_scalar_type(format);
			if (view->desc.aspect == APREND_TEXTURE_VIEW_ASPECT_COLOR && reads != APREND_SHADER_SCALAR_TYPE_NONE &&
			    expectation.scalar_type != reads) {
				printf("aprend: %s: set %u binding %u element %u is a view of format %u, which reads as %s, and the shader reads it as %s\n", who,
				       slot, entry._binding, entry._array_element, (unsigned)format, aprend_scalar_type_name(reads),
				       aprend_scalar_type_name(expectation.scalar_type));
				return false;
			}
			if (expectation.format != SPUDGPU_FORMAT_UNKNOWN && format != expectation.format) {
				printf("aprend: %s: set %u binding %u element %u is a storage image view of format %u, and the shader declares format %u there\n",
				       who, slot, entry._binding, entry._array_element, (unsigned)format, (unsigned)expectation.format);
				return false;
			}
		}
	}
	return true;
}

// The core of [binding_count] [bindings], checked by the caller: the
// instance's core of the same bindings if it has one, else a new one with a
// SpudGPU layout of them. Held once more for the caller either way. NULL,
// printed for [who], if a new one can't be made.
static aprend_binding_layout_core *aprend_binding_layout_core_acquire(
    aprend_instance instance,
    const aprend_binding_desc *bindings,
    uint32_t binding_count,
    const char *who) {
	// What makes two layouts one: each binding's number, type, count and
	// stages, in binding order. Names are each handle's own.
	aprend_binding_desc key[APREND_MAX_BINDINGS_PER_LAYOUT]{};
	for (uint32_t i = 0; i < binding_count; ++i) {
		key[i]       = bindings[i];
		key[i]._name = nullptr;
	}
	std::sort(key, key + binding_count, [](const aprend_binding_desc &a, const aprend_binding_desc &b) { return a._binding < b._binding; });

	for (aprend_binding_layout_core *core : instance->binding_layout_cores) {
		if (core->binding_count != binding_count)
			continue;
		bool same = true;
		for (uint32_t i = 0; i < binding_count && same; ++i) {
			const aprend_binding_desc &a = core->bindings[i];
			const aprend_binding_desc &b = key[i];
			same = a._binding == b._binding && a._type == b._type && a._count == b._count && a._stages == b._stages;
		}
		if (same) {
			++core->holders;
			return core;
		}
	}

	aprend_binding_layout_core *core = (aprend_binding_layout_core *)calloc(1, sizeof(aprend_binding_layout_core));
	if (!core) {
		printf("apricot: %s: out of memory\n", who);
		return nullptr;
	}
	core->instance      = instance;
	core->binding_count = binding_count;
	core->holders       = 1;
	spudgpu_descriptor_set_layout_desc ld{};
	ld.base.type = SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC;
	for (uint32_t i = 0; i < binding_count; ++i) {
		core->bindings[i]              = key[i];
		ld.bindings[i].binding         = key[i]._binding;
		ld.bindings[i].descriptor_type = key[i]._type;
		ld.bindings[i].count           = key[i]._count;
		ld.bindings[i].stage_flags     = key[i]._stages;
	}
	ld.binding_count = binding_count;
	SPUDRESULT sr    = spudgpu_create_descriptor_set_layout(instance->desc.device, &ld, &core->layout);
	if (SPUDFAIL(sr)) {
		printf("apricot: %s failed: %s\n", who, spudresult_str(sr));
		free(core);
		return nullptr;
	}
	try {
		instance->binding_layout_cores.push_back(core);
	} catch (const std::bad_alloc &) {
		printf("apricot: %s: out of memory\n", who);
		spudgpu_destroy_descriptor_set_layout(core->layout);
		free(core);
		return nullptr;
	}
	return core;
}

// Creates a layout of [binding_count] [bindings], checked by the caller,
// with a copy of each binding's name. NULL, printed for [who], if it can't be
// created.
static aprend_binding_layout_t *aprend_binding_layout_build(
    aprend_instance instance,
    const aprend_binding_desc *bindings,
    uint32_t binding_count,
    const char *who) {
	APREND_MALLOC__T(result, aprend_binding_layout_t);
	if (!result)
		return nullptr;
	APREND_CONSTRUCT__T(result, aprend_binding_layout_t);
	result->instance      = instance;
	result->binding_count = binding_count;

	try {
		for (uint32_t i = 0; i < binding_count; ++i) {
			result->bindings[i] = bindings[i];
			result->names[i]    = bindings[i]._name ? bindings[i]._name : "";
			// The layout's own copy, never the caller's string.
			result->bindings[i]._name = result->names[i].empty() ? nullptr : result->names[i].c_str();
			result->descriptor_count += bindings[i]._count;
		}
	} catch (const std::bad_alloc &) {
		printf("apricot: %s: out of memory\n", who);
		APREND_DESTRUCT__T(result, aprend_binding_layout_t);
		return nullptr;
	}

	result->core = aprend_binding_layout_core_acquire(instance, bindings, binding_count, who);
	if (!result->core) {
		APREND_DESTRUCT__T(result, aprend_binding_layout_t);
		return nullptr;
	}
	return result;
}

extern "C" {

aprend_shader aprend_shader_create_spirv(
    aprend_instance instance,
    const void *spirv_code,
    uint64_t spirv_size,
    SPUDGPU_SHADER_STAGE shader_stage) {
	if (!instance || !spirv_code || !spirv_size || spirv_size % 4 != 0)
		return nullptr;
	if (!aprend_shader_stage_supported(shader_stage))
		return nullptr;
	if (!aprend_shader_stage_feature_ok(instance, shader_stage, "aprend_shader_create_spirv"))
		return nullptr;

	APREND_MALLOC__T(result, aprend_shader_t);
	if (!result)
		return nullptr;
	APREND_CONSTRUCT__T(result, aprend_shader_t);
	result->instance = instance;

	// Reflected first: SPIR-V that can't be read isn't handed to the device.
	if (!aprend_shader_reflect(result, spirv_code, spirv_size, shader_stage, "aprend_shader_create_spirv")) {
		APREND_DESTRUCT__T(result, aprend_shader_t);
		return nullptr;
	}
	SPUDRESULT sr = aprend_shader_create_module(result, spirv_code, spirv_size, shader_stage, "aprend_shader_create_spirv");
	if (SPUDFAIL(sr)) {
		printf("apricot: aprend_shader_create_spirv failed: %s\n", spudresult_str(sr));
		APREND_DESTRUCT__T(result, aprend_shader_t);
		return nullptr;
	}
	return result;
}

/* Reads the whole file at [filename] into a malloc'd block. Its SpudFiles
 * instance is over the system heap and lives for the call: Aprend keeps no
 * files open. The file is shared with every other reader, writer and
 * rename, since nothing here minds them. SPUDRESULT_SFS_NOT_FOUND when no
 * file is there; a size that isn't whole SPIR-V words is
 * SPUDRESULT_DESC_INVALID_PARAMETERS, after printing why. */
static SPUDRESULT aprend_read_spirv_file(const char *filename, uint8_t **out_data, uint64_t *out_size) {
	*out_data = nullptr;
	*out_size = 0;
	const SPUD_ALLOCATOR allocator = spud_allocator_system();
	sfs_instance_desc files_desc   = {};
	files_desc.base.type           = SPUD_STRUCT_TYPE_FILES_INSTANCE_DESC;
	files_desc.allocator           = &allocator;
	sfs_instance files             = nullptr;
	SPUDRESULT sr                  = sfs_instance_create(&files_desc, &files);
	if (SPUDFAIL(sr))
		return sr;

	sfs_open_desc open_desc = {};
	open_desc.base.type     = SPUD_STRUCT_TYPE_FILES_OPEN_DESC;
	open_desc.path          = filename;
	open_desc.access        = SFS_ACCESS_READ;
	open_desc.create        = SFS_CREATE_OPEN_EXISTING;
	open_desc.share         = SFS_SHARE_READ | SFS_SHARE_WRITE | SFS_SHARE_DELETE;
	sfs_file file           = nullptr;
	sr                      = sfs_file_open(files, &open_desc, &file);
	uint64_t size           = 0;
	if (!SPUDFAIL(sr))
		sr = sfs_file_get_size(file, &size);
	if (!SPUDFAIL(sr) && (size == 0 || size % 4 != 0)) {
		printf("apricot: aprend_shader_read_from_file_spirv: '%s' is %llu bytes, not a whole number of SPIR-V words\n", filename,
		       (unsigned long long)size);
		sr = SPUDRESULT_DESC_INVALID_PARAMETERS;
	}
	uint8_t *data = nullptr;
	if (!SPUDFAIL(sr)) {
		data = (uint8_t *)malloc(size);
		if (!data)
			sr = SPUDRESULT_OUT_OF_MEMORY;
	}
	if (!SPUDFAIL(sr)) {
		uint64_t read = 0;
		sr            = sfs_file_read_at(file, 0, data, size, &read);
		if (!SPUDFAIL(sr) && read != size)
			sr = SPUDRESULT_API_SPECIFIC_FAILURE; // shrunk while being read
	}
	sfs_file_close(file);
	sfs_instance_destroy(files);
	if (SPUDFAIL(sr)) {
		free(data);
		return sr;
	}
	*out_data = data;
	*out_size = size;
	return SPUD_SUCCESS;
}

aprend_shader aprend_shader_read_from_file_spirv(
    aprend_instance instance,
    const char *filename,
    SPUDGPU_SHADER_STAGE shader_stage) {
	if (!instance || !filename)
		return nullptr;
	if (!aprend_shader_stage_supported(shader_stage))
		return nullptr;
	if (!aprend_shader_stage_feature_ok(instance, shader_stage, "aprend_shader_read_from_file_spirv"))
		return nullptr;

	uint8_t *data      = nullptr;
	uint64_t data_size = 0;
	SPUDRESULT sr      = aprend_read_spirv_file(filename, &data, &data_size);
	if (sr == SPUDRESULT_SFS_NOT_FOUND || sr == SPUDRESULT_DESC_INVALID_PARAMETERS)
		return nullptr;

	APREND_MALLOC__T(result, aprend_shader_t);
	if (!result) {
		free(data);
		return nullptr;
	}
	APREND_CONSTRUCT__T(result, aprend_shader_t);
	result->instance = instance;
	if (SPUDFAIL(sr))
		goto failedattempt;

	if (!aprend_shader_reflect(result, data, data_size, shader_stage, filename)) {
		free(data);
		APREND_DESTRUCT__T(result, aprend_shader_t);
		return nullptr;
	}
	// The raw file bytes aren't needed once SpudGPU has its own copy.
	sr = aprend_shader_create_module(result, data, data_size, shader_stage, filename);
	free(data);
	if (SPUDFAIL(sr))
		goto failedattempt;

	return result;
failedattempt:
	printf("apricot: aprend_shader_read_from_file_spirv failed ('%s'): %s\n", filename, spudresult_str(sr));
	APREND_DESTRUCT__T(result, aprend_shader_t);
	return nullptr;
}
void aprend_shader_destroy(aprend_shader shader) {
	if (!shader)
		return;
	// Released once the GPU has finished everything submitted so far.
	aprend_instance_retire(shader->instance, shader, &aprend_release_handle<aprend_shader_t>);
}

const aprend_shader_reflection *aprend_shader_get_reflection(aprend_shader shader) { return shader ? &shader->reflection.view : nullptr; }

const aprend_shader_resource *aprend_shader_find_resource(aprend_shader shader, const char *name) {
	if (!shader)
		return nullptr;
	if (!name)
		return nullptr;
	for (const aprend_shader_resource &resource : shader->reflection.resources) {
		if (strcmp(resource.name, name) == 0)
			return &resource;
		if (resource.block_name && strcmp(resource.block_name, name) == 0)
			return &resource;
	}
	return nullptr;
}

SPUDGPU_SHADER_STAGE aprend_shader_get_stage(aprend_shader shader) { return shader ? shader->stage : (SPUDGPU_SHADER_STAGE)SPUDGPU_SHADER_STAGE_NONE; }

spudgpu_blend_attachment_desc aprend_blend_premultiplied(void) {
	spudgpu_blend_attachment_desc blend{};
	blend.blend_enable           = true;
	blend.src_color_blend_factor = SPUDGPU_BLEND_FACTOR_ONE;
	blend.dst_color_blend_factor = SPUDGPU_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blend.color_blend_op         = SPUDGPU_BLEND_OP_ADD;
	blend.src_alpha_blend_factor = SPUDGPU_BLEND_FACTOR_ONE;
	blend.dst_alpha_blend_factor = SPUDGPU_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blend.alpha_blend_op         = SPUDGPU_BLEND_OP_ADD;
	return blend;
}
spudgpu_blend_attachment_desc aprend_blend_alpha(void) {
	spudgpu_blend_attachment_desc blend = aprend_blend_premultiplied();
	blend.src_color_blend_factor        = SPUDGPU_BLEND_FACTOR_SRC_ALPHA;
	return blend;
}
spudgpu_blend_attachment_desc aprend_blend_additive(void) {
	spudgpu_blend_attachment_desc blend = aprend_blend_premultiplied();
	blend.dst_color_blend_factor        = SPUDGPU_BLEND_FACTOR_ONE;
	blend.dst_alpha_blend_factor        = SPUDGPU_BLEND_FACTOR_ONE;
	return blend;
}

aprend_graphics_pipeline aprend_graphics_pipeline_create(
    aprend_instance instance,
    const aprend_graphics_pipeline_desc *desc_in) {
	if (!instance)
		return nullptr;
	if (!desc_in)
		return nullptr;
	if (!aprend_base_ok("aprend_graphics_pipeline_create", "desc", &desc_in->base, APRI_STRUCT_TYPE_APREND_GRAPHICS_PIPELINE_DESC))
		return nullptr;
	aprend_graphics_pipeline_desc desc = *desc_in;
	if (!desc.fragment_shader)
		return nullptr;
	if (desc.fragment_shader->stage != SPUDGPU_SHADER_STAGE_FRAGMENT) {
		printf("apricot: aprend_graphics_pipeline_create: fragment_shader was not created for the fragment stage\n");
		return nullptr;
	}
	if (desc.fragment_shader->instance != instance) {
		printf("apricot: aprend_graphics_pipeline_create: fragment_shader belongs to another instance\n");
		return nullptr;
	}
	// The geometry comes from a vertex shader or a mesh shader, never both.
	if (desc.mesh_shader) {
		if (desc.vertex_shader) {
			printf("apricot: aprend_graphics_pipeline_create: both vertex_shader and mesh_shader are given\n");
			return nullptr;
		}
		if (desc.mesh_shader->stage != SPUDGPU_SHADER_STAGE_MESH) {
			printf("apricot: aprend_graphics_pipeline_create: mesh_shader was not created for the mesh stage\n");
			return nullptr;
		}
		if (desc.mesh_shader->instance != instance) {
			printf("apricot: aprend_graphics_pipeline_create: mesh_shader belongs to another instance\n");
			return nullptr;
		}
		if (!desc._mesh_entry_point) {
			printf("apricot: aprend_graphics_pipeline_create: _mesh_entry_point is NULL\n");
			return nullptr;
		}
		if (!desc._mesh_entry_point[0]) {
			printf("apricot: aprend_graphics_pipeline_create: _mesh_entry_point is an empty string\n");
			return nullptr;
		}
		if (desc._vertex_binding_count != 0) {
			printf("apricot: aprend_graphics_pipeline_create: a mesh shader pipeline has no vertex input, and _vertex_binding_count is %u\n",
			       desc._vertex_binding_count);
			return nullptr;
		}
		if (!(instance->features & SPUDGPU_FEATURE_MESH_SHADING)) {
			printf("apricot: aprend_graphics_pipeline_create: mesh_shader is given and the device was created without SPUDGPU_FEATURE_MESH_SHADING\n");
			return nullptr;
		}
	} else {
		if (!desc.vertex_shader) {
			printf("apricot: aprend_graphics_pipeline_create: neither vertex_shader nor mesh_shader is given\n");
			return nullptr;
		}
		if (desc.vertex_shader->stage != SPUDGPU_SHADER_STAGE_VERTEX) {
			printf("apricot: aprend_graphics_pipeline_create: vertex_shader was not created for the vertex stage\n");
			return nullptr;
		}
		if (desc.vertex_shader->instance != instance) {
			printf("apricot: aprend_graphics_pipeline_create: vertex_shader belongs to another instance\n");
			return nullptr;
		}
		if (!desc._vertex_entry_point) {
			printf("apricot: aprend_graphics_pipeline_create: _vertex_entry_point is NULL\n");
			return nullptr;
		}
		if (!desc._vertex_entry_point[0]) {
			printf("apricot: aprend_graphics_pipeline_create: _vertex_entry_point is an empty string\n");
			return nullptr;
		}
	}
	if (!desc._fragment_entry_point) {
		printf("apricot: aprend_graphics_pipeline_create: _fragment_entry_point is NULL\n");
		return nullptr;
	}
	if (!desc._fragment_entry_point[0]) {
		printf("apricot: aprend_graphics_pipeline_create: _fragment_entry_point is an empty string\n");
		return nullptr;
	}
	if (desc._vertex_binding_count > APREND_MAX_VERTEX_BINDINGS) {
		printf("apricot: aprend_graphics_pipeline_create: _vertex_binding_count %u is above APREND_MAX_VERTEX_BINDINGS\n", desc._vertex_binding_count);
		return nullptr;
	}
	uint32_t vertex_attribute_count = 0;
	for (uint32_t i = 0; i < desc._vertex_binding_count; ++i) {
		const aprend_vertex_layout &layout = desc._vertex_bindings[i]._layout;
		if (layout.count == 0) {
			printf("apricot: aprend_graphics_pipeline_create: _vertex_bindings[%u] has no elements\n", i);
			return nullptr;
		}
		if (!layout.elements) {
			printf("apricot: aprend_graphics_pipeline_create: _vertex_bindings[%u] has a NULL elements array\n", i);
			return nullptr;
		}
		if (layout.stride == 0) {
			printf("apricot: aprend_graphics_pipeline_create: _vertex_bindings[%u] has a stride of 0\n", i);
			return nullptr;
		}
		// Every format is 32-bit, which each API reads from 4-byte offsets.
		if (layout.stride % 4 != 0) {
			printf("apricot: aprend_graphics_pipeline_create: _vertex_bindings[%u] has a stride of %u, not a multiple of 4\n", i, layout.stride);
			return nullptr;
		}
		for (uint32_t e = 0; e < layout.count; ++e) {
			const aprend_vertex_element &element = layout.elements[e];
			const uint32_t size                  = aprend_vertex_element_type_get_size(element.type);
			if (size == 0) {
				printf("apricot: aprend_graphics_pipeline_create: element %u of _vertex_bindings[%u] has type %u, which is not an "
				       "APREND_VERTEX_ELEMENT_TYPE\n",
				       e, i, (unsigned)element.type);
				return nullptr;
			}
			if (element.offset % 4 != 0) {
				printf("apricot: aprend_graphics_pipeline_create: element %u of _vertex_bindings[%u] has an offset of %u, not a multiple of 4\n", e,
				       i, element.offset);
				return nullptr;
			}
			if ((uint64_t)element.offset + size > layout.stride) {
				printf("apricot: aprend_graphics_pipeline_create: element %u of _vertex_bindings[%u] ends at %llu, past the stride of %u\n", e, i,
				       (unsigned long long)element.offset + size, layout.stride);
				return nullptr;
			}
		}
		if (layout.count > SPUDGPU_MAX_VERTEX_ATTRIBUTES - vertex_attribute_count) {
			printf("apricot: aprend_graphics_pipeline_create: the vertex bindings hold more than SPUDGPU_MAX_VERTEX_ATTRIBUTES elements\n");
			return nullptr;
		}
		vertex_attribute_count += layout.count;
	}
	if (desc._push_constant_range_count > APREND_MAX_PUSH_CONSTANT_RANGES) {
		printf("apricot: aprend_graphics_pipeline_create: _push_constant_range_count %u is above APREND_MAX_PUSH_CONSTANT_RANGES\n",
		       desc._push_constant_range_count);
		return nullptr;
	}
	for (uint32_t i = 0; i < desc._push_constant_range_count; ++i) {
		const spudgpu_push_constant_range_desc &range = desc._push_constant_ranges[i];
		if (range.stage_flags == SPUDGPU_SHADER_STAGE_NONE) {
			printf("apricot: aprend_graphics_pipeline_create: _push_constant_ranges[%u] has no stages\n", i);
			return nullptr;
		}
		if (range.size == 0) {
			printf("apricot: aprend_graphics_pipeline_create: _push_constant_ranges[%u] has a size of 0\n", i);
			return nullptr;
		}
		if (range.offset % 4 != 0) {
			printf("apricot: aprend_graphics_pipeline_create: _push_constant_ranges[%u] has an offset that is not a multiple of 4\n", i);
			return nullptr;
		}
		if (range.size % 4 != 0) {
			printf("apricot: aprend_graphics_pipeline_create: _push_constant_ranges[%u] has a size that is not a multiple of 4\n", i);
			return nullptr;
		}
		if (range.size > UINT32_MAX - range.offset) {
			printf("apricot: aprend_graphics_pipeline_create: _push_constant_ranges[%u] runs past what 32 bits can address\n", i);
			return nullptr;
		}
	}
	if (desc._cull_mode > SPUDGPU_CULL_MODE_BACK) {
		printf("apricot: aprend_graphics_pipeline_create: _cull_mode %u is not a SPUDGPU_CULL_MODE\n", (unsigned)desc._cull_mode);
		return nullptr;
	}
	if (desc._depth_test && desc._depth_compare_op == SPUDGPU_COMPARE_OP_NEVER) {
		// NEVER would discard every fragment - almost certainly an unset field.
		printf("apricot: aprend_graphics_pipeline_create: _depth_test is set but _depth_compare_op is NEVER\n");
		return nullptr;
	}
	if (desc._depth_bias_clamp != 0.0f && !(instance->features & SPUDGPU_FEATURE_DEPTH_BIAS_CLAMP)) {
		printf("apricot: aprend_graphics_pipeline_create: _depth_bias_clamp is %g and the device was created without SPUDGPU_FEATURE_DEPTH_BIAS_CLAMP\n",
		       (double)desc._depth_bias_clamp);
		return nullptr;
	}
	if (desc.color_attachment_count > APREND_MAX_COLOR_TARGETS) {
		printf("apricot: aprend_graphics_pipeline_create: color_attachment_count %u is above APREND_MAX_COLOR_TARGETS\n", desc.color_attachment_count);
		return nullptr;
	}
	for (uint32_t i = 0; i < desc.color_attachment_count; ++i) {
		if (desc.color_attachment_formats[i] == SPUDGPU_FORMAT_UNKNOWN) {
			printf("apricot: aprend_graphics_pipeline_create: color_attachment_formats[%u] is SPUDGPU_FORMAT_UNKNOWN\n", i);
			return nullptr;
		}
	}
	if (!(instance->features & SPUDGPU_FEATURE_INDEPENDENT_BLEND)) {
		for (uint32_t i = 1; i < desc.color_attachment_count; ++i) {
			if (!aprend_blends_equal(desc._blends[0], desc._blends[i])) {
				printf("apricot: aprend_graphics_pipeline_create: _blends[%u] differs from _blends[0] and the device was created without "
				       "SPUDGPU_FEATURE_INDEPENDENT_BLEND\n",
				       i);
				return nullptr;
			}
		}
	}
	if (desc._binding_layout_count > APREND_MAX_BINDING_LAYOUTS) {
		printf("apricot: aprend_graphics_pipeline_create: _binding_layout_count %u is above APREND_MAX_BINDING_LAYOUTS\n", desc._binding_layout_count);
		return nullptr;
	}
	for (uint32_t i = 0; i < desc._binding_layout_count; ++i) {
		if (!desc._binding_layouts[i]) {
			printf("apricot: aprend_graphics_pipeline_create: _binding_layouts[%u] is NULL\n", i);
			return nullptr;
		}
		if (desc._binding_layouts[i]->instance != instance) {
			printf("apricot: aprend_graphics_pipeline_create: _binding_layouts[%u] belongs to another instance\n", i);
			return nullptr;
		}
	}

	APREND_MALLOC__T(result, aprend_graphics_pipeline_t);
	if (!result)
		return nullptr;
	APREND_CONSTRUCT__T(result, aprend_graphics_pipeline_t);

	result->desc     = desc;
	result->instance = instance;

	// The vertex layouts' elements and their names, copied into one block:
	// every binding's elements, then every name. The desc then points at the
	// copies, so aprend_graphics_pipeline_get_desc never hands back the
	// caller's memory, and everything below reads them.
	{
		size_t element_count = 0;
		size_t name_bytes    = 0;
		for (uint32_t i = 0; i < desc._vertex_binding_count; ++i) {
			const aprend_vertex_layout &layout = desc._vertex_bindings[i]._layout;
			element_count += layout.count;
			for (uint32_t e = 0; e < layout.count; ++e)
				if (layout.elements[e].name)
					name_bytes += strlen(layout.elements[e].name) + 1;
		}
		if (element_count > 0) {
			result->vertex_elements = (char *)malloc(element_count * sizeof(aprend_vertex_element) + name_bytes);
			if (!result->vertex_elements) {
				printf("apricot: aprend_graphics_pipeline_create: out of memory\n");
				APREND_DESTRUCT__T(result, aprend_graphics_pipeline_t);
				return nullptr;
			}
			aprend_vertex_element *elements = (aprend_vertex_element *)result->vertex_elements;
			char *names                     = result->vertex_elements + element_count * sizeof(aprend_vertex_element);
			for (uint32_t i = 0; i < desc._vertex_binding_count; ++i) {
				aprend_vertex_layout &layout = result->desc._vertex_bindings[i]._layout;
				memcpy(elements, layout.elements, layout.count * sizeof(aprend_vertex_element));
				for (uint32_t e = 0; e < layout.count; ++e) {
					if (!elements[e].name)
						continue;
					const size_t length = strlen(elements[e].name) + 1;
					memcpy(names, elements[e].name, length);
					elements[e].name = names;
					names += length;
				}
				layout.elements = elements;
				elements += layout.count;
			}
		}
	}

	// The desc against what the shaders declare, and the locations each
	// vertex element is read at.
	std::vector<aprend_vertex_attribute_plan> plan;
	try {
		static const char who[]     = "aprend_graphics_pipeline_create";
		aprend_shader geometry      = desc.mesh_shader ? desc.mesh_shader : desc.vertex_shader;
		const char *geometry_entry  = desc.mesh_shader ? desc._mesh_entry_point : desc._vertex_entry_point;
		bool ok = aprend_pipeline_check_shader(who, geometry, geometry_entry, desc._binding_layouts, desc._binding_layout_count,
		                                       desc._push_constant_ranges, desc._push_constant_range_count, result->images,
		                                       result->storage_writes) &&
		          aprend_pipeline_check_shader(who, desc.fragment_shader, desc._fragment_entry_point, desc._binding_layouts,
		                                       desc._binding_layout_count, desc._push_constant_ranges, desc._push_constant_range_count,
		                                       result->images, result->storage_writes);
		if (ok)
			ok = aprend_pipeline_check_outputs(who, desc);
		if (ok && !desc.mesh_shader)
			ok = aprend_pipeline_plan_vertex_input(who, result->desc, desc.vertex_shader, plan);
		if (!ok) {
			APREND_DESTRUCT__T(result, aprend_graphics_pipeline_t);
			return nullptr;
		}
	} catch (const std::bad_alloc &) {
		printf("apricot: aprend_graphics_pipeline_create: out of memory\n");
		APREND_DESTRUCT__T(result, aprend_graphics_pipeline_t);
		return nullptr;
	}

	// The names are copied, so the desc aprend_graphics_pipeline_get_desc
	// hands back never points at a string the caller has released.
	// The first name is the vertex shader's, or the mesh shader's in a mesh
	// shader pipeline; the other of the two is not read and is left as the
	// caller gave it.
	result->is_mesh                  = desc.mesh_shader != nullptr;
	const char *geometry_entry       = result->is_mesh ? desc._mesh_entry_point : desc._vertex_entry_point;
	const size_t vertex_entry_size   = strlen(geometry_entry) + 1;
	const size_t fragment_entry_size = strlen(desc._fragment_entry_point) + 1;
	result->entry_points             = (char *)malloc(vertex_entry_size + fragment_entry_size);
	if (!result->entry_points) {
		APREND_DESTRUCT__T(result, aprend_graphics_pipeline_t);
		return nullptr;
	}
	memcpy(result->entry_points, geometry_entry, vertex_entry_size);
	memcpy(result->entry_points + vertex_entry_size, desc._fragment_entry_point, fragment_entry_size);
	result->desc._fragment_entry_point = result->entry_points + vertex_entry_size;

	spudgpu_shader_pipeline_desc pd{};
	pd.base.type = SPUD_STRUCT_TYPE_GPU_SHADER_PIPELINE_DESC;
	if (result->is_mesh) {
		result->desc._mesh_entry_point   = result->entry_points;
		result->desc._vertex_entry_point = nullptr;
		pd.mesh_module                   = desc.mesh_shader->shader_module;
		pd.mesh_entry_point              = result->entry_points;
	} else {
		result->desc._vertex_entry_point = result->entry_points;
		result->desc._mesh_entry_point   = nullptr;
		pd.vertex_module                 = desc.vertex_shader->shader_module;
		pd.vertex_entry_point            = result->entry_points;
	}
	pd.fragment_module      = desc.fragment_shader->shader_module;
	pd.fragment_entry_point = result->desc._fragment_entry_point;

	for (uint32_t binding = 0; binding < desc._vertex_binding_count; ++binding) {
		const aprend_vertex_binding_desc &vb = result->desc._vertex_bindings[binding];
		result->vertex_strides[binding]      = vb._layout.stride;
		for (uint32_t e = 0; e < vb._layout.count; ++e) {
			const aprend_vertex_element &element = vb._layout.elements[e];
			const uint32_t end                   = element.offset + aprend_vertex_element_type_get_size(element.type);
			if (end > result->vertex_extents[binding])
				result->vertex_extents[binding] = end;
		}

		pd.vertex_bindings[binding].binding      = binding;
		pd.vertex_bindings[binding].stride       = result->vertex_strides[binding];
		pd.vertex_bindings[binding].per_instance = vb._per_instance;
	}
	pd.vertex_binding_count = desc._vertex_binding_count;
	// At the locations the plan gives them: by position, running on through
	// the bindings in order, or by name. An element the shader doesn't read
	// by name is no attribute.
	for (const aprend_vertex_attribute_plan &planned : plan) {
		if (planned.location == UINT32_MAX)
			continue;
		spudgpu_vertex_attribute_desc &attribute = pd.vertex_attributes[pd.vertex_attribute_count];
		attribute.location                       = planned.location;
		attribute.binding                        = planned.binding;
		attribute.format                         = aprend_vertex_element_type_to_format(planned.element->type);
		attribute.offset                         = planned.element->offset;
		++pd.vertex_attribute_count;
	}

	for (uint32_t i = 0; i < desc._push_constant_range_count; ++i) {
		pd.push_constant_ranges[i] = desc._push_constant_ranges[i];
		const uint32_t range_end   = desc._push_constant_ranges[i].offset + desc._push_constant_ranges[i].size;
		if (range_end > result->push_constant_end)
			result->push_constant_end = range_end;
	}
	pd.push_constant_range_count = desc._push_constant_range_count;

	pd.primitive_topology = desc._topology;
	pd.cull_mode          = desc._cull_mode;
	pd.front_face_ccw     = desc._front_face_ccw;
	pd.wireframe          = desc._wireframe;
	pd.depth_test_enable  = desc._depth_test;
	pd.depth_write_enable = desc._depth_write;
	pd.depth_compare_op   = desc._depth_compare_op;
	pd.depth_bias_constant = desc._depth_bias_constant;
	pd.depth_bias_slope    = desc._depth_bias_slope;
	pd.depth_bias_clamp    = desc._depth_bias_clamp;

	for (uint32_t i = 0; i < desc.color_attachment_count; ++i) {
		pd.color_attachment_formats[i] = desc.color_attachment_formats[i];
		pd.blend_attachments[i]        = desc._blends[i];
	}
	pd.color_attachment_count = desc.color_attachment_count;
	pd.depth_format           = desc.depth_format;
	for (uint32_t i = 0; i < desc._binding_layout_count; ++i)
		pd.descriptor_set_layouts[i] = desc._binding_layouts[i]->core->layout;
	pd.descriptor_set_layout_count = desc._binding_layout_count;
	pd.debug_name = desc._debug_name;

	SPUDRESULT sr = spudgpu_create_shader_pipeline(instance->desc.device, &pd, &result->pipeline);
	if (SPUDFAIL(sr)) {
		printf("apricot: aprend_graphics_pipeline_create failed: %s\n", spudresult_str(sr));
		APREND_DESTRUCT__T(result, aprend_graphics_pipeline_t);
		return nullptr;
	}

	return result;
}
void aprend_graphics_pipeline_destroy(aprend_graphics_pipeline p) {
	if (!p)
		return;
	// A compiled list that binds it is refused at submit.
	aprend_instance_forget(p->instance, p);
	// Released once the GPU has finished everything submitted so far.
	aprend_instance_retire(p->instance, p, &aprend_release_handle<aprend_graphics_pipeline_t>);
}
aprend_graphics_pipeline_desc aprend_graphics_pipeline_get_desc(aprend_graphics_pipeline p) {
	if (!p)
		return aprend_graphics_pipeline_desc{};
	aprend_graphics_pipeline_desc desc = p->desc;
	desc.base = apri_struct_base(APRI_STRUCT_TYPE_APREND_GRAPHICS_PIPELINE_DESC);
	return desc;
}

uint32_t aprend_vertex_element_type_get_size(APREND_VERTEX_ELEMENT_TYPE type) {
	switch (type) {
	case APREND_VERTEX_ELEMENT_TYPE_FLOAT:
	case APREND_VERTEX_ELEMENT_TYPE_INT:
	case APREND_VERTEX_ELEMENT_TYPE_UINT:
		return 4;
	case APREND_VERTEX_ELEMENT_TYPE_VEC2:
	case APREND_VERTEX_ELEMENT_TYPE_INT2:
	case APREND_VERTEX_ELEMENT_TYPE_UINT2:
		return 8;
	case APREND_VERTEX_ELEMENT_TYPE_VEC3:
	case APREND_VERTEX_ELEMENT_TYPE_INT3:
	case APREND_VERTEX_ELEMENT_TYPE_UINT3:
		return 12;
	case APREND_VERTEX_ELEMENT_TYPE_VEC4:
	case APREND_VERTEX_ELEMENT_TYPE_INT4:
	case APREND_VERTEX_ELEMENT_TYPE_UINT4:
		return 16;
	default:
		return 0;
	}
}

uint32_t aprend_vertex_layout_get_element_index(const aprend_vertex_layout *layout, const char *name) {
	if (!layout)
		return UINT32_MAX;
	if (!layout->elements)
		return UINT32_MAX;
	if (!name)
		return UINT32_MAX;
	for (uint32_t i = 0; i < layout->count; ++i) {
		const char *element_name = layout->elements[i].name;
		if (element_name && strcmp(element_name, name) == 0)
			return i;
	}
	return UINT32_MAX;
}

aprend_compute_pipeline aprend_compute_pipeline_create(
    aprend_instance instance,
    const aprend_compute_pipeline_desc *desc_in) {
	if (!instance) {
		printf("apricot: aprend_compute_pipeline_create: instance is NULL\n");
		return nullptr;
	}
	if (!desc_in) {
		printf("apricot: aprend_compute_pipeline_create: desc is NULL\n");
		return nullptr;
	}
	if (!aprend_base_ok("aprend_compute_pipeline_create", "desc", &desc_in->base, APRI_STRUCT_TYPE_APREND_COMPUTE_PIPELINE_DESC))
		return nullptr;
	aprend_compute_pipeline_desc desc = *desc_in;
	if (!desc.compute_shader) {
		printf("apricot: aprend_compute_pipeline_create: compute_shader is NULL\n");
		return nullptr;
	}
	if (desc.compute_shader->instance != instance) {
		printf("apricot: aprend_compute_pipeline_create: compute_shader belongs to another instance\n");
		return nullptr;
	}
	if (desc.compute_shader->stage != SPUDGPU_SHADER_STAGE_COMPUTE) {
		printf("apricot: aprend_compute_pipeline_create: compute_shader was not created for the compute stage\n");
		return nullptr;
	}
	if (!desc._entry_point) {
		printf("apricot: aprend_compute_pipeline_create: _entry_point is NULL\n");
		return nullptr;
	}
	if (!desc._entry_point[0]) {
		printf("apricot: aprend_compute_pipeline_create: _entry_point is an empty string\n");
		return nullptr;
	}
	if (desc._binding_layout_count > APREND_MAX_BINDING_LAYOUTS) {
		printf("apricot: aprend_compute_pipeline_create: _binding_layout_count %u is above APREND_MAX_BINDING_LAYOUTS\n", desc._binding_layout_count);
		return nullptr;
	}
	for (uint32_t i = 0; i < desc._binding_layout_count; ++i) {
		if (!desc._binding_layouts[i]) {
			printf("apricot: aprend_compute_pipeline_create: _binding_layouts[%u] is NULL\n", i);
			return nullptr;
		}
		if (desc._binding_layouts[i]->instance != instance) {
			printf("apricot: aprend_compute_pipeline_create: _binding_layouts[%u] belongs to another instance\n", i);
			return nullptr;
		}
	}
	if (desc._push_constant_range_count > APREND_MAX_PUSH_CONSTANT_RANGES) {
		printf("apricot: aprend_compute_pipeline_create: _push_constant_range_count %u is above APREND_MAX_PUSH_CONSTANT_RANGES\n",
		       desc._push_constant_range_count);
		return nullptr;
	}
	for (uint32_t i = 0; i < desc._push_constant_range_count; ++i) {
		const spudgpu_push_constant_range_desc &range = desc._push_constant_ranges[i];
		if (range.stage_flags != SPUDGPU_SHADER_STAGE_COMPUTE) {
			printf("apricot: aprend_compute_pipeline_create: _push_constant_ranges[%u] has a stage other than compute alone\n", i);
			return nullptr;
		}
		if (range.size == 0) {
			printf("apricot: aprend_compute_pipeline_create: _push_constant_ranges[%u] has a size of 0\n", i);
			return nullptr;
		}
		if (range.offset % 4 != 0) {
			printf("apricot: aprend_compute_pipeline_create: _push_constant_ranges[%u] has an offset that is not a multiple of 4\n", i);
			return nullptr;
		}
		if (range.size % 4 != 0) {
			printf("apricot: aprend_compute_pipeline_create: _push_constant_ranges[%u] has a size that is not a multiple of 4\n", i);
			return nullptr;
		}
		if (range.size > UINT32_MAX - range.offset) {
			printf("apricot: aprend_compute_pipeline_create: _push_constant_ranges[%u] runs past what 32 bits can address\n", i);
			return nullptr;
		}
	}

	APREND_MALLOC__T(result, aprend_compute_pipeline_t);
	if (!result)
		return nullptr;
	APREND_CONSTRUCT__T(result, aprend_compute_pipeline_t);

	result->desc     = desc;
	result->instance = instance;

	try {
		if (!aprend_pipeline_check_shader("aprend_compute_pipeline_create", desc.compute_shader, desc._entry_point, desc._binding_layouts,
		                                  desc._binding_layout_count, desc._push_constant_ranges, desc._push_constant_range_count,
		                                  result->images, result->storage_writes)) {
			APREND_DESTRUCT__T(result, aprend_compute_pipeline_t);
			return nullptr;
		}
	} catch (const std::bad_alloc &) {
		printf("apricot: aprend_compute_pipeline_create: out of memory\n");
		APREND_DESTRUCT__T(result, aprend_compute_pipeline_t);
		return nullptr;
	}

	// The name is copied, so the desc aprend_compute_pipeline_get_desc hands
	// back never points at a string the caller has released.
	const size_t entry_size = strlen(desc._entry_point) + 1;
	result->entry_point     = (char *)malloc(entry_size);
	if (!result->entry_point) {
		APREND_DESTRUCT__T(result, aprend_compute_pipeline_t);
		return nullptr;
	}
	memcpy(result->entry_point, desc._entry_point, entry_size);
	result->desc._entry_point = result->entry_point;

	spudgpu_compute_pipeline_desc pd{};
	pd.base.type = SPUD_STRUCT_TYPE_GPU_COMPUTE_PIPELINE_DESC;
	pd.compute_module      = desc.compute_shader->shader_module;
	pd.compute_entry_point = result->entry_point;
	for (uint32_t i = 0; i < desc._binding_layout_count; ++i)
		pd.descriptor_set_layouts[i] = desc._binding_layouts[i]->core->layout;
	pd.descriptor_set_layout_count = desc._binding_layout_count;
	for (uint32_t i = 0; i < desc._push_constant_range_count; ++i) {
		pd.push_constant_ranges[i] = desc._push_constant_ranges[i];
		const uint32_t range_end   = desc._push_constant_ranges[i].offset + desc._push_constant_ranges[i].size;
		if (range_end > result->push_constant_end)
			result->push_constant_end = range_end;
	}
	pd.push_constant_range_count = desc._push_constant_range_count;
	pd.debug_name = desc._debug_name;

	SPUDRESULT sr = spudgpu_create_compute_pipeline(instance->desc.device, &pd, &result->pipeline);
	if (SPUDFAIL(sr)) {
		printf("apricot: aprend_compute_pipeline_create failed: %s\n", spudresult_str(sr));
		result->pipeline = nullptr;
		APREND_DESTRUCT__T(result, aprend_compute_pipeline_t);
		return nullptr;
	}

	return result;
}
void aprend_compute_pipeline_destroy(aprend_compute_pipeline p) {
	if (!p)
		return;
	// A compiled list that binds it is refused at submit.
	aprend_instance_forget(p->instance, p);
	// Released once the GPU has finished everything submitted so far.
	aprend_instance_retire(p->instance, p, &aprend_release_handle<aprend_compute_pipeline_t>);
}
aprend_compute_pipeline_desc aprend_compute_pipeline_get_desc(aprend_compute_pipeline p) {
	if (!p)
		return aprend_compute_pipeline_desc{};
	aprend_compute_pipeline_desc desc = p->desc;
	desc.base = apri_struct_base(APRI_STRUCT_TYPE_APREND_COMPUTE_PIPELINE_DESC);
	return desc;
}

aprend_binding_layout aprend_binding_layout_create(
    aprend_instance instance,
    const aprend_binding_desc *bindings,
    uint32_t binding_count) {
	if (!instance) {
		printf("apricot: aprend_binding_layout_create: instance is NULL\n");
		return nullptr;
	}
	if (!bindings) {
		printf("apricot: aprend_binding_layout_create: bindings is NULL\n");
		return nullptr;
	}
	if (binding_count == 0) {
		printf("apricot: aprend_binding_layout_create: binding_count is 0\n");
		return nullptr;
	}
	if (binding_count > APREND_MAX_BINDINGS_PER_LAYOUT) {
		printf("apricot: aprend_binding_layout_create: binding_count %u is above APREND_MAX_BINDINGS_PER_LAYOUT\n", binding_count);
		return nullptr;
	}
	for (uint32_t i = 0; i < binding_count; ++i) {
		if (bindings[i]._type >= APREND_DESCRIPTOR_TYPE_COUNT) {
			printf("apricot: aprend_binding_layout_create: entry %u has a _type that is not a SPUDGPU_DESCRIPTOR_TYPE\n", i);
			return nullptr;
		}
		if (bindings[i]._count == 0) {
			printf("apricot: aprend_binding_layout_create: entry %u has a _count of 0\n", i);
			return nullptr;
		}
		if (bindings[i]._stages == SPUDGPU_SHADER_STAGE_NONE) {
			printf("apricot: aprend_binding_layout_create: entry %u has no _stages\n", i);
			return nullptr;
		}
		for (uint32_t j = 0; j < i; ++j) {
			if (bindings[j]._binding == bindings[i]._binding) {
				printf("apricot: aprend_binding_layout_create: entries %u and %u share binding %u\n", j, i, bindings[i]._binding);
				return nullptr;
			}
		}
	}

	return aprend_binding_layout_build(instance, bindings, binding_count, "aprend_binding_layout_create");
}

// Whether two declarations of one block, [a_count] members from [a] and
// [b_count] from [b], agree: the shorter's members are the first of the
// longer's, each with the same name, type and place. Two stages may declare
// one block to different lengths (each the members it reads); two that lay
// a member out differently would each read the other's writes wrongly.
static bool aprend_block_declarations_agree(const aprend_shader_member *a, uint32_t a_count, const aprend_shader_member *b, uint32_t b_count) {
	const uint32_t count = a_count < b_count ? a_count : b_count;
	for (uint32_t m = 0; m < count; ++m) {
		if (strcmp(a[m].name, b[m].name) != 0)
			return false;
		if (a[m].type != b[m].type)
			return false;
		if (a[m].offset != b[m].offset)
			return false;
		if (a[m].array_count != b[m].array_count)
			return false;
		if (a[m].array_stride != b[m].array_stride)
			return false;
		if (a[m].matrix_stride != b[m].matrix_stride)
			return false;
		if (a[m].row_major != b[m].row_major)
			return false;
	}
	return true;
}

aprend_binding_layout aprend_binding_layout_create_from_shaders(
    aprend_instance instance,
    const aprend_shader *shaders,
    uint32_t shader_count,
    uint32_t set) {
	static const char who[] = "aprend_binding_layout_create_from_shaders";
	if (!instance) {
		printf("apricot: %s: instance is NULL\n", who);
		return nullptr;
	}
	if (!shaders) {
		printf("apricot: %s: shaders is NULL\n", who);
		return nullptr;
	}
	if (shader_count == 0) {
		printf("apricot: %s: shader_count is 0\n", who);
		return nullptr;
	}
	for (uint32_t i = 0; i < shader_count; ++i) {
		if (!shaders[i]) {
			printf("apricot: %s: shaders[%u] is NULL\n", who, i);
			return nullptr;
		}
		if (shaders[i]->instance != instance) {
			printf("apricot: %s: shaders[%u] is of another instance\n", who, i);
			return nullptr;
		}
	}

	// The bindings of [set] every shader uses, merged, and for each the
	// shader resource its name and block are taken from.
	aprend_binding_desc bindings[APREND_MAX_BINDINGS_PER_LAYOUT]{};
	const aprend_shader_resource *sources[APREND_MAX_BINDINGS_PER_LAYOUT]{};
	const aprend_shader_member *source_members[APREND_MAX_BINDINGS_PER_LAYOUT]{};
	uint32_t binding_count = 0;
	for (uint32_t i = 0; i < shader_count; ++i) {
		const aprend_shader_reflection &reflection = shaders[i]->reflection.view;
		for (uint32_t r = 0; r < reflection.resource_count; ++r) {
			const aprend_shader_resource &resource = reflection.resources[r];
			if (resource.set != set || !resource.used)
				continue;
			uint32_t index = 0;
			while (index < binding_count && bindings[index]._binding != resource.binding)
				++index;
			if (index == binding_count) {
				if (binding_count == APREND_MAX_BINDINGS_PER_LAYOUT) {
					printf("apricot: %s: set %u has more than APREND_MAX_BINDINGS_PER_LAYOUT bindings\n", who, set);
					return nullptr;
				}
				++binding_count;
				bindings[index]._binding = resource.binding;
				bindings[index]._type    = resource.type;
				bindings[index]._count   = resource.count;
				bindings[index]._stages  = reflection.stage;
				bindings[index]._name    = resource.name;
				sources[index]           = &resource;
				source_members[index]    = reflection.members;
				continue;
			}
			aprend_binding_desc &binding = bindings[index];
			if (binding._type != resource.type) {
				printf("apricot: %s: set %u binding %u is of type %u in one shader and %u in another\n", who, set, resource.binding,
				       (unsigned)binding._type, (unsigned)resource.type);
				return nullptr;
			}
			binding._stages |= reflection.stage;
			// An array read to different lengths: the layout has the longest.
			if (resource.count > binding._count)
				binding._count = resource.count;
			if (resource.member_count > 0 &&
			    !aprend_block_declarations_agree(source_members[index] + sources[index]->first_member, sources[index]->member_count,
			                                     reflection.members + resource.first_member, resource.member_count)) {
				printf("apricot: %s: set %u binding %u is a block the shaders declare with different members (one is not the start of the other)\n",
				       who, set, resource.binding);
				return nullptr;
			}
			// The block as the shader that declares it largest has it.
			if (resource.block_size > sources[index]->block_size) {
				sources[index]        = &resource;
				source_members[index] = reflection.members;
			}
		}
	}
	if (binding_count == 0) {
		printf("apricot: %s: no shader uses a binding of set %u\n", who, set);
		return nullptr;
	}

	aprend_binding_layout_t *result = aprend_binding_layout_build(instance, bindings, binding_count, who);
	if (!result)
		return nullptr;
	try {
		for (uint32_t i = 0; i < binding_count; ++i) {
			const aprend_shader_resource &resource = *sources[i];
			if (resource.type != SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER && resource.type != SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER)
				continue;
			aprend_binding_layout_t::block_info &block = result->blocks[i];
			block.known                                = true;
			result->strings.push_back(resource.block_name ? resource.block_name : "");
			block.block_name    = result->strings.back().c_str();
			block.size          = resource.block_size;
			block.first_member  = (uint32_t)result->members.size();
			block.member_count  = resource.member_count;
			block.first_uniform = (uint32_t)result->uniforms.size();
			for (uint32_t m = 0; m < resource.member_count; ++m) {
				aprend_shader_member member = source_members[i][resource.first_member + m];
				result->strings.push_back(member.name);
				member.name = result->strings.back().c_str();
				result->members.push_back(member);
				// Every member with a type is a uniform a buffer of this
				// block can be written by name through.
				if (resource.type == SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER && member.type != APREND_UNIFORM_TYPE_UNKNOWN) {
					aprend_uniform uniform{};
					uniform.name        = member.name;
					uniform.type        = member.type;
					uniform.array_count = member.array_count;
					uniform.offset      = member.offset;
					result->uniforms.push_back(uniform);
				}
			}
			block.uniform_count = (uint32_t)result->uniforms.size() - block.first_uniform;
		}
	} catch (const std::bad_alloc &) {
		printf("apricot: %s: out of memory\n", who);
		APREND_DESTRUCT__T(result, aprend_binding_layout_t);
		return nullptr;
	}
	return result;
}

// The index of [binding] in [layout]'s bindings, or UINT32_MAX.
static uint32_t aprend_binding_layout_index(const aprend_binding_layout_t *layout, uint32_t binding) {
	for (uint32_t i = 0; i < layout->binding_count; ++i)
		if (layout->bindings[i]._binding == binding)
			return i;
	return UINT32_MAX;
}

bool aprend_binding_layout_find(
    aprend_binding_layout layout,
    const char *name,
    uint32_t *out_binding,
    aprend_binding_desc *out_desc) {
	if (!layout)
		return false;
	if (!name)
		return false;
	for (uint32_t i = 0; i < layout->binding_count; ++i) {
		const bool by_name  = !layout->names[i].empty() && layout->names[i] == name;
		const bool by_block = layout->blocks[i].known && strcmp(layout->blocks[i].block_name, name) == 0;
		if (!by_name && !by_block)
			continue;
		if (out_binding)
			*out_binding = layout->bindings[i]._binding;
		if (out_desc)
			*out_desc = layout->bindings[i];
		return true;
	}
	return false;
}

bool aprend_binding_layout_get_binding(
    aprend_binding_layout layout,
    uint32_t index,
    aprend_binding_desc *out_desc) {
	if (!layout)
		return false;
	if (!out_desc)
		return false;
	if (index >= layout->binding_count)
		return false;
	*out_desc = layout->bindings[index];
	return true;
}

uint32_t aprend_binding_layout_get_binding_count(aprend_binding_layout layout) { return layout ? layout->binding_count : 0; }

bool aprend_binding_layout_get_block(
    aprend_binding_layout layout,
    uint32_t binding,
    uint32_t *out_size,
    const aprend_shader_member **out_members,
    uint32_t *out_member_count) {
	if (!layout)
		return false;
	const uint32_t index = aprend_binding_layout_index(layout, binding);
	if (index == UINT32_MAX)
		return false;
	const aprend_binding_layout_t::block_info &block = layout->blocks[index];
	if (!block.known)
		return false;
	if (out_size)
		*out_size = block.size;
	if (out_members)
		*out_members = layout->members.data() + block.first_member;
	if (out_member_count)
		*out_member_count = block.member_count;
	return true;
}

bool aprend_binding_layout_get_uniform_layout(
    aprend_binding_layout layout,
    uint32_t binding,
    aprend_uniform_layout *out) {
	if (!layout)
		return false;
	if (!out)
		return false;
	const uint32_t index = aprend_binding_layout_index(layout, binding);
	if (index == UINT32_MAX)
		return false;
	const aprend_binding_layout_t::block_info &block = layout->blocks[index];
	if (!block.known)
		return false;
	if (layout->bindings[index]._type != SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
		return false;
	out->count    = block.uniform_count;
	// The vector is never changed after creation, so the pointer holds.
	out->uniforms = const_cast<aprend_uniform *>(layout->uniforms.data()) + block.first_uniform;
	out->size     = block.size;
	return true;
}
void aprend_binding_layout_destroy(aprend_binding_layout layout) {
	if (!layout)
		return;
	// Released once the GPU has finished everything submitted so far.
	aprend_instance_retire(layout->instance, layout, &aprend_release_handle<aprend_binding_layout_t>);
}

aprend_binding_set aprend_binding_set_create(
    aprend_binding_layout layout,
    const aprend_binding_set_entry *entries,
    uint32_t entry_count) {
	if (!layout) {
		printf("apricot: aprend_binding_set_create: layout is NULL\n");
		return nullptr;
	}
	APREND_ENTER(layout->instance, "aprend_binding_set_create");
	if (!entries) {
		printf("apricot: aprend_binding_set_create: entries is NULL\n");
		return nullptr;
	}
	if (entry_count != layout->descriptor_count) {
		printf("apricot: aprend_binding_set_create: expected %u entries, got %u\n", layout->descriptor_count, entry_count);
		return nullptr;
	}
	// Every entry must name an array element the layout declares, once. The
	// count matches, so nothing repeated also means nothing left unfilled.
	uint32_t image_use_count = 0;
	bool has_buffer_set      = false;
	for (uint32_t i = 0; i < entry_count; ++i) {
		const aprend_binding_set_entry &entry = entries[i];
		const aprend_binding_desc *binding    = aprend_binding_layout_find_desc(layout, entry._binding);
		if (!binding) {
			printf("apricot: aprend_binding_set_create: entry %u names binding %u, which the layout doesn't declare\n", i, entry._binding);
			return nullptr;
		}
		if (entry._array_element >= binding->_count) {
			printf("apricot: aprend_binding_set_create: entry %u (binding %u) has _array_element %u of %u\n", i, entry._binding, entry._array_element,
			       binding->_count);
			return nullptr;
		}
		if (entry._type != binding->_type) {
			printf("apricot: aprend_binding_set_create: entry %u (binding %u) has _type %u, the layout declares %u\n", i, entry._binding,
			       (unsigned)entry._type, (unsigned)binding->_type);
			return nullptr;
		}
		for (uint32_t j = 0; j < i; ++j) {
			if (entries[j]._binding == entry._binding && entries[j]._array_element == entry._array_element) {
				printf("apricot: aprend_binding_set_create: entries %u and %u both fill binding %u element %u\n", j, i, entry._binding,
				       entry._array_element);
				return nullptr;
			}
		}
		if (!aprend_binding_set_entry_resource_valid("aprend_binding_set_create", layout->instance, entry, i))
			return nullptr;
		if (aprend_descriptor_type_reads_view(entry._type))
			++image_use_count;
		if (entry._type == SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER && entry._resource._uniform._set)
			has_buffer_set = true;
		if (entry._type == SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER && entry._resource._storage._set)
			has_buffer_set = true;
	}
	// One descriptor set if nothing in it changes with the frame; one for
	// each frame in flight if an entry is a buffer set.
	const uint32_t frame_set_count = has_buffer_set ? layout->instance->desc.frames_in_flight : 1;

	// What the writes point at; only needed until the set is written.
	struct write_info {
		spudgpu_descriptor_buffer_info buffer;
		spudgpu_descriptor_image_info image;
	};
	write_info *infos = (write_info *)calloc(entry_count, sizeof(write_info));
	if (!infos)
		return nullptr;
	spudgpu_write_descriptor_set *writes = (spudgpu_write_descriptor_set *)calloc(entry_count, sizeof(spudgpu_write_descriptor_set));
	if (!writes) {
		free(infos);
		return nullptr;
	}

	APREND_MALLOC__T(result, aprend_binding_set_t);
	if (!result) {
		free(writes);
		free(infos);
		return nullptr;
	}
	APREND_CONSTRUCT__T(result, aprend_binding_set_t);
	result->layout   = layout;
	// Held from here on: the destructor lets go of it on every path below.
	result->core = layout->core;
	++result->core->holders;
	result->instance = layout->instance;

	// What the set was built from, for aprend_binding_set_update to change
	// part of. entry_count is never 0: a layout has at least one binding.
	result->entries = (aprend_binding_set_entry *)malloc(sizeof(aprend_binding_set_entry) * entry_count);
	if (!result->entries) {
		APREND_DESTRUCT__T(result, aprend_binding_set_t);
		free(writes);
		free(infos);
		return nullptr;
	}
	memcpy(result->entries, entries, sizeof(aprend_binding_set_entry) * entry_count);
	result->entry_count = entry_count;

	if (image_use_count > 0) {
		result->image_uses = (aprend_binding_set_t::image_use *)calloc(image_use_count, sizeof(aprend_binding_set_t::image_use));
		if (!result->image_uses) {
			APREND_DESTRUCT__T(result, aprend_binding_set_t);
			free(writes);
			free(infos);
			return nullptr;
		}
	}

	// Room for every entry of every frame's set: at most each one is a
	// staged storage buffer.
	result->storage_store_capacity = entry_count;
	result->storage_stores =
	    (aprend_binding_set_t::storage_store *)calloc((size_t)entry_count * frame_set_count, sizeof(aprend_binding_set_t::storage_store));
	if (!result->storage_stores) {
		APREND_DESTRUCT__T(result, aprend_binding_set_t);
		free(writes);
		free(infos);
		return nullptr;
	}

	// frame_set_count goes up with each set acquired, so the destructor
	// gives back exactly those if a later one fails.
	for (uint32_t frame = 0; frame < frame_set_count; ++frame) {
		SPUDRESULT sr = aprend_binding_layout_acquire_set(layout->core, &result->sets[frame]);
		if (SPUDFAIL(sr)) {
			printf("apricot: aprend_binding_set_create failed: %s\n", spudresult_str(sr));
			result->sets[frame] = nullptr;
			APREND_DESTRUCT__T(result, aprend_binding_set_t);
			free(writes);
			free(infos);
			return nullptr;
		}
		++result->frame_set_count;
	}

	for (uint32_t frame = 0; frame < frame_set_count; ++frame) {
		aprend_binding_set_t::storage_store *frame_stores = result->storage_stores + (size_t)frame * entry_count;
		for (uint32_t i = 0; i < entry_count; ++i) {
			const aprend_binding_set_entry &entry = entries[i];
			spudgpu_write_descriptor_set &write   = writes[i];
			write                                 = {};
			write.base.type                       = SPUD_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET;
			infos[i]                              = {};
			write.dst_set                         = result->sets[frame];
			write.dst_binding                     = entry._binding;
			write.dst_array_element               = entry._array_element;
			write.descriptor_count                = 1;
			write.descriptor_type                 = entry._type;

			if (entry._type == SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
				const auto &r                      = entry._resource._uniform;
				const aprend_uniform_buffer buffer = r._set ? r._set->buffers[frame] : r._buffer;
				infos[i].buffer.buffer             = buffer->buffer;
				infos[i].buffer.offset             = r._offset;
				infos[i].buffer.range              = r._range ? r._range : buffer->size - r._offset;
				write.buffer_info                  = &infos[i].buffer;
				continue;
			}
			if (entry._type == SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
				const auto &r                      = entry._resource._storage;
				const aprend_storage_buffer buffer = r._set ? r._set->buffers[frame] : r._buffer;
				infos[i].buffer.buffer             = buffer->store.buffer;
				infos[i].buffer.offset             = buffer->buffer_view_desc.offset_from_parent_buffer + r._offset;
				infos[i].buffer.range              = r._range ? r._range : buffer->size - r._offset;
				write.buffer_info                  = &infos[i].buffer;
				// Every storage buffer, staged or not; the same one in two
				// slots is listed twice, and a command list takes each once.
				frame_stores[result->storage_store_counts[frame]++] = {&buffer->store, entry._binding};
				continue;
			}

			const auto &r = entry._resource._image;
			if (aprend_descriptor_type_reads_view(entry._type)) {
				// A storage image is read and written in GENERAL; anything
				// sampled is read in SHADER_READ_ONLY.
				const SPUDGPU_IMAGE_LAYOUT image_layout =
				    entry._type == SPUDGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE ? SPUDGPU_IMAGE_LAYOUT_GENERAL : SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY;
				infos[i].image.image_view   = r._view->image_view;
				infos[i].image.image_layout = image_layout;
				write.image_info            = &infos[i].image;

				// The same textures in every frame's set: recorded once.
				if (frame == 0)
					result->image_uses[result->image_use_count++] = {r._view, image_layout, entry._binding};
			}
			if (aprend_descriptor_type_reads_sampler(entry._type))
				write.sampler = r._sampler->sampler;
		}
		spudgpu_update_descriptor_sets(layout->instance->desc.device, writes, entry_count);
	}

	free(writes);
	free(infos);
	return result;
}
bool aprend_binding_set_update(
    aprend_binding_set set,
    const aprend_binding_set_entry *entries,
    uint32_t entry_count) {
	static const char who[] = "aprend_binding_set_update";
	if (!set) {
		printf("apricot: %s: set is NULL\n", who);
		return false;
	}
	APREND_ENTER(set->instance, "aprend_binding_set_update");
	if (!entries) {
		printf("apricot: %s: entries is NULL\n", who);
		return false;
	}
	if (entry_count == 0) {
		printf("apricot: %s: entry_count is 0\n", who);
		return false;
	}
	if (entry_count > set->entry_count) {
		printf("apricot: %s: %u entries, and the set has %u\n", who, entry_count, set->entry_count);
		return false;
	}
	aprend_binding_layout layout = set->layout;
	// Each entry names an array element the layout declares, once, and holds
	// what its slot takes: the checks aprend_binding_set_create makes.
	for (uint32_t i = 0; i < entry_count; ++i) {
		const aprend_binding_set_entry &entry = entries[i];
		const aprend_binding_desc *binding    = aprend_binding_layout_find_desc(layout, entry._binding);
		if (!binding) {
			printf("apricot: %s: entry %u names binding %u, which the layout doesn't declare\n", who, i, entry._binding);
			return false;
		}
		if (entry._array_element >= binding->_count) {
			printf("apricot: %s: entry %u (binding %u) has _array_element %u of %u\n", who, i, entry._binding, entry._array_element, binding->_count);
			return false;
		}
		if (entry._type != binding->_type) {
			printf("apricot: %s: entry %u (binding %u) has _type %u, the layout declares %u\n", who, i, entry._binding, (unsigned)entry._type,
			       (unsigned)binding->_type);
			return false;
		}
		for (uint32_t j = 0; j < i; ++j) {
			if (entries[j]._binding == entry._binding && entries[j]._array_element == entry._array_element) {
				printf("apricot: %s: entries %u and %u both fill binding %u element %u\n", who, j, i, entry._binding, entry._array_element);
				return false;
			}
		}
		if (!aprend_binding_set_entry_resource_valid(who, layout->instance, entry, i))
			return false;
	}

	// The set as it will be: what it was built from, with the given entries
	// in place of the ones that fill the same elements.
	aprend_binding_set_entry *merged = (aprend_binding_set_entry *)malloc(sizeof(aprend_binding_set_entry) * set->entry_count);
	if (!merged)
		return false;
	memcpy(merged, set->entries, sizeof(aprend_binding_set_entry) * set->entry_count);
	for (uint32_t i = 0; i < entry_count; ++i)
		for (uint32_t j = 0; j < set->entry_count; ++j)
			if (merged[j]._binding == entries[i]._binding && merged[j]._array_element == entries[i]._array_element)
				merged[j] = entries[i];

	// Descriptor sets a submitted frame may be reading are never rewritten.
	// A whole new set is built, the handle takes what it holds, and the new
	// handle leaves with the old descriptor sets, which the release queue
	// gives back to the layout once the GPU has finished with them.
	aprend_binding_set fresh = aprend_binding_set_create(layout, merged, set->entry_count);
	free(merged);
	if (!fresh)
		return false;
	std::swap(set->sets, fresh->sets);
	std::swap(set->frame_set_count, fresh->frame_set_count);
	std::swap(set->entries, fresh->entries);
	std::swap(set->entry_count, fresh->entry_count);
	std::swap(set->image_uses, fresh->image_uses);
	std::swap(set->image_use_count, fresh->image_use_count);
	std::swap(set->storage_stores, fresh->storage_stores);
	std::swap(set->storage_store_capacity, fresh->storage_store_capacity);
	std::swap(set->storage_store_counts, fresh->storage_store_counts);
	// A list compiled before would bind the old descriptor sets, which go to
	// the release queue now and may be gone before it is submitted.
	aprend_instance_forget(set->instance, set);
	aprend_binding_set_destroy(fresh);
	return true;
}
void aprend_binding_set_destroy(aprend_binding_set set) {
	if (!set)
		return;
	// A compiled list that binds it is refused at submit.
	aprend_instance_forget(set->instance, set);
	// Released once the GPU has finished everything submitted so far.
	aprend_instance_retire(set->instance, set, &aprend_release_handle<aprend_binding_set_t>);
}

} // Extern "C"
