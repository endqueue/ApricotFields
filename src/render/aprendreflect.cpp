
// SPIR-V reflection for aprend_shader (render/aprendshader.h), through
// SPIRV-Cross's C++ interface. Nothing here calls SpudGPU.

#include "aprendreflect_internal.hpp"

#include <spirv_cross.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <new>
#include <unordered_set>

uint32_t aprend_uniform_type_columns(APREND_UNIFORM_TYPE type) {
	switch (type) {
	case APREND_UNIFORM_TYPE_MAT2:
		return 2;
	case APREND_UNIFORM_TYPE_MAT3:
		return 3;
	case APREND_UNIFORM_TYPE_MAT4:
		return 4;
	default:
		return 1;
	}
}

uint32_t aprend_uniform_type_column_size(APREND_UNIFORM_TYPE type) {
	const uint32_t size = aprend_uniform_type_get_size(type);
	return size / aprend_uniform_type_columns(type);
}

void aprend_write_block_member(
    const APREND_SHADER_MEMBER &member,
    uint8_t *block,
    const void *data) {
	const uint8_t *source        = (const uint8_t *)data;
	const uint32_t columns       = aprend_uniform_type_columns(member.type);
	const uint32_t column_size   = aprend_uniform_type_column_size(member.type);
	const uint32_t element_size  = columns * column_size;
	for (uint32_t element = 0; element < member.array_count; ++element) {
		uint8_t *destination = block + member.offset + (size_t)element * member.array_stride;
		if (columns == 1) {
			memcpy(destination, source, element_size);
		} else if (!member.row_major) {
			// Column after column, each at the matrix stride: a mat3's
			// columns are 12 bytes apart in the caller's data and 16 in a
			// std140 block.
			for (uint32_t column = 0; column < columns; ++column)
				memcpy(destination + (size_t)column * member.matrix_stride, source + (size_t)column * column_size, column_size);
		} else {
			// Stored a row at a time: the caller's column c, row r is
			// row r, column c of the block.
			const uint32_t rows = column_size / 4;
			for (uint32_t column = 0; column < columns; ++column)
				for (uint32_t row = 0; row < rows; ++row)
					memcpy(destination + (size_t)row * member.matrix_stride + (size_t)column * 4, source + (size_t)column * column_size + (size_t)row * 4, 4);
		}
		source += element_size;
	}
}

void aprend_read_block_member(
    const APREND_SHADER_MEMBER &member,
    const uint8_t *block,
    void *out_data) {
	uint8_t *destination         = (uint8_t *)out_data;
	const uint32_t columns       = aprend_uniform_type_columns(member.type);
	const uint32_t column_size   = aprend_uniform_type_column_size(member.type);
	const uint32_t element_size  = columns * column_size;
	for (uint32_t element = 0; element < member.array_count; ++element) {
		const uint8_t *source = block + member.offset + (size_t)element * member.array_stride;
		// aprend_write_block_member, the other way.
		if (columns == 1) {
			memcpy(destination, source, element_size);
		} else if (!member.row_major) {
			for (uint32_t column = 0; column < columns; ++column)
				memcpy(destination + (size_t)column * column_size, source + (size_t)column * member.matrix_stride, column_size);
		} else {
			const uint32_t rows = column_size / 4;
			for (uint32_t column = 0; column < columns; ++column)
				for (uint32_t row = 0; row < rows; ++row)
					memcpy(destination + (size_t)column * column_size + (size_t)row * 4, source + (size_t)row * member.matrix_stride + (size_t)column * 4, 4);
		}
		destination += element_size;
	}
}

