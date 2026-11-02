// A shader package's reflection table (aprendreflect_table.hpp), read into
// the store a shader keeps (aprendreflect_internal.hpp). The table is bytes
// a caller vouches for only by having built them: every count, offset and
// string is checked before anything is read, and nothing is read through a
// pointer into the table but by memcpy. A fuzz target
// (tests/fuzz/aprend_reflection_fuzz.cpp) reads through here.

#include "aprendreflect_internal.hpp"
#include "aprendreflect_table.hpp"

#include <cstdio>
#include <cstring>
#include <new>

namespace {

struct table_reader {
	const uint8_t *bytes;
	uint64_t size;
	aprend_reflect_table_header header;
	std::string *error;

	// [what] into the error, and the table's corruption.
	APRESULT corrupt(const char *what) {
		*error = what;
		return APRESULT_CORRUPT_DATA;
	}

	// [count] records of [record_size] at [offset]: 4-byte aligned and
	// wholly inside the table.
	bool list_fits(uint32_t offset, uint32_t count, uint64_t record_size) const {
		return offset % 4 == 0 && (uint64_t)offset <= size && (uint64_t)count * record_size <= size - offset;
	}

	template <typename T>
	T record(uint32_t offset, uint32_t index) const {
		T value;
		memcpy(&value, bytes + offset + (uint64_t)index * sizeof(T), sizeof(T));
		return value;
	}

	// A string of the string area: offset inside it and a 0 before its end.
	bool string_ok(uint32_t offset, bool may_be_none) const {
		if (offset == APREND_REFLECT_TABLE_NO_STRING)
			return may_be_none;
		if (offset >= header.string_size)
			return false;
		const uint8_t *start = bytes + header.string_offset + offset;
		return memchr(start, 0, header.string_size - offset) != nullptr;
	}

	const char *string_at(uint32_t offset) const {
		return offset == APREND_REFLECT_TABLE_NO_STRING ? nullptr : (const char *)(bytes + header.string_offset + offset);
	}
};

bool scalar_type_ok(uint32_t type) {
	return type <= APREND_SHADER_SCALAR_TYPE_OTHER;
}

// The bytes a member reaches past its offset: its elements at their
// stride, each a column at a time where it is a matrix. 0 for an array
// without a size, which may only end a storage block.
uint64_t member_extent(const aprend_reflect_table_member &m) {
	if (m.array_count == 0 || m.type == APREND_UNIFORM_TYPE_UNKNOWN)
		return m.size;
	const uint64_t columns     = aprend_uniform_type_columns(m.type);
	const uint64_t column_size = aprend_uniform_type_column_size(m.type);
	uint64_t element           = columns * column_size;
	if (columns > 1) {
		const uint64_t rows = column_size / 4;
		element             = (m.flags & APREND_REFLECT_TABLE_ROW_MAJOR) ? (rows - 1) * m.matrix_stride + columns * 4
		                                                                 : (columns - 1) * m.matrix_stride + column_size;
	}
	const uint64_t extent = (uint64_t)(m.array_count - 1) * m.array_stride + element;
	return extent > m.size ? extent : m.size;
}

} // namespace

