
#include "render/rbtrendmaterial.h"
#include "rbtrend_internal.hpp"
#include "rbtrendimages_internal.hpp"
#include "rbtrendraytracing_internal.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

// Takes a range of [size] bytes of [instance]'s material arena: from a
// buffer that has one free, or a buffer added for it. A block larger than an
// arena buffer gets a buffer of its own. False, reported, if a buffer can't
// be created. Throws std::bad_alloc, taking nothing.
static bool rbtrend_material_arena_take(
    rbtrend_instance instance,
    uint32_t size,
    rbtrend_material_arena_buffer **out_buffer,
    uint32_t *out_offset) {
	const uint32_t alignment = instance->material_block_alignment;
	const uint32_t units     = (size + alignment - 1) / alignment;
	std::vector<rbtrend_material_arena_buffer *> &buffers = instance->material_arena.buffers;
	if ((uint64_t)units * alignment <= RBTREND_MATERIAL_ARENA_BUFFER_SIZE) {
		for (rbtrend_material_arena_buffer *buffer : buffers) {
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
	const bool dedicated      = (uint64_t)units * alignment > RBTREND_MATERIAL_ARENA_BUFFER_SIZE;
	const uint32_t buffer_size = dedicated ? units * alignment : RBTREND_MATERIAL_ARENA_BUFFER_SIZE;
	rbtrend_material_arena_buffer *buffer = (rbtrend_material_arena_buffer *)malloc(sizeof(rbtrend_material_arena_buffer));
	if (!buffer)
		throw std::bad_alloc();
	buffer = new (buffer) rbtrend_material_arena_buffer();
	try {
		buffer->ranges.reset(buffer_size / alignment);
		buffers.reserve(buffers.size() + 1);
	} catch (...) {
		buffer->~rbtrend_material_arena_buffer();
		free(buffer);
		throw;
	}
	buffer->dedicated = dedicated;
	if (rbtrend_buffer_create_device_uniform(instance, buffer_size, &buffer->buffer, nullptr) != RBT_RESULT_OK) {
		buffer->~rbtrend_material_arena_buffer();
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
static void rbtrend_material_arena_give(
    rbtrend_instance instance,
    rbtrend_material_arena_buffer *buffer,
    uint32_t offset,
    uint32_t size) {
	const uint32_t alignment = instance->material_block_alignment;
	const uint32_t units     = (size + alignment - 1) / alignment;
	std::vector<rbtrend_material_arena_buffer *> &buffers = instance->material_arena.buffers;
	if (!buffer->dedicated) {
		// Next to a free span, or a span between two, it needs no memory;
		// with none it may, and without it the range is lost to the arena,
		// which is all that is lost.
		try {
			if (!buffer->ranges.give(offset / alignment, units))
				rbtrend_report(instance, RBT_RESULT_INVALID_ARGUMENT, "a material's range of the arena was given back twice");
		} catch (const std::bad_alloc &) {
			rbtrend_report(instance, RBT_RESULT_OUT_OF_MEMORY, "out of memory: a material's range of the arena is lost until the instance goes");
		}
		if (!buffer->ranges.is_whole())
			return;
		bool spare_exists = false;
		for (const rbtrend_material_arena_buffer *other : buffers)
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
	rbtrend_release_handle<rbtrend_buffer_t>(buffer->buffer);
	buffer->~rbtrend_material_arena_buffer();
	free(buffer);
}

void rbtrend_material_arena_destroy(rbtrend_instance instance) {
	for (rbtrend_material_arena_buffer *buffer : instance->material_arena.buffers) {
		rbtrend_release_handle<rbtrend_buffer_t>(buffer->buffer);
		buffer->~rbtrend_material_arena_buffer();
		free(buffer);
	}
	instance->material_arena.buffers.clear();
}

rbtrend_material_t::~rbtrend_material_t() {
	// Released through the queue (rbtrend_material_destroy), or a failed
	// create's: either way on the thread using the instance, and with the
	// GPU done with whatever was submitted while the material lived, so the
	// ranges go straight back.
	rbtrend_binding_set_destroy(this->set);
	// A lit material's entry in the lit-material table, its generation
	// advanced (D4).
	rbtrend_rt_lit_give(this->instance, this->rt_lit_slot);
	for (uint32_t i = 0; i < this->slot_count; ++i) {
		rbtrend_material_slot &slot = this->slots[i];
		if (slot.arena)
			rbtrend_material_arena_give(this->instance, slot.arena, slot.offset, slot.block_size);
		free(slot.bytes);
	}
	free(this->entries);
	free(this->resource_serial);
	free(this->sampler_serial);
}

static bool rbtrend_material_type_reads_view(CRGPU_DESCRIPTOR_TYPE type) {
	return type == CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE || type == CRGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
	       type == CRGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}
static bool rbtrend_material_type_reads_sampler(CRGPU_DESCRIPTOR_TYPE type) {
	return type == CRGPU_DESCRIPTOR_TYPE_SAMPLER || type == CRGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}

// Whether [slot] is called [name]: by the layout's name for it, or by its
// block name.
static bool rbtrend_material_slot_named(const rbtrend_material_slot &slot, const char *name) {
	if (strcmp(slot.name, name) == 0)
		return true;
	return slot.block_name && strcmp(slot.block_name, name) == 0;
}

// The slot named [name], through [out_slot]. RBT_RESULT_NOT_FOUND, [error]
// saying so, if there is none.
static RBT_RESULT rbtrend_material_find_slot(rbtrend_material material, const char *name, rbtrend_material_slot **out_slot, RBTREND_ERROR *error) {
	for (uint32_t i = 0; i < material->slot_count; ++i) {
		if (rbtrend_material_slot_named(material->slots[i], name)) {
			*out_slot = &material->slots[i];
			return RBT_RESULT_OK;
		}
	}
	return rbtrend_fail(error, RBT_RESULT_NOT_FOUND, "the material's layout has no binding named \"%s\"", name);
}

// The entry of element [array_element] of [slot], through [out_entry].
// RBT_RESULT_INVALID_ARGUMENT, [error] saying why, if the binding has no such
// element.
static RBT_RESULT rbtrend_material_slot_entry(
    rbtrend_material material,
    rbtrend_material_slot *slot,
    uint32_t array_element,
    RBTREND_BINDING_SET_ENTRY **out_entry,
    RBTREND_ERROR *error) {
	if (array_element >= slot->binding._count)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "binding \"%s\" has %u elements, and element %u was asked for", slot->name, slot->binding._count,
		                   array_element);
	*out_entry = &material->entries[slot->first_entry + array_element];
	return RBT_RESULT_OK;
}

// The first element of [material] that lacks what its type reads: its slot
// and element, and whether it is the sampler it lacks. False if there is
// none.
static bool rbtrend_material_find_empty(
    rbtrend_material material,
    const rbtrend_material_slot **out_slot,
    uint32_t *out_element,
    bool *out_sampler) {
	for (uint32_t s = 0; s < material->slot_count; ++s) {
		const rbtrend_material_slot &slot = material->slots[s];
		for (uint32_t element = 0; element < slot.binding._count; ++element) {
			const uint32_t i                   = slot.first_entry + element;
			const CRGPU_DESCRIPTOR_TYPE type = material->entries[i]._type;
			// A sampler binding reads a sampler and nothing else.
			const bool needs_resource = type != CRGPU_DESCRIPTOR_TYPE_SAMPLER;
			const bool needs_sampler  = rbtrend_material_type_reads_sampler(type);
			const bool lacks_resource = needs_resource && material->resource_serial[i] == 0;
			const bool lacks_sampler  = needs_sampler && material->sampler_serial[i] == 0;
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
// at the first submission: a new range's contents are undefined. Fails,
// [error] saying why, with the slot holding what it got as far as, for the
// material's destructor to give back.
static RBT_RESULT rbtrend_material_own_block(
    rbtrend_material material,
    rbtrend_material_slot &slot,
    uint32_t block_size,
    const uint8_t *initial,
    RBTREND_ERROR *error) {
	slot.bytes = (uint8_t *)calloc(block_size, 1);
	if (!slot.bytes)
		return rbtrend_fail(error, RBT_RESULT_OUT_OF_MEMORY, "no memory for the %u bytes of \"%s\"", block_size, slot.name);
	if (initial)
		memcpy(slot.bytes, initial, block_size);
	bool taken = false;
	try {
		taken = rbtrend_material_arena_take(material->instance, block_size, &slot.arena, &slot.offset);
	} catch (const std::bad_alloc &) {
		return rbtrend_fail(error, RBT_RESULT_OUT_OF_MEMORY, "no memory for a range of the material arena");
	}
	if (!taken)
		return rbtrend_fail(error, RBT_RESULT_REND_GPU_FAILURE, "a buffer of the material arena can't be created for \"%s\"", slot.name);
	slot.block_size                          = block_size;
	slot.dirty                               = true;
	RBTREND_BINDING_SET_ENTRY &entry          = material->entries[slot.first_entry];
	entry._resource._uniform._buffer         = slot.arena->buffer;
	entry._resource._uniform._offset         = slot.offset;
	entry._resource._uniform._range          = block_size;
	material->resource_serial[slot.first_entry] = RBTREND_SERIAL_OWNED;
	return RBT_RESULT_OK;
}

bool rbtrend_material_prepare(rbtrend_material material) {
	const rbtrend_material_slot *empty = nullptr;
	uint32_t element                  = 0;
	bool sampler                      = false;
	if (rbtrend_material_find_empty(material, &empty, &element, &sampler)) {
		const char *missing = sampler ? "sampler" : rbtrend_material_type_reads_view(empty->binding._type) ? "texture view" : "buffer";
		rbtrend_report(material->instance, RBT_RESULT_INVALID_ARGUMENT, "SET_MATERIAL with a material whose binding \"%s\" (element %u) has no %s: set it, or start the material as a copy of one that has every binding set (rbtrend_material_copy)", empty->name, element, missing);
		return false;
	}
	if (!material->set) {
		(void)rbtrend_binding_set_create(material->layout, material->entries, material->entry_count, &material->set, nullptr);
		if (!material->set) {
			rbtrend_report(material->instance, RBT_RESULT_REND_GPU_FAILURE, "SET_MATERIAL: the material's binding set can't be created");
			return false;
		}
		material->resources_changed = false;
		return true;
	}
	if (!material->resources_changed)
		return true;
	// Every element, changed or not: what the set was built from is the
	// material's entries, so this is the set as the material holds it now.
	if (!(rbtrend_binding_set_update(material->set, material->entries, material->entry_count, nullptr) == RBT_RESULT_OK)) {
		rbtrend_report(material->instance, RBT_RESULT_REND_GPU_FAILURE, "SET_MATERIAL: the material's binding set can't be built again");
		return false;
	}
	material->resources_changed = false;
	return true;
}

bool rbtrend_material_stage_uploads(rbtrend_material material, eq::array<rbtrend_upload_copy> &copies) {
	for (uint32_t i = 0; i < material->slot_count; ++i) {
		rbtrend_material_slot &slot = material->slots[i];
		if (!slot.arena || !slot.dirty)
			continue;
		// The bytes as they are now, in this frame's upload memory, which
		// outlives the submission's reading of it; copied into the range by
		// the GPU, in order with everything else on the queue.
		RBTREND_TRANSIENT_SPAN span{};
		if (rbtrend_transient_allocate(material->instance, slot.block_size, 16, &span, nullptr) != RBT_RESULT_OK) {
			rbtrend_report(material->instance, RBT_RESULT_REND_GPU_FAILURE, "submit: transient memory can't be taken for the uniform block of material binding \"%s\"", slot.name);
			return false;
		}
		memcpy(span.data, slot.bytes, slot.block_size);
		const rbtrend_transient_chunk *chunk = (const rbtrend_transient_chunk *)span._chunk;
		rbtrend_push(copies, rbtrend_upload_copy{chunk->buffer, span._offset, slot.arena->buffer->parts[0].store.buffer, slot.offset, slot.block_size, &slot.dirty,
		                                       CRGPU_RESOURCE_STATE_VERTEX_BUFFER});
	}
	return true;
}

extern "C" {

RBT_RESULT rbtrend_material_create(
    rbtrend_instance instance,
    const RBTREND_MATERIAL_DESC *desc,
    rbtrend_material *out_material,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (out_material)
		*out_material = nullptr;
	if (!instance)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "instance is NULL");
	if (RBT_STALE(instance, "rbtrend_material_create", "instance"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "instance is destroyed");
	RBTREND_ENTER(instance, "rbtrend_material_create");
	if (!desc)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc is NULL");
	if (const RBT_RESULT base = rbtrend_base_check(out_error, "desc", &desc->base, RBT_STRUCT_TYPE_REND_MATERIAL_DESC))
		return base;
	if (!desc->_layout)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "_layout is NULL");
	if (desc->_layout->instance != instance)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "_layout is of another instance");
	if (desc->_external_block_count > 0 && !desc->_external_blocks)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "_external_blocks is NULL with a count of %u", desc->_external_block_count);
	rbtrend_binding_layout layout = desc->_layout;
	// Every binding is set by its name, so each has one, and only one has it.
	for (uint32_t i = 0; i < layout->binding_count; ++i) {
		if (layout->names[i].empty())
			return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "binding %u of the layout has no name: give the layout's bindings _name, or make it from shaders", layout->bindings[i]._binding);
		for (uint32_t j = 0; j < i; ++j) {
			if (layout->names[j] == layout->names[i])
				return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "two bindings of the layout are named \"%s\"", layout->names[i].c_str());
		}
	}
	// Each external block a uniform buffer binding.
	for (uint32_t e = 0; e < desc->_external_block_count; ++e) {
		const char *name = desc->_external_blocks[e];
		if (!name)
			return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "_external_blocks[%u] is NULL", e);
		bool found = false;
		for (uint32_t i = 0; i < layout->binding_count && !found; ++i) {
			const bool named = layout->names[i] == name || (layout->blocks[i].known && strcmp(layout->blocks[i].block_name, name) == 0);
			if (named && layout->bindings[i]._type == CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
				found = true;
		}
		if (!found)
			return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "_external_blocks[%u] \"%s\" is not a uniform buffer binding of the layout", e, name);
	}

	RBTREND_MALLOC__T(result, rbtrend_material_t);
	if (!result)
		return rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory for the material");
	RBTREND_CONSTRUCT__T(result, rbtrend_material_t);
	result->instance    = instance;
	result->layout      = layout;
	result->entry_count = layout->descriptor_count;

	result->entries      = (RBTREND_BINDING_SET_ENTRY *)calloc(result->entry_count, sizeof(RBTREND_BINDING_SET_ENTRY));
	result->resource_serial = (uint64_t *)calloc(result->entry_count, sizeof(uint64_t));
	result->sampler_serial  = (uint64_t *)calloc(result->entry_count, sizeof(uint64_t));
	if (!result->entries || !result->resource_serial || !result->sampler_serial) {
		RBTREND_DESTRUCT__T(result, rbtrend_material_t);
		return rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory for the material's entries");
	}

	uint32_t next_entry = 0;
	for (uint32_t i = 0; i < layout->binding_count; ++i) {
		rbtrend_material_slot &slot = result->slots[i];
		// slot_count goes up first, so the destructor frees what this slot
		// has taken if a later step fails.
		++result->slot_count;
		slot.binding     = layout->bindings[i];
		slot.name        = layout->names[i].c_str();
		slot.block_name  = layout->blocks[i].known ? layout->blocks[i].block_name : nullptr;
		slot.first_entry = next_entry;
		next_entry += slot.binding._count;

		for (uint32_t element = 0; element < slot.binding._count; ++element) {
			RBTREND_BINDING_SET_ENTRY &entry = result->entries[slot.first_entry + element];
			entry._binding                  = slot.binding._binding;
			entry._array_element            = element;
			entry._type                     = slot.binding._type;
		}

		// Owned: a uniform block the layout knows, one buffer, not external.
		RBTREND_UNIFORM_LAYOUT uniforms{};
		if (slot.binding._type != CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
			continue;
		if (slot.binding._count != 1)
			continue;
		if (!(rbtrend_binding_layout_get_uniform_layout(layout, slot.binding._binding, &uniforms) == RBT_RESULT_OK))
			continue;
		bool external = false;
		for (uint32_t e = 0; e < desc->_external_block_count; ++e)
			if (rbtrend_material_slot_named(slot, desc->_external_blocks[e]))
				external = true;
		if (external)
			continue;

		// Its bytes are copied into its range at submit, from the frame's
		// transient memory.
		if (instance->desc.transient_chunk_size == 0) {
			RBTREND_DESTRUCT__T(result, rbtrend_material_t);
			return rbtrend_fail(out_error, RBT_RESULT_UNSUPPORTED, "\"%s\" is a block the material owns, and the instance has no transient memory to copy it through",
			                   layout->names[i].c_str());
		}
		// A range of the arena for the material's life, the block's size
		// rounded up to 16 as std140 rounds a block.
		slot.members      = layout->members.data() + layout->blocks[i].first_member;
		slot.member_count = layout->blocks[i].member_count;
		if (uniforms.size > UINT32_MAX - 15) {
			RBTREND_DESTRUCT__T(result, rbtrend_material_t);
			return rbtrend_fail(out_error, RBT_RESULT_LIMIT, "the block of \"%s\" is larger than 32 bits can address", layout->names[i].c_str());
		}
		const uint32_t block_size = uniforms.size ? (uniforms.size + 15) & ~15u : 16u;
		if (const RBT_RESULT owned = rbtrend_material_own_block(result, slot, block_size, nullptr, out_error)) {
			RBTREND_DESTRUCT__T(result, rbtrend_material_t);
			return owned;
		}
	}
	if (!out_material) {
		RBTREND_DESTRUCT__T(result, rbtrend_material_t);
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "out_material is NULL");
	}
	// Its destroy's retire, promised now so the destroy can't fail.
	if (rbtrend_instance_reserve_release(instance, 1) != RBT_RESULT_OK) {
		RBTREND_DESTRUCT__T(result, rbtrend_material_t);
		return rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory to release the material later");
	}
	*out_material = result;
	return RBT_RESULT_OK;
}

RBT_RESULT rbtrend_material_copy(
    rbtrend_material source,
    rbtrend_material *out_material,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (out_material)
		*out_material = nullptr;
	if (!source)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "source is NULL");
	if (RBT_STALE(source, "rbtrend_material_copy", "source"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "source is destroyed");
	RBTREND_ENTER(source->instance, "rbtrend_material_copy");

	RBTREND_MALLOC__T(result, rbtrend_material_t);
	if (!result)
		return rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory for the material");
	RBTREND_CONSTRUCT__T(result, rbtrend_material_t);
	result->instance    = source->instance;
	result->layout      = source->layout;
	result->entry_count = source->entry_count;
	// Not the source's binding set, nor its lit scene: a set of its own is
	// built at the first compile that binds the copy, and a lit scene's
	// material is the scene's alone.

	result->entries      = (RBTREND_BINDING_SET_ENTRY *)calloc(result->entry_count, sizeof(RBTREND_BINDING_SET_ENTRY));
	result->resource_serial = (uint64_t *)calloc(result->entry_count, sizeof(uint64_t));
	result->sampler_serial  = (uint64_t *)calloc(result->entry_count, sizeof(uint64_t));
	if (!result->entries || !result->resource_serial || !result->sampler_serial) {
		RBTREND_DESTRUCT__T(result, rbtrend_material_t);
		return rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory for the material's entries");
	}
	// Every resource the source holds, the same handles: they are the
	// caller's, shared by both.
	memcpy(result->entries, source->entries, sizeof(RBTREND_BINDING_SET_ENTRY) * result->entry_count);
	memcpy(result->resource_serial, source->resource_serial, sizeof(uint64_t) * result->entry_count);
	memcpy(result->sampler_serial, source->sampler_serial, sizeof(uint64_t) * result->entry_count);

	for (uint32_t i = 0; i < source->slot_count; ++i) {
		const rbtrend_material_slot &from = source->slots[i];
		rbtrend_material_slot &slot       = result->slots[i];
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
		if (const RBT_RESULT owned = rbtrend_material_own_block(result, slot, from.block_size, from.bytes, out_error)) {
			RBTREND_DESTRUCT__T(result, rbtrend_material_t);
			return owned;
		}
	}
	if (!out_material) {
		RBTREND_DESTRUCT__T(result, rbtrend_material_t);
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "out_material is NULL");
	}
	// Its destroy's retire, promised now so the destroy can't fail.
	if (rbtrend_instance_reserve_release(result->instance, 1) != RBT_RESULT_OK) {
		RBTREND_DESTRUCT__T(result, rbtrend_material_t);
		return rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory to release the material later");
	}
	*out_material = result;
	return RBT_RESULT_OK;
}

void rbtrend_material_destroy(rbtrend_material material) {
	if (!material)
		return;
	if (RBT_STALE(material, "rbtrend_material_destroy", "material"))
		return;
	// A list compiled against it and not submitted would write its blocks.
	rbtrend_instance_forget(material->instance, material);
	// The whole material goes through the release queue: its ranges of the
	// arena are given back once the GPU has finished with everything
	// submitted up to now, which may read them.
	RBT_KILL(material);
	rbtrend_instance_retire(material->instance, material, &rbtrend_release_handle<rbtrend_material_t>);
}

rbtrend_binding_layout rbtrend_material_get_layout(rbtrend_material material) {
	if (!material)
		return nullptr;
	if (RBT_STALE(material, "rbtrend_material_get_layout", "material"))
		return nullptr;
	return material->layout;
}

bool rbtrend_material_is_complete(rbtrend_material material) {
	if (!material)
		return false;
	if (RBT_STALE(material, "rbtrend_material_is_complete", "material"))
		return false;
	const rbtrend_material_slot *slot = nullptr;
	uint32_t element                 = 0;
	bool sampler                     = false;
	return !rbtrend_material_find_empty(material, &slot, &element, &sampler);
}

// Whether binding [index] of [layout] is a block a material of it can own:
// a uniform buffer, one buffer, whose block the layout knows.
static bool rbtrend_layout_has_ownable_block(rbtrend_binding_layout layout, uint32_t index) {
	const RBTREND_BINDING_DESC &binding = layout->bindings[index];
	return binding._type == CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER && binding._count == 1 && layout->blocks[index].known;
}

// The member of [uniform] through [out_member], and its block's slot through
// [out_slot]. RBT_RESULT_INVALID_ARGUMENT, [error] saying why, if it names
// none of [material]'s layout or one in a block the material doesn't own.
static RBT_RESULT rbtrend_material_uniform_member(
    rbtrend_material material,
    const RBTREND_MATERIAL_UNIFORM &uniform,
    const RBTREND_SHADER_MEMBER **out_member,
    rbtrend_material_slot **out_slot,
    RBTREND_ERROR *error) {
	if (!uniform._layout)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "uniform is no uniform (rbtrend_material_find_uniform fills one)");
	if (uniform._layout != material->layout)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "uniform was found in another layout than the material's");
	// Found in this layout, so in range; checked all the same, since the
	// fields are the caller's to have copied.
	if (uniform._binding_index >= material->slot_count)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "uniform names binding %u, and the layout has %u", uniform._binding_index, material->slot_count);
	rbtrend_material_slot &slot = material->slots[uniform._binding_index];
	if (!slot.arena)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "binding \"%s\" is a block the material doesn't own (it was named in _external_blocks)", slot.name);
	if (uniform._member_index >= slot.member_count)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "uniform names member %u of \"%s\", which has %u", uniform._member_index, slot.name, slot.member_count);
	const RBTREND_SHADER_MEMBER &member = slot.members[uniform._member_index];
	if (member.type == RBTREND_UNIFORM_TYPE_UNKNOWN)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "member \"%s\" has a type with no RBTREND_UNIFORM_TYPE: write it with rbtrend_material_set_uniform_bytes", member.name);
	if (member.array_count == 0)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "member \"%s\" is an array without a size", member.name);
	*out_slot   = &slot;
	*out_member = &member;
	return RBT_RESULT_OK;
}