namespace {

// The SPIR-V execution models a shader of [stage] may have its entry point
// in. A mesh shader may be written for either mesh extension.
bool aprend_stage_matches(SPUDGPU_SHADER_STAGE stage, spv::ExecutionModel model) {
	switch (stage) {
	case SPUDGPU_SHADER_STAGE_VERTEX:
		return model == spv::ExecutionModelVertex;
	case SPUDGPU_SHADER_STAGE_FRAGMENT:
		return model == spv::ExecutionModelFragment;
	case SPUDGPU_SHADER_STAGE_COMPUTE:
		return model == spv::ExecutionModelGLCompute;
	case SPUDGPU_SHADER_STAGE_MESH:
		return model == spv::ExecutionModelMeshEXT || model == spv::ExecutionModelMeshNV;
	default:
		return false;
	}
}

/* The SpudGPU format a storage image declared with [format] (GLSL's
 * `layout(rgba8)`) is viewed in: Vulkan's table of formats compatible with
 * each SPIR-V image format, which is one format each. UNKNOWN for an image
 * declared without one, and for one SpudGPU has no format for. */
SPUDGPU_FORMAT aprend_spirv_image_format(spv::ImageFormat format) {
	switch (format) {
	case spv::ImageFormatRgba32f: return SPUDGPU_FORMAT_R32G32B32A32_FLOAT;
	case spv::ImageFormatRgba16f: return SPUDGPU_FORMAT_R16G16B16A16_FLOAT;
	case spv::ImageFormatR32f: return SPUDGPU_FORMAT_R32_FLOAT;
	case spv::ImageFormatRgba8: return SPUDGPU_FORMAT_R8G8B8A8_UNORM;
	case spv::ImageFormatRgba8Snorm: return SPUDGPU_FORMAT_R8G8B8A8_SNORM;
	case spv::ImageFormatRg32f: return SPUDGPU_FORMAT_R32G32_FLOAT;
	case spv::ImageFormatRg16f: return SPUDGPU_FORMAT_R16G16_FLOAT;
	case spv::ImageFormatR11fG11fB10f: return SPUDGPU_FORMAT_R11G11B10_FLOAT;
	case spv::ImageFormatR16f: return SPUDGPU_FORMAT_R16_FLOAT;
	case spv::ImageFormatRgba16: return SPUDGPU_FORMAT_R16G16B16A16_UNORM;
	case spv::ImageFormatRgb10A2: return SPUDGPU_FORMAT_R10G10B10A2_UNORM;
	case spv::ImageFormatRg16: return SPUDGPU_FORMAT_R16G16_UNORM;
	case spv::ImageFormatRg8: return SPUDGPU_FORMAT_R8G8_UNORM;
	case spv::ImageFormatR16: return SPUDGPU_FORMAT_R16_UNORM;
	case spv::ImageFormatR8: return SPUDGPU_FORMAT_R8_UNORM;
	case spv::ImageFormatRgba16Snorm: return SPUDGPU_FORMAT_R16G16B16A16_SNORM;
	case spv::ImageFormatRg16Snorm: return SPUDGPU_FORMAT_R16G16_SNORM;
	case spv::ImageFormatRg8Snorm: return SPUDGPU_FORMAT_R8G8_SNORM;
	case spv::ImageFormatR16Snorm: return SPUDGPU_FORMAT_R16_SNORM;
	case spv::ImageFormatR8Snorm: return SPUDGPU_FORMAT_R8_SNORM;
	case spv::ImageFormatRgba32i: return SPUDGPU_FORMAT_R32G32B32A32_SINT;
	case spv::ImageFormatRgba16i: return SPUDGPU_FORMAT_R16G16B16A16_SINT;
	case spv::ImageFormatRgba8i: return SPUDGPU_FORMAT_R8G8B8A8_SINT;
	case spv::ImageFormatR32i: return SPUDGPU_FORMAT_R32_SINT;
	case spv::ImageFormatRg32i: return SPUDGPU_FORMAT_R32G32_SINT;
	case spv::ImageFormatRg16i: return SPUDGPU_FORMAT_R16G16_SINT;
	case spv::ImageFormatRg8i: return SPUDGPU_FORMAT_R8G8_SINT;
	case spv::ImageFormatR16i: return SPUDGPU_FORMAT_R16_SINT;
	case spv::ImageFormatR8i: return SPUDGPU_FORMAT_R8_SINT;
	case spv::ImageFormatRgba32ui: return SPUDGPU_FORMAT_R32G32B32A32_UINT;
	case spv::ImageFormatRgba16ui: return SPUDGPU_FORMAT_R16G16B16A16_UINT;
	case spv::ImageFormatRgba8ui: return SPUDGPU_FORMAT_R8G8B8A8_UINT;
	case spv::ImageFormatR32ui: return SPUDGPU_FORMAT_R32_UINT;
	case spv::ImageFormatRgb10a2ui: return SPUDGPU_FORMAT_R10G10B10A2_UINT;
	case spv::ImageFormatRg32ui: return SPUDGPU_FORMAT_R32G32_UINT;
	case spv::ImageFormatRg16ui: return SPUDGPU_FORMAT_R16G16_UINT;
	case spv::ImageFormatRg8ui: return SPUDGPU_FORMAT_R8G8_UINT;
	case spv::ImageFormatR16ui: return SPUDGPU_FORMAT_R16_UINT;
	case spv::ImageFormatR8ui: return SPUDGPU_FORMAT_R8_UINT;
	default: return SPUDGPU_FORMAT_UNKNOWN;
	}
}

APREND_SHADER_SCALAR_TYPE aprend_scalar_type(const spirv_cross::SPIRType &type) {
	switch (type.basetype) {
	case spirv_cross::SPIRType::Float:
		return type.width == 32 ? APREND_SHADER_SCALAR_TYPE_FLOAT : APREND_SHADER_SCALAR_TYPE_OTHER;
	case spirv_cross::SPIRType::Int:
		return type.width == 32 ? APREND_SHADER_SCALAR_TYPE_INT : APREND_SHADER_SCALAR_TYPE_OTHER;
	case spirv_cross::SPIRType::UInt:
		return type.width == 32 ? APREND_SHADER_SCALAR_TYPE_UINT : APREND_SHADER_SCALAR_TYPE_OTHER;
	case spirv_cross::SPIRType::Boolean:
		return APREND_SHADER_SCALAR_TYPE_BOOL;
	default:
		return APREND_SHADER_SCALAR_TYPE_OTHER;
	}
}

// The APREND_UNIFORM_TYPE of one element of [type], a scalar, vector or
// matrix; APREND_UNIFORM_TYPE_UNKNOWN for any that has none.
APREND_UNIFORM_TYPE aprend_uniform_type_of(const spirv_cross::SPIRType &type) {
	const APREND_SHADER_SCALAR_TYPE scalar = aprend_scalar_type(type);
	const uint32_t vecsize                 = type.vecsize;
	if (type.columns > 1) {
		// Square float matrices only: mat2x3 and the rest have no type.
		if (scalar != APREND_SHADER_SCALAR_TYPE_FLOAT || type.columns != vecsize)
			return APREND_UNIFORM_TYPE_UNKNOWN;
		if (vecsize == 2)
			return APREND_UNIFORM_TYPE_MAT2;
		if (vecsize == 3)
			return APREND_UNIFORM_TYPE_MAT3;
		if (vecsize == 4)
			return APREND_UNIFORM_TYPE_MAT4;
		return APREND_UNIFORM_TYPE_UNKNOWN;
	}
	switch (scalar) {
	case APREND_SHADER_SCALAR_TYPE_FLOAT: {
		static const APREND_UNIFORM_TYPE types[] = {APREND_UNIFORM_TYPE_FLOAT, APREND_UNIFORM_TYPE_VEC2, APREND_UNIFORM_TYPE_VEC3,
		                                            APREND_UNIFORM_TYPE_VEC4};
		return vecsize >= 1 && vecsize <= 4 ? types[vecsize - 1] : APREND_UNIFORM_TYPE_UNKNOWN;
	}
	case APREND_SHADER_SCALAR_TYPE_INT: {
		static const APREND_UNIFORM_TYPE types[] = {APREND_UNIFORM_TYPE_INT, APREND_UNIFORM_TYPE_INT2, APREND_UNIFORM_TYPE_INT3,
		                                            APREND_UNIFORM_TYPE_INT4};
		return vecsize >= 1 && vecsize <= 4 ? types[vecsize - 1] : APREND_UNIFORM_TYPE_UNKNOWN;
	}
	case APREND_SHADER_SCALAR_TYPE_UINT: {
		static const APREND_UNIFORM_TYPE types[] = {APREND_UNIFORM_TYPE_UINT, APREND_UNIFORM_TYPE_UINT2, APREND_UNIFORM_TYPE_UINT3,
		                                            APREND_UNIFORM_TYPE_UINT4};
		return vecsize >= 1 && vecsize <= 4 ? types[vecsize - 1] : APREND_UNIFORM_TYPE_UNKNOWN;
	}
	case APREND_SHADER_SCALAR_TYPE_BOOL:
		// A bool is a 32-bit value in a block; a bvec has no type here.
		return vecsize == 1 ? (APREND_UNIFORM_TYPE)APREND_UNIFORM_TYPE_BOOL : (APREND_UNIFORM_TYPE)APREND_UNIFORM_TYPE_UNKNOWN;
	default:
		return APREND_UNIFORM_TYPE_UNKNOWN;
	}
}

// Reads one shader. Throws: SPIRV-Cross's errors and std::bad_alloc go to
// aprend_reflect_spirv, which catches them.
struct aprend_reflector {
	spirv_cross::Compiler &compiler;
	aprend_reflection_store &out;
	std::string &error;
	// Every variable the entry point reaches; empty, with [all_used] set,
	// when the entry point can't be told.
	std::unordered_set<spirv_cross::VariableID> active{};
	bool all_used{false};

