
#include "render/aprendmaterial.h"
#include "aprend_internal.hpp"
#include "aprendimages_internal.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

// Takes a range of [size] bytes of [instance]'s material arena: from a
// buffer that has one free, or a buffer added for it. A block larger than an
// arena buffer gets a buffer of its own. False, printed, if a buffer can't
// be created. Throws std::bad_alloc, taking nothing.
static bool aprend_material_arena_take(
    aprend_instance instance,
    uint32_t size,
    aprend_material_arena_buffer **out_buffer,
    uint32_t *out_offset) {
	const uint32_t alignment = instance->material_block_alignment;
	const uint32_t units     = (size + alignment - 1) / alignment;
	std::vector<aprend_material_arena_buffer *> &buffers = instance->material_arena.buffers;
	if ((uint64_t)units * alignment <= APREND_MATERIAL_ARENA_BUFFER_SIZE) {
		for (aprend_material_arena_buffer *buffer : buffers) {
			uint32_t unit = 0;
			if (buffer->dedicated || !buffer->ranges.take(units, &unit))
				continue;
			*out_buffer = buffer;
			*out_offset = unit * alignment;
			return true;
		}
	}
	// A buffer more: an arena buffer, or one the size of a block too large
	// for one.
	const bool dedicated      = (uint64_t)units * alignment > APREND_MATERIAL_ARENA_BUFFER_SIZE;
	const uint32_t buffer_size = dedicated ? units * alignment : APREND_MATERIAL_ARENA_BUFFER_SIZE;
	aprend_material_arena_buffer *buffer = (aprend_material_arena_buffer *)malloc(sizeof(aprend_material_arena_buffer));
	if (!buffer)
		throw std::bad_alloc();
	buffer = new (buffer) aprend_material_arena_buffer();
	try {
		buffer->ranges.reset(buffer_size / alignment);
		buffers.reserve(buffers.size() + 1);
	} catch (...) {
		buffer->~aprend_material_arena_buffer();
		free(buffer);
		throw;
	}
	buffer->dedicated = dedicated;
	buffer->buffer    = aprend_uniform_buffer_create_device(instance, buffer_size);
	if (!buffer->buffer) {
		buffer->~aprend_material_arena_buffer();
		free(buffer);
		return false;
	}
	uint32_t unit = 0;
	buffer->ranges.take(units, &unit);
	buffers.push_back(buffer); // room was reserved above
	*out_buffer = buffer;
	*out_offset = unit * alignment;
	return true;
}

// Gives back [size] bytes at [offset] of [buffer], a range of [instance]'s
// material arena the GPU has finished with. A dedicated buffer goes with its
// one range. An arena buffer left with no range goes too, unless it is the
// only empty one: that one is kept as a spare, so a material made after the
// last one of a buffer went doesn't make a buffer again.
static void aprend_material_arena_give(
    aprend_instance instance,
    aprend_material_arena_buffer *buffer,
    uint32_t offset,
    uint32_t size) {
	const uint32_t alignment = instance->material_block_alignment;
	const uint32_t units     = (size + alignment - 1) / alignment;
	std::vector<aprend_material_arena_buffer *> &buffers = instance->material_arena.buffers;
	if (!buffer->dedicated) {
		// Next to a free span, or a span between two, it needs no memory;
		// with none it may, and without it the range is lost to the arena,
		// which is all that is lost.
		try {
			if (!buffer->ranges.give(offset / alignment, units))
				printf("aprend: a material's range of the arena was given back twice\n");
		} catch (const std::bad_alloc &) {
			printf("aprend: out of memory: a material's range of the arena is lost until the instance goes\n");
		}
		if (!buffer->ranges.is_whole())
			return;
		bool spare_exists = false;
		for (const aprend_material_arena_buffer *other : buffers)
			if (other != buffer && !other->dedicated && other->ranges.is_whole())
				spare_exists = true;
		if (!spare_exists)
			return;
		// Another is empty already: this one goes, below.
	}
	for (size_t i = 0; i < buffers.size(); ++i) {
		if (buffers[i] == buffer) {
			buffers.erase(buffers.begin() + (std::ptrdiff_t)i);
			break;
		}
	}
	// This is a release function's work: the GPU has finished with the
	// range, and with nothing else in the buffer, so with the buffer.
	aprend_release_handle<aprend_uniform_buffer_t>(buffer->buffer);
	buffer->~aprend_material_arena_buffer();
	free(buffer);
}