RBT_RESULT rbtrend_material_find_uniform(
    rbtrend_binding_layout layout,
    const char *name,
    RBTREND_MATERIAL_UNIFORM *out_uniform,
    RBTREND_SHADER_MEMBER *out_member,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (!layout)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "layout is NULL");
	if (RBT_STALE(layout, "rbtrend_material_find_uniform", "layout"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "layout is destroyed");
	if (!name)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "name is NULL");
	if (!out_uniform)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "out_uniform is NULL");
	// Qualified by the binding's name or its block's: one block, so one
	// member at most.
	for (uint32_t i = 0; i < layout->binding_count; ++i) {
		if (!rbtrend_layout_has_ownable_block(layout, i))
			continue;
		const rbtrend_binding_layout_t::block_info &block = layout->blocks[i];
		const char *qualifiers[2]                        = {layout->names[i].c_str(), block.block_name};
		for (const char *qualifier : qualifiers) {
			if (!qualifier || !qualifier[0])
				continue;
			const size_t length = strlen(qualifier);
			if (strncmp(name, qualifier, length) != 0 || name[length] != '.')
				continue;
			for (uint32_t m = 0; m < block.member_count; ++m) {
				const RBTREND_SHADER_MEMBER &member = layout->members[block.first_member + m];
				if (strcmp(member.name, name + length + 1) != 0)
					continue;
				*out_uniform = {layout, i, m};
				if (out_member)
					*out_member = member;
				return RBT_RESULT_OK;
			}
		}
	}
	// The path alone: in exactly one block.
	bool found                = false;
	RBTREND_MATERIAL_UNIFORM at{};
	for (uint32_t i = 0; i < layout->binding_count; ++i) {
		if (!rbtrend_layout_has_ownable_block(layout, i))
			continue;
		const rbtrend_binding_layout_t::block_info &block = layout->blocks[i];
		for (uint32_t m = 0; m < block.member_count; ++m) {
			if (strcmp(layout->members[block.first_member + m].name, name) != 0)
				continue;
			if (found)
				return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "\"%s\" is a member of more than one block of the layout: name it after its binding, as \"%s.%s\"", name, layout->names[i].c_str(), name);
			found = true;
			at    = {layout, i, m};
		}
	}
	if (!found)
		return rbtrend_fail(out_error, RBT_RESULT_NOT_FOUND, "no uniform block of the layout has a member \"%s\"", name);
	*out_uniform = at;
	if (out_member)
		*out_member = layout->members[layout->blocks[at._binding_index].first_member + at._member_index];
	return RBT_RESULT_OK;
}