	const char *keep(const std::string &text) {
		out.strings.push_back(text);
		return out.strings.back().c_str();
	}

	bool used(spirv_cross::ID id) const { return all_used || active.count(id) != 0; }

	// The top-level members of the block being listed that the entry point
	// touches.
	std::unordered_set<uint32_t> used_top_members{};

	// Adds the members of a block: the leaves of [block_type], each marked
	// used if the entry point touches the top-level member it is under.
	void add_block_members(spirv_cross::ID variable, const spirv_cross::SPIRType &block_type) {
		used_top_members.clear();
		if (!all_used) {
			for (const spirv_cross::BufferRange &range : compiler.get_active_buffer_ranges(variable))
				used_top_members.insert(range.index);
		}
		for (uint32_t i = 0; i < (uint32_t)block_type.member_types.size(); ++i) {
			const size_t first = out.members.size();
			add_members_of(block_type, i, 0, "");
			for (size_t m = first; m < out.members.size(); ++m)
				out.members[m].used = all_used || used_top_members.count(i) != 0;
		}
	}

	// Adds a member for each leaf of [struct_type], offsets from
	// [base_offset] and names after [prefix]. An array of structs is each of
	// its elements; an array of arrays of a leaf is each outer element.
	void add_members(const spirv_cross::SPIRType &struct_type, uint32_t base_offset, const std::string &prefix) {
		for (uint32_t i = 0; i < (uint32_t)struct_type.member_types.size(); ++i)
			add_members_of(struct_type, i, base_offset, prefix);
	}

