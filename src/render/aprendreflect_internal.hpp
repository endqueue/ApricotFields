#ifndef APREND_REFLECT_INTERNAL_HPP
#define APREND_REFLECT_INTERNAL_HPP

/* Internal header — never included outside ApricotFields/src/render/ (and
 * tests/aprendreflect_tests.cpp, which tests it without a device).
 *
 * The reflection of one shader's SPIR-V (render/aprendshader.h), and what
 * the rest of Aprend does with it: building a binding layout, checking a
 * pipeline, writing a block member. Nothing here calls into SpudGPU, so it
 * runs without a device. */

#include "render/aprendshader.h"

#include <deque>
#include <string>
#include <vector>

/* Owns everything aprend_shader_reflection points at. [view] is filled by
 * aprend_reflect_spirv and points into the vectors and strings below, so a
 * store is never copied or moved once filled: it lives inside the handle
 * that owns it. */
struct aprend_reflection_store {
	aprend_shader_reflection view{};
	std::vector<aprend_shader_resource> resources{};
	std::vector<aprend_shader_member> members{};
	std::vector<aprend_shader_input> inputs{};
	/* Every name the view points at. A deque, so adding one never moves the
	 * ones already pointed at. */
	std::deque<std::string> strings{};
};

/* Reflects [word_count] words of SPIR-V, the code of a shader of [stage],
 * into [out], which is empty. False, with what was wrong in [error], if the
 * code isn't SPIR-V, has no entry point for [stage], or its entry point uses
 * a resource Aprend has no binding for. [out] is then in no particular
 * state, and is only destroyed. Throws nothing: SPIRV-Cross's exceptions
 * and std::bad_alloc are caught here. */
bool aprend_reflect_spirv(
    const uint32_t *words,
    size_t word_count,
    SPUDGPU_SHADER_STAGE stage,
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
    const aprend_shader_member &member,
    uint8_t *block,
    const void *data);

/* aprend_write_block_member the other way: the member from [block] into
 * [out_data], tightly packed, as the caller would have given it. The same
 * checks are the caller's. */
void aprend_read_block_member(
    const aprend_shader_member &member,
    const uint8_t *block,
    void *out_data);

#endif // APREND_REFLECT_INTERNAL_HPP