RBT_RESULT rbtrend_material_set_uniform_value(
    rbtrend_material material,
    RBTREND_MATERIAL_UNIFORM uniform,
    const void *data,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (!material)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "material is NULL");
	if (RBT_STALE(material, "rbtrend_material_set_uniform_value", "material"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "material is destroyed");
	RBTREND_ENTER(material->instance, "rbtrend_material_set_uniform_value");
	if (!data)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "data is NULL");
	rbtrend_material_slot *slot         = nullptr;
	const RBTREND_SHADER_MEMBER *member = nullptr;
	if (const RBT_RESULT found = rbtrend_material_uniform_member(material, uniform, &member, &slot, out_error))
		return found;
	// The block was sized from these members, so each one fits.
	rbtrend_write_block_member(*member, slot->bytes, data);
	slot->dirty = true;
	return RBT_RESULT_OK;
}

RBT_RESULT rbtrend_material_get_uniform_value(
    rbtrend_material material,
    RBTREND_MATERIAL_UNIFORM uniform,
    void *out_data,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (!material)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "material is NULL");
	if (RBT_STALE(material, "rbtrend_material_get_uniform_value", "material"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "material is destroyed");
	if (!out_data)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "out_data is NULL");
	rbtrend_material_slot *slot         = nullptr;
	const RBTREND_SHADER_MEMBER *member = nullptr;
	if (const RBT_RESULT found = rbtrend_material_uniform_member(material, uniform, &member, &slot, out_error))
		return found;
	rbtrend_read_block_member(*member, slot->bytes, out_data);
	return RBT_RESULT_OK;
}