	// Member [i] of [struct_type], as add_members.
	void add_members_of(const spirv_cross::SPIRType &struct_type, uint32_t i, uint32_t base_offset, const std::string &prefix) {
		{
			const spirv_cross::SPIRType &member_type = compiler.get_type(struct_type.member_types[i]);
			std::string name                         = compiler.get_member_name(struct_type.self, i);
			if (name.empty())
				name = "_m" + std::to_string(i);
			const uint32_t offset = base_offset + compiler.type_struct_member_offset(struct_type, i);
			const uint32_t size   = (uint32_t)compiler.get_declared_struct_member_size(struct_type, i);
			const uint32_t stride = member_type.array.empty() ? 0 : compiler.type_struct_member_array_stride(struct_type, i);
			const uint32_t matrix_stride =
			    member_type.columns > 1 ? compiler.type_struct_member_matrix_stride(struct_type, i) : 0;
			const bool row_major = compiler.has_member_decoration(struct_type.self, i, spv::DecorationRowMajor);
			add_member(member_type, (uint32_t)member_type.array.size(), offset, size, stride, matrix_stride, row_major, prefix + name);
		}
	}

	// [type] with only its first [dims] array dimensions (the innermost
	// ones) left to list; [stride] is the stride of the outermost of those.
	void add_member(
	    const spirv_cross::SPIRType &type,
	    uint32_t dims,
	    uint32_t offset,
	    uint32_t size,
	    uint32_t stride,
	    uint32_t matrix_stride,
	    bool row_major,
	    const std::string &name) {
		const bool is_struct = type.basetype == spirv_cross::SPIRType::Struct;
		// A leaf with one dimension or none is one member.
		if (!is_struct && dims <= 1) {
			APREND_SHADER_MEMBER member{};
			member.name          = keep(name);
			member.type          = aprend_uniform_type_of(type);
			member.offset        = offset;
			member.size          = size;
			member.array_count   = dims == 0 ? 1 : (type.array_size_literal[0] ? type.array[0] : 0);
			member.array_stride  = dims == 0 ? 0 : stride;
			member.matrix_stride = matrix_stride;
			member.row_major     = row_major;
			out.members.push_back(member);
			return;
		}
		if (dims == 0) {
			add_members(type, offset, name + ".");
			return;
		}
		// The outermost remaining dimension, element by element. Its
		// elements are tightly strided arrays of the dimensions inside it,
		// so the next one in is this stride over its own count.
		const uint32_t outer = dims - 1;
		uint32_t count       = type.array_size_literal[outer] ? type.array[outer] : 0;
		if (count == 0) {
			// An array without a size: only the last member of a storage
			// block. Its element 0 is listed; the resource records the stride.
			count = 1;
		}
		const uint32_t inner_stride = (dims >= 2 && type.array[outer - 1]) ? stride / type.array[outer - 1] : 0;
		const uint32_t element_size = stride;
		for (uint32_t element = 0; element < count; ++element) {
			add_member(type, outer, offset + element * stride, element_size, inner_stride, matrix_stride, row_major,
			           name + "[" + std::to_string(element) + "]");
		}
	}

