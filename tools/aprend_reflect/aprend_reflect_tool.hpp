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

/* Completes [store], a vertex shader's reflection from its SPIR-V, with the
 * inputs the shader declares and its SPIR-V dropped (card A51), read from
 * [size] bytes of Slang's reflection JSON of the same shader
 * (-reflection-json): each field of a struct parameter, or parameter, bound
 * as a varying input, added at its location with `used` false. Nothing to do
 * for another stage, or a SPIR-V with several vertex entry points (no entry
 * point named; everything counts as used). False, with the reason in
 * [error], if the JSON can't be read, lacks the entry point, doesn't declare
 * an input the SPIR-V has at its location, or declares, unread, a matrix or
 * an array (its locations unknown here). [store] is then in no particular
 * state. Throws nothing. */
bool aprend_reflect_declared_inputs(const char *json, size_t size, aprend_reflection_store *store, std::string *error) noexcept;

/* [store]'s view, as a table, into [out]. False, with the reason in
 * [error], if a count or the table's size doesn't fit its 32-bit field.
 * Throws std::bad_alloc. */
bool aprend_reflect_table_write(const aprend_reflection_store &store, std::vector<uint8_t> *out, std::string *error);

#endif // APREND_REFLECT_TOOL_HPP