RBT_RESULT rbtrend_material_set_uniform(
    rbtrend_material material,
    const char *name,
    const void *data,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (!material)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "material is NULL");
	if (RBT_STALE(material, "rbtrend_material_set_uniform", "material"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "material is destroyed");
	RBTREND_ENTER(material->instance, "rbtrend_material_set_uniform");
	if (!name)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "name is NULL");
	if (!data)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "data is NULL");
	RBTREND_MATERIAL_UNIFORM uniform{};
	if (const RBT_RESULT found = rbtrend_material_find_uniform(material->layout, name, &uniform, nullptr, out_error))
		return found;
	return rbtrend_material_set_uniform_value(material, uniform, data, out_error);
}

RBT_RESULT rbtrend_material_set_uniform_bytes(
    rbtrend_material material,
    const char *name,
    uint32_t offset,
    uint32_t size,
    const void *data,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (!material)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "material is NULL");
	if (RBT_STALE(material, "rbtrend_material_set_uniform_bytes", "material"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "material is destroyed");
	RBTREND_ENTER(material->instance, "rbtrend_material_set_uniform_bytes");
	if (!name)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "name is NULL");
	if (size == 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "size is 0");
	if (!data)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "data is NULL");
	rbtrend_material_slot *slot = nullptr;
	if (const RBT_RESULT found = rbtrend_material_find_slot(material, name, &slot, out_error))
		return found;
	if (!slot->arena)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "\"%s\" is not a block the material owns", name);
	if ((uint64_t)offset + size > slot->block_size)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "%u bytes at offset %u run past the %u bytes of \"%s\"", size, offset, slot->block_size, name);
	memcpy(slot->bytes + offset, data, size);
	slot->dirty = true;
	return RBT_RESULT_OK;
}