	bool unsupported(const spirv_cross::Resource &resource, const char *what) {
		if (!used(resource.id))
			return true;
		error = "the entry point uses \"" + compiler.get_name(resource.id) + "\", " + what + ", which Aprend has no binding for";
		return false;
	}

	// One descriptor binding. False, with [error], for one the entry point
	// uses that Aprend can't give it.
	bool add_resource(const spirv_cross::Resource &resource, SPUDGPU_DESCRIPTOR_TYPE type) {
		const spirv_cross::SPIRType &var_type  = compiler.get_type(resource.type_id);
		const spirv_cross::SPIRType &base_type = compiler.get_type(resource.base_type_id);
		const bool is_buffer = type == SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER || type == SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER;

		APREND_SHADER_RESOURCE out_resource{};
		out_resource.set     = compiler.get_decoration(resource.id, spv::DecorationDescriptorSet);
		out_resource.binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
		out_resource.type    = type;
		out_resource.used    = used(resource.id);

		uint32_t count = 1;
		for (size_t dim = 0; dim < var_type.array.size(); ++dim) {
			if (!var_type.array_size_literal[dim] || var_type.array[dim] == 0) {
				// A descriptor array sized at run time or by a
				// specialization constant: bindless, which Aprend doesn't
				// offer (item 33).
				return unsupported(resource, "an array of descriptors without a fixed size");
			}
			count *= var_type.array[dim];
		}
		out_resource.count = count;

		std::string instance_name = compiler.get_name(resource.id);
		if (is_buffer) {
			const std::string block_name = compiler.get_name(resource.base_type_id);
			out_resource.block_name      = keep(block_name);
			out_resource.name            = instance_name.empty() ? out_resource.block_name : keep(instance_name);
			out_resource.block_size      = (uint32_t)compiler.get_declared_struct_size(base_type);
			if (type == SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
				const spirv_cross::Bitset flags = compiler.get_buffer_block_flags(resource.id);
				out_resource.read_only          = flags.get(spv::DecorationNonWritable);
				// A last member whose array has no size.
				if (!base_type.member_types.empty()) {
					const uint32_t last                 = (uint32_t)base_type.member_types.size() - 1;
					const spirv_cross::SPIRType &member = compiler.get_type(base_type.member_types[last]);
					if (!member.array.empty() && member.array_size_literal.back() && member.array.back() == 0)
						out_resource.runtime_array_stride = compiler.type_struct_member_array_stride(base_type, last);
				}
			}
			out_resource.first_member = (uint32_t)out.members.size();
			add_block_members(resource.id, base_type);
			out_resource.member_count = (uint32_t)out.members.size() - out_resource.first_member;
		} else {
			out_resource.name = keep(instance_name);
		}

		if (type != SPUDGPU_DESCRIPTOR_TYPE_SAMPLER && !is_buffer) {
			const spirv_cross::SPIRType::ImageType &image = base_type.image;
			if (image.ms)
				return unsupported(resource, "a multisampled image");
			switch (image.dim) {
			case spv::Dim2D:
				out_resource.image_shape = image.arrayed ? APREND_TEXTURE_VIEW_SHAPE_2D_ARRAY : APREND_TEXTURE_VIEW_SHAPE_2D;
				break;
			case spv::DimCube:
				out_resource.image_shape = image.arrayed ? APREND_TEXTURE_VIEW_SHAPE_CUBE_ARRAY : APREND_TEXTURE_VIEW_SHAPE_CUBE;
				break;
			case spv::Dim3D:
				out_resource.image_shape = APREND_TEXTURE_VIEW_SHAPE_3D;
				break;
			case spv::Dim1D:
				return unsupported(resource, "a 1D image (a row of texels is a 2D texture of height 1, aprendimages.h)");
			case spv::DimBuffer:
				return unsupported(resource, "a texel buffer");
			case spv::DimSubpassData:
				return unsupported(resource, "an input attachment");
			default:
				return unsupported(resource, "an image of a dimension no texture view has");
			}
			out_resource.image_scalar_type   = aprend_scalar_type(compiler.get_type(image.type));
			out_resource.image_depth_compare = image.depth;
			if (type == SPUDGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
				out_resource.read_only       = compiler.has_decoration(resource.id, spv::DecorationNonWritable);
				const SPUDGPU_FORMAT format  = aprend_spirv_image_format(image.format);
				if (format != SPUDGPU_FORMAT_UNKNOWN && out_resource.used)
					out.storage_formats.push_back({out_resource.set, out_resource.binding, format});
			}
		}
		out.resources.push_back(out_resource);
		return true;
	}

