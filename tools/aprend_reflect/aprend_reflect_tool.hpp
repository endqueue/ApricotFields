#ifndef APREND_REFLECT_TOOL_HPP
#define APREND_REFLECT_TOOL_HPP

/* tools/aprend_reflect's own: reflecting a shader's SPIR-V into the store
 * Aprend reads (aprendreflect_internal.hpp), and writing the store as the
 * table a package carries (aprendreflect_table.hpp). Built into the tool and
 * into ApricotFields' aprendreflect_tests, never into the library. */

#include "aprendreflect_internal.hpp"
#include "aprendreflect_table.hpp"

#include <string>
#include <vector>

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

/* [store]'s view, as a table, into [out]. False, with the reason in
 * [error], if a count or the table's size doesn't fit its 32-bit field.
 * Throws std::bad_alloc. */
bool aprend_reflect_table_write(const aprend_reflection_store &store, std::vector<uint8_t> *out, std::string *error);

#endif // APREND_REFLECT_TOOL_HPP