RBT_RESULT rbtrend_material_set_uniform_buffer(
    rbtrend_material material,
    const char *name,
    uint32_t array_element,
    rbtrend_buffer buffer,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (!material)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "material is NULL");
	if (RBT_STALE(material, "rbtrend_material_set_uniform_buffer", "material"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "material is destroyed");
	RBTREND_ENTER(material->instance, "rbtrend_material_set_uniform_buffer");
	if (!name)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "name is NULL");
	rbtrend_material_slot *slot = nullptr;
	if (const RBT_RESULT found = rbtrend_material_find_slot(material, name, &slot, out_error))
		return found;
	if (slot->binding._type != CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "binding \"%s\" is not a uniform buffer binding", name);
	if (slot->arena)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "binding \"%s\" is a block the material owns", name);
	RBTREND_BINDING_SET_ENTRY *entry = nullptr;
	if (const RBT_RESULT found = rbtrend_material_slot_entry(material, slot, array_element, &entry, out_error))
		return found;
	if (!buffer)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "buffer is NULL");
	if (RBT_STALE(buffer, "rbtrend_material_set_uniform_buffer", "buffer"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "buffer is destroyed");
	if (buffer->instance != material->instance)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "buffer is of another instance");
	if (!(buffer->desc.usage & RBTREND_BUFFER_USAGE_UNIFORM))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "buffer was created without RBTREND_BUFFER_USAGE_UNIFORM");
	uint64_t &serial = material->resource_serial[entry - material->entries];
	if (entry->_resource._uniform._buffer == buffer && serial == buffer->serial)
		return RBT_RESULT_OK; // held already: nothing to build again
	entry->_resource._uniform._buffer = buffer;
	entry->_resource._uniform._offset = 0;
	entry->_resource._uniform._range  = 0;
	serial                            = buffer->serial;
	material->resources_changed       = true;
	return RBT_RESULT_OK;
}