	bool reflect(SPUDGPU_SHADER_STAGE stage) {
		// The entry point of the shader's stage. With exactly one, what it
		// reaches can be told apart from what is only declared.
		std::vector<spirv_cross::EntryPoint> entries;
		for (const spirv_cross::EntryPoint &entry : compiler.get_entry_points_and_stages())
			if (aprend_stage_matches(stage, entry.execution_model))
				entries.push_back(entry);
		if (entries.empty()) {
			error = "the SPIR-V has no entry point for the shader's stage";
			return false;
		}
		out.view.stage = stage;
		if (entries.size() == 1) {
			compiler.set_entry_point(entries[0].name, entries[0].execution_model);
			active                 = compiler.get_active_interface_variables();
			out.view.entry_point   = keep(entries[0].name);
		} else {
			all_used             = true;
			out.view.entry_point = nullptr;
		}

		const spirv_cross::ShaderResources resources = compiler.get_shader_resources();
		for (const spirv_cross::Resource &r : resources.uniform_buffers)
			if (!add_resource(r, SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER))
				return false;
		for (const spirv_cross::Resource &r : resources.storage_buffers)
			if (!add_resource(r, SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER))
				return false;
		for (const spirv_cross::Resource &r : resources.sampled_images)
			if (!add_resource(r, SPUDGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER))
				return false;
		for (const spirv_cross::Resource &r : resources.separate_images)
			if (!add_resource(r, SPUDGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE))
				return false;
		for (const spirv_cross::Resource &r : resources.separate_samplers)
			if (!add_resource(r, SPUDGPU_DESCRIPTOR_TYPE_SAMPLER))
				return false;
		for (const spirv_cross::Resource &r : resources.storage_images)
			if (!add_resource(r, SPUDGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE))
				return false;
		for (const spirv_cross::Resource &r : resources.subpass_inputs)
			if (!unsupported(r, "an input attachment"))
				return false;
		for (const spirv_cross::Resource &r : resources.acceleration_structures)
			if (!unsupported(r, "an acceleration structure"))
				return false;
		for (const spirv_cross::Resource &r : resources.atomic_counters)
			if (!unsupported(r, "an atomic counter"))
				return false;

		// In set and binding order. The members stay where they are: each
		// resource knows where its own start.
		std::stable_sort(out.resources.begin(), out.resources.end(), [](const APREND_SHADER_RESOURCE &a, const APREND_SHADER_RESOURCE &b) {
			return a.set != b.set ? a.set < b.set : a.binding < b.binding;
		});

		// A block the entry point doesn't touch needs no range.
		if (!resources.push_constant_buffers.empty() && used(resources.push_constant_buffers[0].id)) {
			const spirv_cross::Resource &r         = resources.push_constant_buffers[0];
			const spirv_cross::SPIRType &base_type = compiler.get_type(r.base_type_id);
			const std::string instance_name        = compiler.get_name(r.id);
			out.view.has_push_constants            = true;
			out.view.push_constant_name            = keep(instance_name.empty() ? compiler.get_name(r.base_type_id) : instance_name);
			out.view.push_constant_first_member    = (uint32_t)out.members.size();
			add_block_members(r.id, base_type);
			out.view.push_constant_member_count = (uint32_t)out.members.size() - out.view.push_constant_first_member;
			// The bytes the used members span: what a range for this stage
			// has to cover, at the least.
			uint32_t begin = UINT32_MAX;
			uint32_t end   = 0;
			for (uint32_t i = 0; i < out.view.push_constant_member_count; ++i) {
				const APREND_SHADER_MEMBER &member = out.members[out.view.push_constant_first_member + i];
				if (!member.used)
					continue;
				begin = std::min(begin, member.offset);
				end   = std::max(end, member.offset + member.size);
			}
			out.view.push_constant_begin = begin == UINT32_MAX ? 0 : begin;
			out.view.push_constant_end   = end;
		}

		if (stage == SPUDGPU_SHADER_STAGE_VERTEX) {
			for (const spirv_cross::Resource &r : resources.stage_inputs) {
				const spirv_cross::SPIRType &type = compiler.get_type(r.type_id);
				APREND_SHADER_INPUT input{};
				input.name        = keep(compiler.get_name(r.id));
				input.location    = compiler.get_decoration(r.id, spv::DecorationLocation);
				input.scalar_type = aprend_scalar_type(type);
				input.components  = type.vecsize;
				uint32_t locations = type.columns;
				for (size_t dim = 0; dim < type.array.size(); ++dim)
					locations *= type.array[dim] ? type.array[dim] : 1;
				input.location_count = locations;
				input.used           = used(r.id);
				out.inputs.push_back(input);
			}
			std::sort(out.inputs.begin(), out.inputs.end(),
			          [](const APREND_SHADER_INPUT &a, const APREND_SHADER_INPUT &b) { return a.location < b.location; });
		}

		if (stage == SPUDGPU_SHADER_STAGE_FRAGMENT) {
			for (const spirv_cross::Resource &r : resources.stage_outputs) {
				if (!used(r.id))
					continue;
				const spirv_cross::SPIRType &type = compiler.get_type(r.type_id);
				uint32_t locations                = 1;
				for (size_t dim = 0; dim < type.array.size(); ++dim)
					locations *= type.array[dim] ? type.array[dim] : 1;
				const uint32_t first = compiler.get_decoration(r.id, spv::DecorationLocation);
				const uint32_t end   = first + locations;
				if (end > out.view.color_output_count)
					out.view.color_output_count = end;
				if (out.output_types.size() < end)
					out.output_types.resize(end, APREND_SHADER_SCALAR_TYPE_NONE);
				for (uint32_t location = first; location < end; ++location)
					out.output_types[location] = aprend_scalar_type(type);
			}
		}

		if ((stage == SPUDGPU_SHADER_STAGE_COMPUTE || stage == SPUDGPU_SHADER_STAGE_MESH) && out.view.entry_point) {
			// Literal sizes only; one set by a specialization constant reads
			// as 0 here.
			if (compiler.get_execution_mode_bitset().get(spv::ExecutionModeLocalSize)) {
				for (uint32_t axis = 0; axis < 3; ++axis)
					out.view.workgroup_size[axis] = compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, axis);
			}
		}

		out.view.resources      = out.resources.data();
		out.view.resource_count = (uint32_t)out.resources.size();
		out.view.members        = out.members.data();
		out.view.member_count   = (uint32_t)out.members.size();
		out.view.inputs         = out.inputs.data();
		out.view.input_count    = (uint32_t)out.inputs.size();
		return true;
	}
};

} // namespace

bool aprend_reflect_spirv(
    const uint32_t *words,
    size_t word_count,
    SPUDGPU_SHADER_STAGE stage,
    aprend_reflection_store *out,
    std::string *error) noexcept {
	try {
		spirv_cross::Compiler compiler(words, word_count);
		aprend_reflector reflector{compiler, *out, *error};
		return reflector.reflect(stage);
	} catch (const std::bad_alloc &) {
		try {
			*error = "out of memory";
		} catch (...) {
		}
		return false;
	} catch (const std::exception &e) {
		try {
			*error = std::string("the SPIR-V can't be read: ") + e.what();
		} catch (...) {
		}
		return false;
	}
}