void aprend_material_arena_destroy(aprend_instance instance) {
	for (aprend_material_arena_buffer *buffer : instance->material_arena.buffers) {
		aprend_release_handle<aprend_uniform_buffer_t>(buffer->buffer);
		buffer->~aprend_material_arena_buffer();
		free(buffer);
	}
	instance->material_arena.buffers.clear();
}

aprend_material_t::~aprend_material_t() {
	// Released through the queue (aprend_material_destroy), or a failed
	// create's: either way on the thread using the instance, and with the
	// GPU done with whatever was submitted while the material lived, so the
	// ranges go straight back.
	aprend_binding_set_destroy(this->set);
	for (uint32_t i = 0; i < this->slot_count; ++i) {
		aprend_material_slot &slot = this->slots[i];
		if (slot.arena)
			aprend_material_arena_give(this->instance, slot.arena, slot.offset, slot.block_size);
		free(slot.bytes);
	}
	free(this->entries);
	free(this->has_resource);
	free(this->has_sampler);
}

static bool aprend_material_type_reads_view(SPUDGPU_DESCRIPTOR_TYPE type) {
	return type == SPUDGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE || type == SPUDGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
	       type == SPUDGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}
static bool aprend_material_type_reads_sampler(SPUDGPU_DESCRIPTOR_TYPE type) {
	return type == SPUDGPU_DESCRIPTOR_TYPE_SAMPLER || type == SPUDGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}

// Whether [slot] is called [name]: by the layout's name for it, or by its
// block name.
static bool aprend_material_slot_named(const aprend_material_slot &slot, const char *name) {
	if (strcmp(slot.name, name) == 0)
		return true;
	return slot.block_name && strcmp(slot.block_name, name) == 0;
}

// The slot named [name], or NULL, printed for [who].
static aprend_material_slot *aprend_material_find_slot(aprend_material material, const char *name, const char *who) {
	for (uint32_t i = 0; i < material->slot_count; ++i)
		if (aprend_material_slot_named(material->slots[i], name))
			return &material->slots[i];
	printf("apricot: %s: the material's layout has no binding named \"%s\"\n", who, name);
	return nullptr;
}

// The entry of element [array_element] of [slot], or NULL, printed, if the
// binding has no such element.
static aprend_binding_set_entry *aprend_material_slot_entry(
    aprend_material material,
    aprend_material_slot *slot,
    uint32_t array_element,
    const char *who) {
	if (array_element >= slot->binding._count) {
		printf("apricot: %s: binding \"%s\" has %u elements, and element %u was asked for\n", who, slot->name, slot->binding._count, array_element);
		return nullptr;
	}
	return &material->entries[slot->first_entry + array_element];
}

// The first element of [material] that lacks what its type reads: its slot
// and element, and whether it is the sampler it lacks. False if there is
// none.
static bool aprend_material_find_empty(
    aprend_material material,
    const aprend_material_slot **out_slot,
    uint32_t *out_element,
    bool *out_sampler) {
	for (uint32_t s = 0; s < material->slot_count; ++s) {
		const aprend_material_slot &slot = material->slots[s];
		for (uint32_t element = 0; element < slot.binding._count; ++element) {
			const uint32_t i                   = slot.first_entry + element;
			const SPUDGPU_DESCRIPTOR_TYPE type = material->entries[i]._type;
			// A sampler binding reads a sampler and nothing else.
			const bool needs_resource = type != SPUDGPU_DESCRIPTOR_TYPE_SAMPLER;
			const bool needs_sampler  = aprend_material_type_reads_sampler(type);
			const bool lacks_resource = needs_resource && !material->has_resource[i];
			const bool lacks_sampler  = needs_sampler && !material->has_sampler[i];
			if (!lacks_resource && !lacks_sampler)
				continue;
			*out_slot    = &slot;
			*out_element = element;
			*out_sampler = !lacks_resource;
			return true;
		}
	}
	return false;
}