RBT_RESULT rbtrend_material_set_storage_buffer(
    rbtrend_material material,
    const char *name,
    uint32_t array_element,
    rbtrend_buffer buffer,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (!material)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "material is NULL");
	if (RBT_STALE(material, "rbtrend_material_set_storage_buffer", "material"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "material is destroyed");
	RBTREND_ENTER(material->instance, "rbtrend_material_set_storage_buffer");
	if (!name)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "name is NULL");
	rbtrend_material_slot *slot = nullptr;
	if (const RBT_RESULT found = rbtrend_material_find_slot(material, name, &slot, out_error))
		return found;
	if (slot->binding._type != CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "binding \"%s\" is not a storage buffer binding", name);
	RBTREND_BINDING_SET_ENTRY *entry = nullptr;
	if (const RBT_RESULT found = rbtrend_material_slot_entry(material, slot, array_element, &entry, out_error))
		return found;
	if (!buffer)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "buffer is NULL");
	if (RBT_STALE(buffer, "rbtrend_material_set_storage_buffer", "buffer"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "buffer is destroyed");
	if (buffer->instance != material->instance)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "buffer is of another instance");
	if (!(buffer->desc.usage & RBTREND_BUFFER_USAGE_STORAGE))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "buffer was created without RBTREND_BUFFER_USAGE_STORAGE");
	uint64_t &serial = material->resource_serial[entry - material->entries];
	if (entry->_resource._storage._buffer == buffer && serial == buffer->serial)
		return RBT_RESULT_OK; // held already: nothing to build again
	entry->_resource._storage._buffer = buffer;
	entry->_resource._storage._offset = 0;
	entry->_resource._storage._range  = 0;
	serial                            = buffer->serial;
	material->resources_changed       = true;
	return RBT_RESULT_OK;
}

