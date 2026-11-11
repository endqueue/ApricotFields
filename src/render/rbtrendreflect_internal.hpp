#ifndef RBTREND_REFLECT_INTERNAL_HPP
#define RBTREND_REFLECT_INTERNAL_HPP

/* Internal header — never included outside rabbittus/src/render/ (and
 * tests/rbtrendreflect_tests.cpp and tests/fuzz/rbtrend_reflection_fuzz.cpp,
 * which test it without a device, and tools/rbtrend_reflect, which fills the
 * store at build time).
 *
 * The reflection of one shader (render/rbtrendshader.h), read from its
 * package's table, and what the rest of Rend does with it: building a
 * binding layout, checking a pipeline, writing a block member. Nothing here
 * calls into Carrottus GPU, so it runs without a device. */

#include "rbtcore.h"
#include "render/rbtrendshader.h"

#include <crgpu.h>

#include <deque>
#include <string>
#include <vector>

/* Owns everything RBTREND_SHADER_REFLECTION points at. [view] is filled by
 * rbtrend_reflect_table_read (at build time, rbtrend_reflect_spirv in
 * tools/rbtrend_reflect) and points into the vectors and strings below, so a
 * store is never copied or moved once filled: it lives inside the handle
 * that owns it. */
struct rbtrend_reflection_store {
	RBTREND_SHADER_REFLECTION view{};
	std::vector<RBTREND_SHADER_RESOURCE> resources{};
	std::vector<RBTREND_SHADER_MEMBER> members{};
	std::vector<RBTREND_SHADER_INPUT> inputs{};
	/* What the public view has no field for (RBTREND_SHADER_REFLECTION is
	 * frozen), kept for Rend's own checks. */
	/* A storage image the entry point uses whose GLSL declares a format
	 * (`layout(rgba8)`): the format a view bound there has to be, the one
	 * Vulkan's table of compatible formats gives. A declared format Carrottus GPU
	 * has no format for, and an image declared without one, isn't listed. */
	struct storage_format {
		uint32_t set;
		uint32_t binding;
		CRGPU_FORMAT format;
	};
	std::vector<storage_format> storage_formats{};
	/* A fragment shader's color outputs by location, view.color_output_count
	 * of them: the kind of number each writes, NONE for a location in that
	 * span it doesn't write. Empty for any other stage. */
	std::vector<RBTREND_SHADER_SCALAR_TYPE> output_types{};
	/* Every name the view points at. A deque, so adding one never moves the
	 * ones already pointed at. */
	std::deque<std::string> strings{};
};

/* Reads [size] bytes of a package's reflection table (rbtrendreflect_table.hpp),
 * the reflection of a shader of [stage] whose entry point is [entry_point],
 * into [out], which is empty. Everything in the table is checked before
 * anything is read: RBT_RESULT_CORRUPT_DATA if it isn't a table
 * tools/rbtrend_reflect wrote or is another stage's or entry point's,
 * RBT_RESULT_REND_REFLECTION_MISMATCH if it is of another table version,
 * RBT_RESULT_OUT_OF_MEMORY; [error] says why. [table] need not be aligned:
 * it is read by memcpy. [out] is then in no particular state, and is only
 * destroyed. Throws nothing. The SPIR-V's reflection itself is made at
 * build time (tools/rbtrend_reflect), not here. */
RBT_RESULT rbtrend_reflect_table_read(
    const void *table,
    uint64_t size,
    CRGPU_SHADER_STAGE stage,
    const char *entry_point,
    rbtrend_reflection_store *out,
    std::string *error) noexcept;

/* Columns of a matrix type, 1 for anything else. */
uint32_t rbtrend_uniform_type_columns(RBTREND_UNIFORM_TYPE type);
/* Bytes of one column of [type] tightly packed: a vec3's 12, a mat3's 12, a
 * float's 4. 0 for a value that is not an RBTREND_UNIFORM_TYPE. */
uint32_t rbtrend_uniform_type_column_size(RBTREND_UNIFORM_TYPE type);

/* Writes one member of a block into [block] (the block's bytes) from
 * [data], which holds the whole member tightly packed: each element after
 * the one before, each matrix column after column (column-major, as glm and
 * most C code keep it). Each element goes at the member's array stride and
 * each column at its matrix stride; a row-major matrix is transposed on the
 * way. The member is the caller's to have checked: a known type, with an
 * array count that is not 0. */
void rbtrend_write_block_member(
    const RBTREND_SHADER_MEMBER &member,
    uint8_t *block,
    const void *data);

/* rbtrend_write_block_member the other way: the member from [block] into
 * [out_data], tightly packed, as the caller would have given it. The same
 * checks are the caller's. */
void rbtrend_read_block_member(
    const RBTREND_SHADER_MEMBER &member,
    const uint8_t *block,
    void *out_data);

#endif // RBTREND_REFLECT_INTERNAL_HPP