// Gives [slot] of [material], an owned block of [block_size] bytes, its
// bytes ([initial], or zeroes if NULL) and a range of the instance's
// material arena, and points its entry at the range. The bytes go across
// at the first submission: a new range's contents are undefined. False,
// printed for [who], with the slot holding what it got as far as, for the
// material's destructor to give back.
static bool aprend_material_own_block(
    aprend_material material,
    aprend_material_slot &slot,
    uint32_t block_size,
    const uint8_t *initial,
    const char *who) {
	slot.bytes = (uint8_t *)calloc(block_size, 1);
	if (!slot.bytes) {
		printf("apricot: %s: out of memory\n", who);
		return false;
	}
	if (initial)
		memcpy(slot.bytes, initial, block_size);
	bool taken = false;
	try {
		taken = aprend_material_arena_take(material->instance, block_size, &slot.arena, &slot.offset);
	} catch (const std::bad_alloc &) {
		printf("apricot: %s: out of memory\n", who);
	}
	if (!taken)
		return false;
	slot.block_size                          = block_size;
	slot.dirty                               = true;
	aprend_binding_set_entry &entry          = material->entries[slot.first_entry];
	entry._resource._uniform._buffer         = slot.arena->buffer;
	entry._resource._uniform._set            = nullptr;
	entry._resource._uniform._offset         = slot.offset;
	entry._resource._uniform._range          = block_size;
	material->has_resource[slot.first_entry] = true;
	return true;
}

bool aprend_material_prepare(aprend_material material) {
	const aprend_material_slot *empty = nullptr;
	uint32_t element                  = 0;
	bool sampler                      = false;
	if (aprend_material_find_empty(material, &empty, &element, &sampler)) {
		const char *missing = sampler ? "sampler" : aprend_material_type_reads_view(empty->binding._type) ? "texture view" : "buffer";
		printf("aprend: SET_MATERIAL with a material whose binding \"%s\" (element %u) has no %s: set it, or start the material as a copy of one "
		       "that has every binding set (aprend_material_copy)\n",
		       empty->name, element, missing);
		return false;
	}
	if (!material->set) {
		material->set = aprend_binding_set_create(material->layout, material->entries, material->entry_count);
		if (!material->set) {
			printf("aprend: SET_MATERIAL: the material's binding set can't be created\n");
			return false;
		}
		material->resources_changed = false;
		return true;
	}
	if (!material->resources_changed)
		return true;
	// Every element, changed or not: what the set was built from is the
	// material's entries, so this is the set as the material holds it now.
	if (!aprend_binding_set_update(material->set, material->entries, material->entry_count)) {
		printf("aprend: SET_MATERIAL: the material's binding set can't be built again\n");
		return false;
	}
	material->resources_changed = false;
	return true;
}

bool aprend_material_stage_uploads(aprend_material material, std::vector<aprend_upload_copy> &copies) {
	for (uint32_t i = 0; i < material->slot_count; ++i) {
		aprend_material_slot &slot = material->slots[i];
		if (!slot.arena || !slot.dirty)
			continue;
		// The bytes as they are now, in this frame's upload memory, which
		// outlives the submission's reading of it; copied into the range by
		// the GPU, in order with everything else on the queue.
		aprend_transient_span span{};
		if (!aprend_transient_allocate(material->instance, slot.block_size, 16, &span)) {
			printf("aprend: submit: transient memory can't be taken for the uniform block of material binding \"%s\"\n", slot.name);
			return false;
		}
		memcpy(span.data, slot.bytes, slot.block_size);
		const aprend_transient_chunk *chunk = (const aprend_transient_chunk *)span._chunk;
		copies.push_back({chunk->buffer, span._offset, slot.arena->buffer->buffer, slot.offset, slot.block_size, &slot.dirty});
	}
	return true;
}