RBT_RESULT rbtrend_material_set_texture(
    rbtrend_material material,
    const char *name,
    uint32_t array_element,
    rbtrend_texture_view view,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (!material)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "material is NULL");
	if (RBT_STALE(material, "rbtrend_material_set_texture", "material"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "material is destroyed");
	RBTREND_ENTER(material->instance, "rbtrend_material_set_texture");
	if (!name)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "name is NULL");
	if (!view)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "view is NULL");
	if (RBT_STALE(view, "rbtrend_material_set_texture", "view"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "view is destroyed");
	rbtrend_material_slot *slot = nullptr;
	if (const RBT_RESULT found = rbtrend_material_find_slot(material, name, &slot, out_error))
		return found;
	if (!rbtrend_material_type_reads_view(slot->binding._type))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "binding \"%s\" is not a sampled image, storage image or combined image sampler binding", name);
	RBTREND_BINDING_SET_ENTRY *entry = nullptr;
	if (const RBT_RESULT found = rbtrend_material_slot_entry(material, slot, array_element, &entry, out_error))
		return found;
	if (view->instance != material->instance)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "view is of another instance");
	const RBTREND_TEXTURE_VIEW_TYPE wanted = slot->binding._type == CRGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE ? RBTREND_TEXTURE_VIEW_TYPE_UNORDERED_ACCESS
	                                                                                                      : RBTREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE;
	if (view->view_type != wanted)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "binding \"%s\" takes a view of type %u, and view is of type %u", name, (unsigned)wanted, (unsigned)view->view_type);
	uint64_t &serial = material->resource_serial[entry - material->entries];
	if (entry->_resource._image._view == view && serial == view->serial)
		return RBT_RESULT_OK; // held already: nothing to build again
	entry->_resource._image._view = view;
	serial                        = view->serial;
	material->resources_changed   = true;
	return RBT_RESULT_OK;
}

