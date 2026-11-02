// The reflection table a shader package carries (aprendreflect_table.hpp),
// written from a reflected shader's store.

#include "aprend_reflect_tool.hpp"

#include <cstring>

namespace {

// The table's strings, each written once and found by its offset.
struct table_strings {
	std::vector<char> bytes;

	uint32_t add(const char *text) {
		if (!text)
			return APREND_REFLECT_TABLE_NO_STRING;
		const uint32_t offset = (uint32_t)bytes.size();
		bytes.insert(bytes.end(), text, text + strlen(text) + 1);
		return offset;
	}
};

void put(std::vector<uint8_t> *out, size_t at, const void *record, size_t size) {
	memcpy(out->data() + at, record, size);
}

} // namespace

bool aprend_reflect_table_write(const aprend_reflection_store &store, std::vector<uint8_t> *out, std::string *error) {
	const APREND_SHADER_REFLECTION &view = store.view;
	table_strings strings;

	aprend_reflect_table_header header;
	memset(&header, 0, sizeof(header));
	header.magic                      = APREND_REFLECT_TABLE_MAGIC;
	header.version                    = APREND_REFLECT_TABLE_VERSION;
	header.stage                      = (uint32_t)view.stage;
	header.entry_point                = strings.add(view.entry_point);
	header.flags                      = view.has_push_constants ? APREND_REFLECT_TABLE_HAS_PUSH_CONSTANTS : 0u;
	header.push_constant_name         = strings.add(view.push_constant_name);
	header.push_constant_begin        = view.push_constant_begin;
	header.push_constant_end          = view.push_constant_end;
	header.push_constant_first_member = view.push_constant_first_member;
	header.push_constant_member_count = view.push_constant_member_count;
	header.color_output_count         = view.color_output_count;
	for (int axis = 0; axis < 3; ++axis)
		header.workgroup_size[axis] = view.workgroup_size[axis];

	std::vector<aprend_reflect_table_resource> resources(view.resource_count);
	for (uint32_t i = 0; i < view.resource_count; ++i) {
		const APREND_SHADER_RESOURCE &r  = view.resources[i];
		aprend_reflect_table_resource &t = resources[i];
		t.name                           = strings.add(r.name);
		t.block_name                     = strings.add(r.block_name);
		t.set                            = r.set;
		t.binding                        = r.binding;
		t.type                           = (uint32_t)r.type;
		t.count                          = r.count;
		t.block_size                     = r.block_size;
		t.runtime_array_stride           = r.runtime_array_stride;
		t.first_member                   = r.first_member;
		t.member_count                   = r.member_count;
		t.image_shape                    = (uint32_t)r.image_shape;
		t.image_scalar_type              = (uint32_t)r.image_scalar_type;
		t.flags = (r.image_depth_compare ? APREND_REFLECT_TABLE_DEPTH_COMPARE : 0u) | (r.read_only ? APREND_REFLECT_TABLE_READ_ONLY : 0u) |
		          (r.used ? APREND_REFLECT_TABLE_USED : 0u);
	}
	std::vector<aprend_reflect_table_member> members(view.member_count);
	for (uint32_t i = 0; i < view.member_count; ++i) {
		const APREND_SHADER_MEMBER &m  = view.members[i];
		aprend_reflect_table_member &t = members[i];
		t.name                         = strings.add(m.name);
		t.type                         = (uint32_t)m.type;
		t.offset                       = m.offset;
		t.size                         = m.size;
		t.array_count                  = m.array_count;
		t.array_stride                 = m.array_stride;
		t.matrix_stride                = m.matrix_stride;
		t.flags                        = (m.row_major ? APREND_REFLECT_TABLE_ROW_MAJOR : 0u) | (m.used ? APREND_REFLECT_TABLE_USED : 0u);
	}
	std::vector<aprend_reflect_table_input> inputs(view.input_count);
	for (uint32_t i = 0; i < view.input_count; ++i) {
		const APREND_SHADER_INPUT &in = view.inputs[i];
		aprend_reflect_table_input &t = inputs[i];
		t.name                        = strings.add(in.name);
		t.location                    = in.location;
		t.location_count              = in.location_count;
		t.scalar_type                 = (uint32_t)in.scalar_type;
		t.components                  = in.components;
		t.flags                       = in.used ? APREND_REFLECT_TABLE_USED : 0u;
	}
	std::vector<aprend_reflect_table_storage_format> formats(store.storage_formats.size());
	for (size_t i = 0; i < formats.size(); ++i) {
		formats[i].set     = store.storage_formats[i].set;
		formats[i].binding = store.storage_formats[i].binding;
		formats[i].format  = (uint32_t)store.storage_formats[i].format;
	}
	strings.bytes.resize((strings.bytes.size() + 3) & ~(size_t)3, 0);

	// Laid out in the order the header names them, each part 4-byte aligned.
	uint64_t at          = sizeof(header);
	const uint64_t parts[6] = {(uint64_t)resources.size() * sizeof(aprend_reflect_table_resource), (uint64_t)members.size() * sizeof(aprend_reflect_table_member),
	                           (uint64_t)inputs.size() * sizeof(aprend_reflect_table_input),
	                           (uint64_t)formats.size() * sizeof(aprend_reflect_table_storage_format),
	                           (uint64_t)store.output_types.size() * sizeof(uint32_t), (uint64_t)strings.bytes.size()};
	uint64_t offsets[6];
	for (int i = 0; i < 6; ++i) {
		offsets[i] = at;
		at += parts[i];
	}
	if (at > UINT32_MAX || store.output_types.size() > UINT32_MAX || formats.size() > UINT32_MAX) {
		*error = "the reflection doesn't fit a table's 32-bit sizes";
		return false;
	}
	header.size                  = (uint32_t)at;
	header.resource_offset       = (uint32_t)offsets[0];
	header.resource_count        = (uint32_t)resources.size();
	header.member_offset         = (uint32_t)offsets[1];
	header.member_count          = (uint32_t)members.size();
	header.input_offset          = (uint32_t)offsets[2];
	header.input_count           = (uint32_t)inputs.size();
	header.storage_format_offset = (uint32_t)offsets[3];
	header.storage_format_count  = (uint32_t)formats.size();
	header.output_type_offset    = (uint32_t)offsets[4];
	header.output_type_count     = (uint32_t)store.output_types.size();
	header.string_offset         = (uint32_t)offsets[5];
	header.string_size           = (uint32_t)strings.bytes.size();

	out->assign((size_t)at, 0);
	put(out, 0, &header, sizeof(header));
	if (!resources.empty())
		put(out, (size_t)offsets[0], resources.data(), (size_t)parts[0]);
	if (!members.empty())
		put(out, (size_t)offsets[1], members.data(), (size_t)parts[1]);
	if (!inputs.empty())
		put(out, (size_t)offsets[2], inputs.data(), (size_t)parts[2]);
	if (!formats.empty())
		put(out, (size_t)offsets[3], formats.data(), (size_t)parts[3]);
	for (size_t i = 0; i < store.output_types.size(); ++i) {
		const uint32_t type = (uint32_t)store.output_types[i];
		put(out, (size_t)offsets[4] + i * sizeof(uint32_t), &type, sizeof(type));
	}
	if (!strings.bytes.empty())
		put(out, (size_t)offsets[5], strings.bytes.data(), (size_t)parts[5]);
	return true;
}