extern "C" {

aprend_material aprend_material_create(
    aprend_instance instance,
    const aprend_material_desc *desc) {
	static const char *who = "aprend_material_create";
	if (!instance) {
		printf("apricot: %s: instance is NULL\n", who);
		return nullptr;
	}
	if (!desc) {
		printf("apricot: %s: desc is NULL\n", who);
		return nullptr;
	}
	if (!desc->_layout) {
		printf("apricot: %s: _layout is NULL\n", who);
		return nullptr;
	}
	if (desc->_layout->instance != instance) {
		printf("apricot: %s: _layout is of another instance\n", who);
		return nullptr;
	}
	if (desc->_external_block_count > 0 && !desc->_external_blocks) {
		printf("apricot: %s: _external_blocks is NULL with a count of %u\n", who, desc->_external_block_count);
		return nullptr;
	}
	aprend_binding_layout layout = desc->_layout;
	// Every binding is set by its name, so each has one, and only one has it.
	for (uint32_t i = 0; i < layout->binding_count; ++i) {
		if (layout->names[i].empty()) {
			printf("apricot: %s: binding %u of the layout has no name: give the layout's bindings _name, or make it from shaders\n", who,
			       layout->bindings[i]._binding);
			return nullptr;
		}
		for (uint32_t j = 0; j < i; ++j) {
			if (layout->names[j] == layout->names[i]) {
				printf("apricot: %s: two bindings of the layout are named \"%s\"\n", who, layout->names[i].c_str());
				return nullptr;
			}
		}
	}
	// Each external block a uniform buffer binding.
	for (uint32_t e = 0; e < desc->_external_block_count; ++e) {
		const char *name = desc->_external_blocks[e];
		if (!name) {
			printf("apricot: %s: _external_blocks[%u] is NULL\n", who, e);
			return nullptr;
		}
		bool found = false;
		for (uint32_t i = 0; i < layout->binding_count && !found; ++i) {
			const bool named = layout->names[i] == name || (layout->blocks[i].known && strcmp(layout->blocks[i].block_name, name) == 0);
			if (named && layout->bindings[i]._type == SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
				found = true;
		}
		if (!found) {
			printf("apricot: %s: _external_blocks[%u] \"%s\" is not a uniform buffer binding of the layout\n", who, e, name);
			return nullptr;
		}
	}

	APREND_MALLOC__T(result, aprend_material_t);
	if (!result)
		return nullptr;
	APREND_CONSTRUCT__T(result, aprend_material_t);
	result->instance    = instance;
	result->layout      = layout;
	result->entry_count = layout->descriptor_count;

	result->entries      = (aprend_binding_set_entry *)calloc(result->entry_count, sizeof(aprend_binding_set_entry));
	result->has_resource = (bool *)calloc(result->entry_count, sizeof(bool));
	result->has_sampler  = (bool *)calloc(result->entry_count, sizeof(bool));
	if (!result->entries || !result->has_resource || !result->has_sampler) {
		APREND_DESTRUCT__T(result, aprend_material_t);
		return nullptr;
	}

	uint32_t next_entry = 0;
	for (uint32_t i = 0; i < layout->binding_count; ++i) {
		aprend_material_slot &slot = result->slots[i];
		// slot_count goes up first, so the destructor frees what this slot
		// has taken if a later step fails.
		++result->slot_count;
		slot.binding     = layout->bindings[i];
		slot.name        = layout->names[i].c_str();
		slot.block_name  = layout->blocks[i].known ? layout->blocks[i].block_name : nullptr;
		slot.first_entry = next_entry;
		next_entry += slot.binding._count;

		for (uint32_t element = 0; element < slot.binding._count; ++element) {
			aprend_binding_set_entry &entry = result->entries[slot.first_entry + element];
			entry._binding                  = slot.binding._binding;
			entry._array_element            = element;
			entry._type                     = slot.binding._type;
		}

		// Owned: a uniform block the layout knows, one buffer, not external.
		aprend_uniform_layout uniforms{};
		if (slot.binding._type != SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
			continue;
		if (slot.binding._count != 1)
			continue;
		if (!aprend_binding_layout_get_uniform_layout(layout, slot.binding._binding, &uniforms))
			continue;
		bool external = false;
		for (uint32_t e = 0; e < desc->_external_block_count; ++e)
			if (aprend_material_slot_named(slot, desc->_external_blocks[e]))
				external = true;
		if (external)
			continue;

		// Its bytes are copied into its range at submit, from the frame's
		// transient memory.
		if (instance->desc.transient_chunk_size == 0) {
			printf("apricot: %s: \"%s\" is a block the material owns, and the instance has no transient memory to copy it through\n", who,
			       slot.name);
			APREND_DESTRUCT__T(result, aprend_material_t);
			return nullptr;
		}
		// A range of the arena for the material's life, the block's size
		// rounded up to 16 as std140 rounds a block.
		slot.members      = layout->members.data() + layout->blocks[i].first_member;
		slot.member_count = layout->blocks[i].member_count;
		if (uniforms.size > UINT32_MAX - 15) {
			printf("apricot: %s: the block of \"%s\" is larger than 32 bits can address\n", who, slot.name);
			APREND_DESTRUCT__T(result, aprend_material_t);
			return nullptr;
		}
		const uint32_t block_size = uniforms.size ? (uniforms.size + 15) & ~15u : 16u;
		if (!aprend_material_own_block(result, slot, block_size, nullptr, who)) {
			APREND_DESTRUCT__T(result, aprend_material_t);
			return nullptr;
		}
	}
	return result;
}

aprend_material aprend_material_copy(aprend_material source) {
	static const char *who = "aprend_material_copy";
	if (!source) {
		printf("apricot: %s: source is NULL\n", who);
		return nullptr;
	}

	APREND_MALLOC__T(result, aprend_material_t);
	if (!result)
		return nullptr;
	APREND_CONSTRUCT__T(result, aprend_material_t);
	result->instance    = source->instance;
	result->layout      = source->layout;
	result->entry_count = source->entry_count;
	// Not the source's binding set, nor its lit scene: a set of its own is
	// built at the first compile that binds the copy, and a lit scene's
	// material is the scene's alone.

	result->entries      = (aprend_binding_set_entry *)calloc(result->entry_count, sizeof(aprend_binding_set_entry));
	result->has_resource = (bool *)calloc(result->entry_count, sizeof(bool));
	result->has_sampler  = (bool *)calloc(result->entry_count, sizeof(bool));
	if (!result->entries || !result->has_resource || !result->has_sampler) {
		printf("apricot: %s: out of memory\n", who);
		APREND_DESTRUCT__T(result, aprend_material_t);
		return nullptr;
	}
	// Every resource the source holds, the same handles: they are the
	// caller's, shared by both.
	memcpy(result->entries, source->entries, sizeof(aprend_binding_set_entry) * result->entry_count);
	memcpy(result->has_resource, source->has_resource, sizeof(bool) * result->entry_count);
	memcpy(result->has_sampler, source->has_sampler, sizeof(bool) * result->entry_count);

	for (uint32_t i = 0; i < source->slot_count; ++i) {
		const aprend_material_slot &from = source->slots[i];
		aprend_material_slot &slot       = result->slots[i];
		// slot_count goes up first, so the destructor gives back what this
		// slot has taken if a later step fails.
		++result->slot_count;
		slot.name         = from.name;
		slot.block_name   = from.block_name;
		slot.binding      = from.binding;
		slot.first_entry  = from.first_entry;
		slot.members      = from.members;
		slot.member_count = from.member_count;
		if (!from.arena)
			continue;
		// An owned block: the source's values, in a range of its own.
		if (!aprend_material_own_block(result, slot, from.block_size, from.bytes, who)) {
			APREND_DESTRUCT__T(result, aprend_material_t);
			return nullptr;
		}
	}
	return result;
}

void aprend_material_destroy(aprend_material material) {
	if (!material)
		return;
	// A list compiled against it and not submitted would write its blocks.
	aprend_instance_forget(material->instance, material);
	// The whole material goes through the release queue: its ranges of the
	// arena are given back once the GPU has finished with everything
	// submitted up to now, which may read them.
	aprend_instance_retire(material->instance, material, &aprend_release_handle<aprend_material_t>);
}

aprend_binding_layout aprend_material_get_layout(aprend_material material) { return material ? material->layout : nullptr; }

bool aprend_material_is_complete(aprend_material material) {
	if (!material)
		return false;
	const aprend_material_slot *slot = nullptr;
	uint32_t element                 = 0;
	bool sampler                     = false;
	return !aprend_material_find_empty(material, &slot, &element, &sampler);
}

// Whether binding [index] of [layout] is a block a material of it can own:
// a uniform buffer, one buffer, whose block the layout knows.
static bool aprend_layout_has_ownable_block(aprend_binding_layout layout, uint32_t index) {
	const aprend_binding_desc &binding = layout->bindings[index];
	return binding._type == SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER && binding._count == 1 && layout->blocks[index].known;
}

// The member of [uniform], or NULL, printed for [who], if it names none of
// [material]'s layout or one in a block the material doesn't own.
// [out_slot] receives the block's slot.
static const aprend_shader_member *aprend_material_uniform_member(
    aprend_material material,
    const aprend_material_uniform &uniform,
    aprend_material_slot **out_slot,
    const char *who) {
	if (!uniform._layout) {
		printf("apricot: %s: uniform is no uniform (aprend_material_find_uniform fills one)\n", who);
		return nullptr;
	}
	if (uniform._layout != material->layout) {
		printf("apricot: %s: uniform was found in another layout than the material's\n", who);
		return nullptr;
	}
	// Found in this layout, so in range; checked all the same, since the
	// fields are the caller's to have copied.
	if (uniform._binding_index >= material->slot_count) {
		printf("apricot: %s: uniform names binding %u, and the layout has %u\n", who, uniform._binding_index, material->slot_count);
		return nullptr;
	}
	aprend_material_slot &slot = material->slots[uniform._binding_index];
	if (!slot.arena) {
		printf("apricot: %s: binding \"%s\" is a block the material doesn't own (it was named in _external_blocks)\n", who, slot.name);
		return nullptr;
	}
	if (uniform._member_index >= slot.member_count) {
		printf("apricot: %s: uniform names member %u of \"%s\", which has %u\n", who, uniform._member_index, slot.name, slot.member_count);
		return nullptr;
	}
	const aprend_shader_member &member = slot.members[uniform._member_index];
	if (member.type == APREND_UNIFORM_TYPE_UNKNOWN) {
		printf("apricot: %s: member \"%s\" has a type with no APREND_UNIFORM_TYPE: write it with aprend_material_set_uniform_bytes\n", who,
		       member.name);
		return nullptr;
	}
	if (member.array_count == 0) {
		printf("apricot: %s: member \"%s\" is an array without a size\n", who, member.name);
		return nullptr;
	}
	*out_slot = &slot;
	return &member;
}

bool aprend_material_find_uniform(
    aprend_binding_layout layout,
    const char *name,
    aprend_material_uniform *out_uniform,
    aprend_shader_member *out_member) {
	static const char *who = "aprend_material_find_uniform";
	if (!layout) {
		printf("apricot: %s: layout is NULL\n", who);
		return false;
	}
	if (!name) {
		printf("apricot: %s: name is NULL\n", who);
		return false;
	}
	if (!out_uniform) {
		printf("apricot: %s: out_uniform is NULL\n", who);
		return false;
	}
	// Qualified by the binding's name or its block's: one block, so one
	// member at most.
	for (uint32_t i = 0; i < layout->binding_count; ++i) {
		if (!aprend_layout_has_ownable_block(layout, i))
			continue;
		const aprend_binding_layout_t::block_info &block = layout->blocks[i];
		const char *qualifiers[2]                        = {layout->names[i].c_str(), block.block_name};
		for (const char *qualifier : qualifiers) {
			if (!qualifier || !qualifier[0])
				continue;
			const size_t length = strlen(qualifier);
			if (strncmp(name, qualifier, length) != 0 || name[length] != '.')
				continue;
			for (uint32_t m = 0; m < block.member_count; ++m) {
				const aprend_shader_member &member = layout->members[block.first_member + m];
				if (strcmp(member.name, name + length + 1) != 0)
					continue;
				*out_uniform = {layout, i, m};
				if (out_member)
					*out_member = member;
				return true;
			}
		}
	}
	// The path alone: in exactly one block.
	bool found                = false;
	aprend_material_uniform at{};
	for (uint32_t i = 0; i < layout->binding_count; ++i) {
		if (!aprend_layout_has_ownable_block(layout, i))
			continue;
		const aprend_binding_layout_t::block_info &block = layout->blocks[i];
		for (uint32_t m = 0; m < block.member_count; ++m) {
			if (strcmp(layout->members[block.first_member + m].name, name) != 0)
				continue;
			if (found) {
				printf("apricot: %s: \"%s\" is a member of more than one block of the layout: name it after its binding, as \"%s.%s\"\n", who,
				       name, layout->names[i].c_str(), name);
				return false;
			}
			found = true;
			at    = {layout, i, m};
		}
	}
	if (!found) {
		printf("apricot: %s: no uniform block of the layout has a member \"%s\"\n", who, name);
		return false;
	}
	*out_uniform = at;
	if (out_member)
		*out_member = layout->members[layout->blocks[at._binding_index].first_member + at._member_index];
	return true;
}

bool aprend_material_set_uniform_value(
    aprend_material material,
    aprend_material_uniform uniform,
    const void *data) {
	static const char *who = "aprend_material_set_uniform_value";
	if (!material) {
		printf("apricot: %s: material is NULL\n", who);
		return false;
	}
	if (!data) {
		printf("apricot: %s: data is NULL\n", who);
		return false;
	}
	aprend_material_slot *slot         = nullptr;
	const aprend_shader_member *member = aprend_material_uniform_member(material, uniform, &slot, who);
	if (!member)
		return false;
	// The block was sized from these members, so each one fits.
	aprend_write_block_member(*member, slot->bytes, data);
	slot->dirty = true;
	return true;
}

bool aprend_material_get_uniform_value(
    aprend_material material,
    aprend_material_uniform uniform,
    void *out_data) {
	static const char *who = "aprend_material_get_uniform_value";
	if (!material) {
		printf("apricot: %s: material is NULL\n", who);
		return false;
	}
	if (!out_data) {
		printf("apricot: %s: out_data is NULL\n", who);
		return false;
	}
	aprend_material_slot *slot         = nullptr;
	const aprend_shader_member *member = aprend_material_uniform_member(material, uniform, &slot, who);
	if (!member)
		return false;
	aprend_read_block_member(*member, slot->bytes, out_data);
	return true;
}

bool aprend_material_set_uniform(
    aprend_material material,
    const char *name,
    const void *data) {
	static const char *who = "aprend_material_set_uniform";
	if (!material) {
		printf("apricot: %s: material is NULL\n", who);
		return false;
	}
	if (!name) {
		printf("apricot: %s: name is NULL\n", who);
		return false;
	}
	if (!data) {
		printf("apricot: %s: data is NULL\n", who);
		return false;
	}
	aprend_material_uniform uniform{};
	if (!aprend_material_find_uniform(material->layout, name, &uniform, nullptr))
		return false;
	return aprend_material_set_uniform_value(material, uniform, data);
}

bool aprend_material_set_uniform_bytes(
    aprend_material material,
    const char *name,
    uint32_t offset,
    uint32_t size,
    const void *data) {
	static const char *who = "aprend_material_set_uniform_bytes";
	if (!material) {
		printf("apricot: %s: material is NULL\n", who);
		return false;
	}
	if (!name) {
		printf("apricot: %s: name is NULL\n", who);
		return false;
	}
	if (size == 0) {
		printf("apricot: %s: size is 0\n", who);
		return false;
	}
	if (!data) {
		printf("apricot: %s: data is NULL\n", who);
		return false;
	}
	aprend_material_slot *slot = aprend_material_find_slot(material, name, who);
	if (!slot)
		return false;
	if (!slot->arena) {
		printf("apricot: %s: \"%s\" is not a block the material owns\n", who, name);
		return false;
	}
	if ((uint64_t)offset + size > slot->block_size) {
		printf("apricot: %s: %u bytes at offset %u run past the %u bytes of \"%s\"\n", who, size, offset, slot->block_size, name);
		return false;
	}
	memcpy(slot->bytes + offset, data, size);
	slot->dirty = true;
	return true;
}

bool aprend_material_set_uniform_buffer(
    aprend_material material,
    const char *name,
    uint32_t array_element,
    aprend_uniform_buffer buffer,
    aprend_uniform_buffer_set set) {
	static const char *who = "aprend_material_set_uniform_buffer";
	if (!material) {
		printf("apricot: %s: material is NULL\n", who);
		return false;
	}
	if (!name) {
		printf("apricot: %s: name is NULL\n", who);
		return false;
	}
	aprend_material_slot *slot = aprend_material_find_slot(material, name, who);
	if (!slot)
		return false;
	if (slot->binding._type != SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
		printf("apricot: %s: binding \"%s\" is not a uniform buffer binding\n", who, name);
		return false;
	}
	if (slot->arena) {
		printf("apricot: %s: binding \"%s\" is a block the material owns\n", who, name);
		return false;
	}
	aprend_binding_set_entry *entry = aprend_material_slot_entry(material, slot, array_element, who);
	if (!entry)
		return false;
	if (!buffer && !set) {
		printf("apricot: %s: neither buffer nor set is given\n", who);
		return false;
	}
	if (buffer && set) {
		printf("apricot: %s: both buffer and set are given\n", who);
		return false;
	}
	if (buffer && buffer->instance != material->instance) {
		printf("apricot: %s: buffer is of another instance\n", who);
		return false;
	}
	if (set && set->instance != material->instance) {
		printf("apricot: %s: set is of another instance\n", who);
		return false;
	}
	entry->_resource._uniform._buffer = buffer;
	entry->_resource._uniform._set    = set;
	entry->_resource._uniform._offset = 0;
	entry->_resource._uniform._range  = 0;
	material->has_resource[entry - material->entries] = true;
	material->resources_changed                       = true;
	return true;
}

bool aprend_material_set_storage_buffer(
    aprend_material material,
    const char *name,
    uint32_t array_element,
    aprend_storage_buffer buffer,
    aprend_storage_buffer_set set) {
	static const char *who = "aprend_material_set_storage_buffer";
	if (!material) {
		printf("apricot: %s: material is NULL\n", who);
		return false;
	}
	if (!name) {
		printf("apricot: %s: name is NULL\n", who);
		return false;
	}
	aprend_material_slot *slot = aprend_material_find_slot(material, name, who);
	if (!slot)
		return false;
	if (slot->binding._type != SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
		printf("apricot: %s: binding \"%s\" is not a storage buffer binding\n", who, name);
		return false;
	}
	aprend_binding_set_entry *entry = aprend_material_slot_entry(material, slot, array_element, who);
	if (!entry)
		return false;
	if (!buffer && !set) {
		printf("apricot: %s: neither buffer nor set is given\n", who);
		return false;
	}
	if (buffer && set) {
		printf("apricot: %s: both buffer and set are given\n", who);
		return false;
	}
	if (buffer && buffer->instance != material->instance) {
		printf("apricot: %s: buffer is of another instance\n", who);
		return false;
	}
	if (set && set->instance != material->instance) {
		printf("apricot: %s: set is of another instance\n", who);
		return false;
	}
	entry->_resource._storage._buffer = buffer;
	entry->_resource._storage._set    = set;
	entry->_resource._storage._offset = 0;
	entry->_resource._storage._range  = 0;
	material->has_resource[entry - material->entries] = true;
	material->resources_changed                       = true;
	return true;
}

bool aprend_material_set_texture(
    aprend_material material,
    const char *name,
    uint32_t array_element,
    aprend_texture_view view) {
	static const char *who = "aprend_material_set_texture";
	if (!material) {
		printf("apricot: %s: material is NULL\n", who);
		return false;
	}
	if (!name) {
		printf("apricot: %s: name is NULL\n", who);
		return false;
	}
	if (!view) {
		printf("apricot: %s: view is NULL\n", who);
		return false;
	}
	aprend_material_slot *slot = aprend_material_find_slot(material, name, who);
	if (!slot)
		return false;
	if (!aprend_material_type_reads_view(slot->binding._type)) {
		printf("apricot: %s: binding \"%s\" is not a sampled image, storage image or combined image sampler binding\n", who, name);
		return false;
	}
	aprend_binding_set_entry *entry = aprend_material_slot_entry(material, slot, array_element, who);
	if (!entry)
		return false;
	if (view->instance != material->instance) {
		printf("apricot: %s: view is of another instance\n", who);
		return false;
	}
	const APREND_TEXTURE_VIEW_TYPE wanted = slot->binding._type == SPUDGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE ? APREND_TEXTURE_VIEW_TYPE_UNORDERED_ACCESS
	                                                                                                      : APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE;
	if (view->view_type != wanted) {
		printf("apricot: %s: binding \"%s\" takes a view of type %u, and view is of type %u\n", who, name, (unsigned)wanted, (unsigned)view->view_type);
		return false;
	}
	entry->_resource._image._view                     = view;
	material->has_resource[entry - material->entries] = true;
	material->resources_changed                       = true;
	return true;
}

bool aprend_material_set_sampler(
    aprend_material material,
    const char *name,
    uint32_t array_element,
    aprend_sampler sampler) {
	static const char *who = "aprend_material_set_sampler";
	if (!material) {
		printf("apricot: %s: material is NULL\n", who);
		return false;
	}
	if (!name) {
		printf("apricot: %s: name is NULL\n", who);
		return false;
	}
	if (!sampler) {
		printf("apricot: %s: sampler is NULL\n", who);
		return false;
	}
	aprend_material_slot *slot = aprend_material_find_slot(material, name, who);
	if (!slot)
		return false;
	if (!aprend_material_type_reads_sampler(slot->binding._type)) {
		printf("apricot: %s: binding \"%s\" is not a sampler or combined image sampler binding\n", who, name);
		return false;
	}
	aprend_binding_set_entry *entry = aprend_material_slot_entry(material, slot, array_element, who);
	if (!entry)
		return false;
	if (sampler->instance != material->instance) {
		printf("apricot: %s: sampler is of another instance\n", who);
		return false;
	}
	entry->_resource._image._sampler                 = sampler;
	material->has_sampler[entry - material->entries] = true;
	material->resources_changed                      = true;
	return true;
}

} // extern "C"