RBT_RESULT rbtrend_material_set_sampler(
    rbtrend_material material,
    const char *name,
    uint32_t array_element,
    rbtrend_sampler sampler,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (!material)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "material is NULL");
	if (RBT_STALE(material, "rbtrend_material_set_sampler", "material"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "material is destroyed");
	RBTREND_ENTER(material->instance, "rbtrend_material_set_sampler");
	if (!name)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "name is NULL");
	if (!sampler)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "sampler is NULL");
	if (RBT_STALE(sampler, "rbtrend_material_set_sampler", "sampler"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "sampler is destroyed");
	rbtrend_material_slot *slot = nullptr;
	if (const RBT_RESULT found = rbtrend_material_find_slot(material, name, &slot, out_error))
		return found;
	if (!rbtrend_material_type_reads_sampler(slot->binding._type))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "binding \"%s\" is not a sampler or combined image sampler binding", name);
	RBTREND_BINDING_SET_ENTRY *entry = nullptr;
	if (const RBT_RESULT found = rbtrend_material_slot_entry(material, slot, array_element, &entry, out_error))
		return found;
	if (sampler->instance != material->instance)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "sampler is of another instance");
	uint64_t &serial = material->sampler_serial[entry - material->entries];
	if (entry->_resource._image._sampler == sampler && serial == sampler->serial)
		return RBT_RESULT_OK; // held already: nothing to build again
	entry->_resource._image._sampler = sampler;
	serial                           = sampler->serial;
	material->resources_changed      = true;
	return RBT_RESULT_OK;
}

} // extern "C"
