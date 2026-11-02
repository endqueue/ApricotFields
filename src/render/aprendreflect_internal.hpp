#ifndef APREND_REFLECT_INTERNAL_HPP
#define APREND_REFLECT_INTERNAL_HPP

/* Internal header — never included outside ApricotFields/src/render/ (and
 * tests/aprendreflect_tests.cpp and tests/fuzz/aprend_reflection_fuzz.cpp,
 * which test it without a device, and tools/aprend_reflect, which fills the
 * store at build time).
 *
 * The reflection of one shader (render/aprendshader.h), read from its
 * package's table, and what the rest of Aprend does with it: building a
 * binding layout, checking a pipeline, writing a block member. Nothing here
 * calls into SpudGPU, so it runs without a device. */

#include "apricore.h"
#include "render/aprendshader.h"

#include <spudgpu.h>

#include <deque>
#include <string>
#include <vector>

/* Owns everything APREND_SHADER_REFLECTION points at. [view] is filled by
 * aprend_reflect_table_read (at build time, aprend_reflect_spirv in
 * tools/aprend_reflect) and points into the vectors and strings below, so a
 * store is never copied or moved once filled: it lives inside the handle
 * that owns it. */
struct aprend_reflection_store {
	APREND_SHADER_REFLECTION view{};
	std::vector<APREND_SHADER_RESOURCE> resources{};
	std::vector<APREND_SHADER_MEMBER> members{};
	std::vector<APREND_SHADER_INPUT> inputs{};
	/* What the public view has no field for (APREND_SHADER_REFLECTION is
	 * frozen), kept for Aprend's own checks. */
	/* A storage image the entry point uses whose GLSL declares a format
	 * (`layout(rgba8)`): the format a view bound there has to be, the one
	 * Vulkan's table of compatible formats gives. A declared format SpudGPU
	 * has no format for, and an image declared without one, isn't listed. */
	struct storage_format {
		uint32_t set;
		uint32_t binding;
		SPUDGPU_FORMAT format;
	};
	std::vector<storage_format> storage_formats{};
	/* A fragment shader's color outputs by location, view.color_output_count
	 * of them: the kind of number each writes, NONE for a location in that
	 * span it doesn't write. Empty for any other stage. */
	std::vector<APREND_SHADER_SCALAR_TYPE> output_types{};
	/* Every name the view points at. A deque, so adding one never moves the
	 * ones already pointed at. */
	std::deque<std::string> strings{};
};

/* Reads [size] bytes of a package's reflection table (aprendreflect_table.hpp),
 * the reflection of a shader of [stage] whose entry point is [entry_point],
 * into [out], which is empty. Everything in the table is checked before
 * anything is read: APRESULT_CORRUPT_DATA if it isn't a table
 * tools/aprend_reflect wrote or is another stage's or entry point's,
 * APRESULT_APREND_REFLECTION_MISMATCH if it is of another table version,
 * APRESULT_OUT_OF_MEMORY; [error] says why. [table] need not be aligned:
 * it is read by memcpy. [out] is then in no particular state, and is only
 * destroyed. Throws nothing. The SPIR-V's reflection itself is made at
 * build time (tools/aprend_reflect), not here. */
APRESULT aprend_reflect_table_read(
    const void *table,
    uint64_t size,
    SPUDGPU_SHADER_STAGE stage,
    const char *entry_point,
    aprend_reflection_store *out,
    std::string *error) noexcept;

/* Columns of a matrix type, 1 for anything else. */
uint32_t aprend_uniform_type_columns(APREND_UNIFORM_TYPE type);
/* Bytes of one column of [type] tightly packed: a vec3's 12, a mat3's 12, a
 * float's 4. 0 for a value that is not an APREND_UNIFORM_TYPE. */
uint32_t aprend_uniform_type_column_size(APREND_UNIFORM_TYPE type);

/* Writes one member of a block into [block] (the block's bytes) from
 * [data], which holds the whole member tightly packed: each element after
 * the one before, each matrix column after column (column-major, as glm and
 * most C code keep it). Each element goes at the member's array stride and
 * each column at its matrix stride; a row-major matrix is transposed on the
 * way. The member is the caller's to have checked: a known type, with an
 * array count that is not 0. */
void aprend_write_block_member(
    const APREND_SHADER_MEMBER &member,
    uint8_t *block,
    const void *data);

/* aprend_write_block_member the other way: the member from [block] into
 * [out_data], tightly packed, as the caller would have given it. The same
 * checks are the caller's. */
void aprend_read_block_member(
    const APREND_SHADER_MEMBER &member,
    const uint8_t *block,
    void *out_data);

#endif // APREND_REFLECT_INTERNAL_HPP
