#ifndef RBTREND_REFLECT_TABLE_HPP
#define RBTREND_REFLECT_TABLE_HPP

/* Internal header: the reflection table a shader package carries
 * (RBTREND_SHADER_PACKAGE::reflection, render/rbtrendshader.h), written at
 * build time by tools/rbtrend_reflect from the package's SPIR-V and read by
 * Rend when a shader is made (rbtrendreflect_internal.hpp,
 * rbtrend_reflect_table_read). Included by the library and by the tool, of
 * the same Rabbittus version: the version below says which layout a
 * table has, and a reader refuses another (RBT_RESULT_REND_REFLECTION_MISMATCH).
 *
 * Every field is a 32-bit unsigned integer, little-endian, so the table is
 * 4-byte aligned plain data with no pointer in it. A string is an offset
 * into the string area (RBTREND_REFLECT_TABLE_NO_STRING for none), each
 * string ending in a 0 inside the area. A list is an offset from the
 * table's start and a count of fixed-size records. The table is laid out
 * header, resources, members, inputs, storage formats, output types,
 * strings, though a reader checks each part's bounds rather than assume it. */

#include <stdint.h>

/* 'A' 'R' 'F' 'T' in its first four bytes. */
#define RBTREND_REFLECT_TABLE_MAGIC 0x54465241u
/* The layout below. Changed whenever a field is, by both sides at once. */
#define RBTREND_REFLECT_TABLE_VERSION 1u
#define RBTREND_REFLECT_TABLE_NO_STRING 0xffffffffu

/* RBTREND_SHADER_REFLECTION's flags, and its records'. */
#define RBTREND_REFLECT_TABLE_HAS_PUSH_CONSTANTS 0x1u
#define RBTREND_REFLECT_TABLE_DEPTH_COMPARE 0x1u
#define RBTREND_REFLECT_TABLE_READ_ONLY 0x2u
#define RBTREND_REFLECT_TABLE_USED 0x4u
#define RBTREND_REFLECT_TABLE_ROW_MAJOR 0x8u

typedef struct rbtrend_reflect_table_header {
	uint32_t magic;
	uint32_t version;
	/* The table's bytes, the header included. */
	uint32_t size;
	uint32_t stage;
	uint32_t entry_point;
	uint32_t flags;
	uint32_t push_constant_name;
	uint32_t push_constant_begin;
	uint32_t push_constant_end;
	uint32_t push_constant_first_member;
	uint32_t push_constant_member_count;
	uint32_t color_output_count;
	uint32_t workgroup_size[3];
	uint32_t resource_offset;
	uint32_t resource_count;
	uint32_t member_offset;
	uint32_t member_count;
	uint32_t input_offset;
	uint32_t input_count;
	uint32_t storage_format_offset;
	uint32_t storage_format_count;
	uint32_t output_type_offset;
	uint32_t output_type_count;
	uint32_t string_offset;
	uint32_t string_size;
} rbtrend_reflect_table_header;
static_assert(sizeof(rbtrend_reflect_table_header) == 27 * 4, "the table's header is 27 words");

/* RBTREND_SHADER_RESOURCE. */
typedef struct rbtrend_reflect_table_resource {
	uint32_t name;
	uint32_t block_name;
	uint32_t set;
	uint32_t binding;
	uint32_t type;
	uint32_t count;
	uint32_t block_size;
	uint32_t runtime_array_stride;
	uint32_t first_member;
	uint32_t member_count;
	uint32_t image_shape;
	uint32_t image_scalar_type;
	/* DEPTH_COMPARE, READ_ONLY, USED. */
	uint32_t flags;
} rbtrend_reflect_table_resource;
static_assert(sizeof(rbtrend_reflect_table_resource) == 13 * 4, "a resource is 13 words");

/* RBTREND_SHADER_MEMBER. */
typedef struct rbtrend_reflect_table_member {
	uint32_t name;
	uint32_t type;
	uint32_t offset;
	uint32_t size;
	uint32_t array_count;
	uint32_t array_stride;
	uint32_t matrix_stride;
	/* ROW_MAJOR, USED. */
	uint32_t flags;
} rbtrend_reflect_table_member;
static_assert(sizeof(rbtrend_reflect_table_member) == 8 * 4, "a member is 8 words");

/* RBTREND_SHADER_INPUT. */
typedef struct rbtrend_reflect_table_input {
	uint32_t name;
	uint32_t location;
	uint32_t location_count;
	uint32_t scalar_type;
	uint32_t components;
	/* USED. */
	uint32_t flags;
} rbtrend_reflect_table_input;
static_assert(sizeof(rbtrend_reflect_table_input) == 6 * 4, "an input is 6 words");

/* rbtrend_reflection_store::storage_format. */
typedef struct rbtrend_reflect_table_storage_format {
	uint32_t set;
	uint32_t binding;
	uint32_t format;
} rbtrend_reflect_table_storage_format;
static_assert(sizeof(rbtrend_reflect_table_storage_format) == 3 * 4, "a storage format is 3 words");

/* The output types are one word each, an RBTREND_SHADER_SCALAR_TYPE. */

#endif // RBTREND_REFLECT_TABLE_HPP
