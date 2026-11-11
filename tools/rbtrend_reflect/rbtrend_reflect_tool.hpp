#ifndef RBTREND_REFLECT_TOOL_HPP
#define RBTREND_REFLECT_TOOL_HPP

/* tools/rbtrend_reflect's own: reflecting a shader's SPIR-V into the store
 * Rend reads (rbtrendreflect_internal.hpp), and writing the store as the
 * table a package carries (rbtrendreflect_table.hpp). Built into the tool and
 * into Rabbittus' rbtrendreflect_tests, never into the library. */

#include "rbtrendreflect_internal.hpp"
#include "rbtrendreflect_table.hpp"

#include <string>
#include <vector>

/* Reflects [word_count] words of SPIR-V, the code of a shader of [stage],
 * into [out], which is empty. False, with what was wrong in [error], if the
 * code isn't SPIR-V, has no entry point for [stage], or its entry point uses
 * a resource Rend has no binding for. [out] is then in no particular
 * state, and is only destroyed. Throws nothing: SPIRV-Cross's exceptions
 * and std::bad_alloc are caught here. */
bool rbtrend_reflect_spirv(
    const uint32_t *words,
    size_t word_count,
    CRGPU_SHADER_STAGE stage,
    rbtrend_reflection_store *out,
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
bool rbtrend_reflect_declared_inputs(const char *json, size_t size, rbtrend_reflection_store *store, std::string *error) noexcept;

/* [store]'s view, as a table, into [out]. False, with the reason in
 * [error], if a count or the table's size doesn't fit its 32-bit field.
 * Throws std::bad_alloc. */
bool rbtrend_reflect_table_write(const rbtrend_reflection_store &store, std::vector<uint8_t> *out, std::string *error);

#endif // RBTREND_REFLECT_TOOL_HPP