APRESULT aprend_reflect_table_read(
    const void *table,
    uint64_t size,
    SPUDGPU_SHADER_STAGE stage,
    const char *entry_point,
    aprend_reflection_store *out,
    std::string *error) noexcept {
	try {
		table_reader r{(const uint8_t *)table, size, {}, error};
		if (size < sizeof(aprend_reflect_table_header))
			return r.corrupt("the reflection is shorter than its table's header");
		memcpy(&r.header, table, sizeof(r.header));
		const aprend_reflect_table_header &h = r.header;
		if (h.magic != APREND_REFLECT_TABLE_MAGIC)
			return r.corrupt("the reflection isn't a table tools/aprend_reflect wrote (its magic)");
		if (h.version != APREND_REFLECT_TABLE_VERSION) {
			char message[160];
			snprintf(message, sizeof(message),
			         "the reflection was written by tools/aprend_reflect of table version %u; this library reads version %u: build the shader again",
			         h.version, APREND_REFLECT_TABLE_VERSION);
			*error = message;
			return APRESULT_APREND_REFLECTION_MISMATCH;
		}
		if (h.size != size)
			return r.corrupt("the reflection's size isn't the one its table states");
		if (h.stage != (uint32_t)stage)
			return r.corrupt("the reflection is of another stage than the package's");
		if (h.flags & ~APREND_REFLECT_TABLE_HAS_PUSH_CONSTANTS)
			return r.corrupt("the reflection's flags hold a bit no table has");
		if (!r.list_fits(h.string_offset, h.string_size, 1))
			return r.corrupt("the reflection's strings reach past its end");
		if (!r.list_fits(h.resource_offset, h.resource_count, sizeof(aprend_reflect_table_resource)))
			return r.corrupt("the reflection's resources reach past its end");
		if (!r.list_fits(h.member_offset, h.member_count, sizeof(aprend_reflect_table_member)))
			return r.corrupt("the reflection's members reach past its end");
		if (!r.list_fits(h.input_offset, h.input_count, sizeof(aprend_reflect_table_input)))
			return r.corrupt("the reflection's inputs reach past its end");
		if (!r.list_fits(h.storage_format_offset, h.storage_format_count, sizeof(aprend_reflect_table_storage_format)))
			return r.corrupt("the reflection's storage formats reach past its end");
		if (!r.list_fits(h.output_type_offset, h.output_type_count, sizeof(uint32_t)))
			return r.corrupt("the reflection's output types reach past its end");
		if (!r.string_ok(h.entry_point, true))
			return r.corrupt("the reflection's entry point isn't a string of its table");
		if (h.entry_point != APREND_REFLECT_TABLE_NO_STRING && strcmp(r.string_at(h.entry_point), entry_point) != 0)
			return r.corrupt("the reflection is of another entry point than the package names");
		if (!r.string_ok(h.push_constant_name, true))
			return r.corrupt("the reflection's push constant name isn't a string of its table");
		if ((uint64_t)h.push_constant_first_member + h.push_constant_member_count > h.member_count)
			return r.corrupt("the reflection's push constant members reach past its members");
		if (h.push_constant_begin > h.push_constant_end)
			return r.corrupt("the reflection's push constants begin after they end");
		if (h.output_type_count != h.color_output_count)
			return r.corrupt("the reflection's output types aren't its color outputs");

		for (uint32_t i = 0; i < h.member_count; ++i) {
			const aprend_reflect_table_member m = r.record<aprend_reflect_table_member>(h.member_offset, i);
			if (!r.string_ok(m.name, false))
				return r.corrupt("a member's name isn't a string of the reflection's table");
			if (m.type != APREND_UNIFORM_TYPE_UNKNOWN && aprend_uniform_type_get_size(m.type) == 0)
				return r.corrupt("a member's type is none Aprend has");
			if (m.flags & ~(APREND_REFLECT_TABLE_ROW_MAJOR | APREND_REFLECT_TABLE_USED))
				return r.corrupt("a member's flags hold a bit no table has");
			if ((uint64_t)m.offset + member_extent(m) > UINT32_MAX)
				return r.corrupt("a member reaches past a block's 32-bit size");
		}
		for (uint32_t i = 0; i < h.resource_count; ++i) {
			const aprend_reflect_table_resource res = r.record<aprend_reflect_table_resource>(h.resource_offset, i);
			if (!r.string_ok(res.name, false))
				return r.corrupt("a resource's name isn't a string of the reflection's table");
			if (!r.string_ok(res.block_name, true))
				return r.corrupt("a resource's block name isn't a string of the reflection's table");
			if (res.type > SPUDGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE)
				return r.corrupt("a resource's type is no descriptor type");
			if (res.count == 0)
				return r.corrupt("a resource holds no descriptor");
			if (res.image_shape > APREND_TEXTURE_VIEW_SHAPE_3D)
				return r.corrupt("a resource's image is of no shape Aprend has");
			if (!scalar_type_ok(res.image_scalar_type))
				return r.corrupt("a resource's image reads no number Aprend has");
			if (res.flags & ~(APREND_REFLECT_TABLE_DEPTH_COMPARE | APREND_REFLECT_TABLE_READ_ONLY | APREND_REFLECT_TABLE_USED))
				return r.corrupt("a resource's flags hold a bit no table has");
			if ((uint64_t)res.first_member + res.member_count > h.member_count)
				return r.corrupt("a resource's members reach past the reflection's members");
			if (i > 0) {
				const aprend_reflect_table_resource before = r.record<aprend_reflect_table_resource>(h.resource_offset, i - 1);
				const uint64_t before_key = ((uint64_t)before.set << 32) | before.binding;
				const uint64_t key        = ((uint64_t)res.set << 32) | res.binding;
				if (before_key >= key)
					return r.corrupt("the reflection's resources aren't ordered by set and binding");
			}
			// A block's members lie inside the block, an array without a
			// size ending it excepted, so writing one stays in its bytes.
			for (uint32_t k = 0; k < res.member_count; ++k) {
				const aprend_reflect_table_member m = r.record<aprend_reflect_table_member>(h.member_offset, res.first_member + k);
				if (m.array_count != 0 && (uint64_t)m.offset + member_extent(m) > res.block_size)
					return r.corrupt("a member reaches past its block");
			}
		}
		for (uint32_t i = 0; i < h.input_count; ++i) {
			const aprend_reflect_table_input in = r.record<aprend_reflect_table_input>(h.input_offset, i);
			if (!r.string_ok(in.name, false))
				return r.corrupt("an input's name isn't a string of the reflection's table");
			if (in.location_count == 0)
				return r.corrupt("an input takes no location");
			if ((uint64_t)in.location + in.location_count > UINT32_MAX)
				return r.corrupt("an input's locations reach past 32 bits");
			if (!scalar_type_ok(in.scalar_type))
				return r.corrupt("an input reads no number Aprend has");
			if (in.components == 0)
				return r.corrupt("an input has no component");
			if (in.components > 4)
				return r.corrupt("an input has more than four components");
			if (in.flags & ~APREND_REFLECT_TABLE_USED)
				return r.corrupt("an input's flags hold a bit no table has");
		}
		for (uint32_t i = 0; i < h.output_type_count; ++i)
			if (!scalar_type_ok(r.record<uint32_t>(h.output_type_offset, i)))
				return r.corrupt("an output's type is no number Aprend has");

		// Checked: now read, every string copied into the store.
		APREND_SHADER_REFLECTION &view = out->view;
		auto keep                      = [&](uint32_t offset) -> const char * {
			if (offset == APREND_REFLECT_TABLE_NO_STRING)
				return nullptr;
			out->strings.emplace_back(r.string_at(offset));
			return out->strings.back().c_str();
		};
		view.stage                      = stage;
		view.entry_point                = keep(h.entry_point);
		view.has_push_constants         = (h.flags & APREND_REFLECT_TABLE_HAS_PUSH_CONSTANTS) != 0;
		view.push_constant_name         = keep(h.push_constant_name);
		view.push_constant_begin        = h.push_constant_begin;
		view.push_constant_end          = h.push_constant_end;
		view.push_constant_first_member = h.push_constant_first_member;
		view.push_constant_member_count = h.push_constant_member_count;
		view.color_output_count         = h.color_output_count;
		for (int axis = 0; axis < 3; ++axis)
			view.workgroup_size[axis] = h.workgroup_size[axis];
		out->resources.resize(h.resource_count);
		for (uint32_t i = 0; i < h.resource_count; ++i) {
			const aprend_reflect_table_resource t = r.record<aprend_reflect_table_resource>(h.resource_offset, i);
			APREND_SHADER_RESOURCE &res           = out->resources[i];
			res.name                              = keep(t.name);
			res.block_name                        = keep(t.block_name);
			res.set                               = t.set;
			res.binding                           = t.binding;
			res.type                              = (SPUDGPU_DESCRIPTOR_TYPE)t.type;
			res.count                             = t.count;
			res.block_size                        = t.block_size;
			res.runtime_array_stride              = t.runtime_array_stride;
			res.first_member                      = t.first_member;
			res.member_count                      = t.member_count;
			res.image_shape                       = (APREND_TEXTURE_VIEW_SHAPE)t.image_shape;
			res.image_scalar_type                 = (APREND_SHADER_SCALAR_TYPE)t.image_scalar_type;
			res.image_depth_compare               = (t.flags & APREND_REFLECT_TABLE_DEPTH_COMPARE) != 0;
			res.read_only                         = (t.flags & APREND_REFLECT_TABLE_READ_ONLY) != 0;
			res.used                              = (t.flags & APREND_REFLECT_TABLE_USED) != 0;
		}
		out->members.resize(h.member_count);
		for (uint32_t i = 0; i < h.member_count; ++i) {
			const aprend_reflect_table_member t = r.record<aprend_reflect_table_member>(h.member_offset, i);
			APREND_SHADER_MEMBER &m             = out->members[i];
			m.name                              = keep(t.name);
			m.type                              = (APREND_UNIFORM_TYPE)t.type;
			m.offset                            = t.offset;
			m.size                              = t.size;
			m.array_count                       = t.array_count;
			m.array_stride                      = t.array_stride;
			m.matrix_stride                     = t.matrix_stride;
			m.row_major                         = (t.flags & APREND_REFLECT_TABLE_ROW_MAJOR) != 0;
			m.used                              = (t.flags & APREND_REFLECT_TABLE_USED) != 0;
		}
		out->inputs.resize(h.input_count);
		for (uint32_t i = 0; i < h.input_count; ++i) {
			const aprend_reflect_table_input t = r.record<aprend_reflect_table_input>(h.input_offset, i);
			APREND_SHADER_INPUT &in            = out->inputs[i];
			in.name                            = keep(t.name);
			in.location                        = t.location;
			in.location_count                  = t.location_count;
			in.scalar_type                     = (APREND_SHADER_SCALAR_TYPE)t.scalar_type;
			in.components                      = t.components;
			in.used                            = (t.flags & APREND_REFLECT_TABLE_USED) != 0;
		}
		out->storage_formats.resize(h.storage_format_count);
		for (uint32_t i = 0; i < h.storage_format_count; ++i) {
			const aprend_reflect_table_storage_format t = r.record<aprend_reflect_table_storage_format>(h.storage_format_offset, i);
			out->storage_formats[i]                     = {t.set, t.binding, (SPUDGPU_FORMAT)t.format};
		}
		out->output_types.resize(h.output_type_count);
		for (uint32_t i = 0; i < h.output_type_count; ++i)
			out->output_types[i] = (APREND_SHADER_SCALAR_TYPE)r.record<uint32_t>(h.output_type_offset, i);
		view.resources      = out->resources.data();
		view.resource_count = h.resource_count;
		view.members        = out->members.data();
		view.member_count   = h.member_count;
		view.inputs         = out->inputs.data();
		view.input_count    = h.input_count;
		return APRESULT_OK;
	} catch (const std::bad_alloc &) {
		try {
			*error = "out of memory";
		} catch (...) {
		}
		return APRESULT_OUT_OF_MEMORY;
	}
}
