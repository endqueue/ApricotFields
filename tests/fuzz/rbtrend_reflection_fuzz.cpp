// rbtrend_reflection_fuzz - Rend's reader of a shader package's reflection
// table (src/render/rbtrendreflect_table.cpp, rbtrend_reflect_table_read()),
// which rbtrend_shader_create() runs on bytes a caller vouches for only by
// having built them.
//
// The input is a stage (one byte: 0 vertex, 1 fragment, 2 compute, 3
// mesh), the package's entry point (bytes up to a 0), then the table. The
// seeds are tables tools/rbtrend_reflect wrote for Rend's own shaders.
// Beyond not crashing, it checks:
// - a failed read gives a reason and one of the reader's codes;
// - a read table's view indexes its own arrays: every block's members, the
//   push constants' members, names that are strings.

#include "rbtfuzz.hpp"

#include "rbtrendreflect_internal.hpp"

#include <string.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 2)
        return 0;
    static const CRGPU_SHADER_STAGE stages[4] = {CRGPU_SHADER_STAGE_VERTEX, CRGPU_SHADER_STAGE_FRAGMENT,
                                                   CRGPU_SHADER_STAGE_COMPUTE, CRGPU_SHADER_STAGE_MESH};
    const CRGPU_SHADER_STAGE stage = stages[data[0] % 4];
    const uint8_t *name = data + 1;
    const uint8_t *end = (const uint8_t *)memchr(name, 0, size - 1);
    if (!end)
        return 0;
    const std::string entry_point((const char *)name, (size_t)(end - name));
    const uint8_t *table = end + 1;
    const size_t table_size = size - (size_t)(table - data);
    // The table as a package holds it: its own 4-byte aligned bytes.
    std::vector<uint32_t> words((table_size + 3) / 4);
    if (table_size)
        memcpy(words.data(), table, table_size);

    rbtrend_reflection_store store;
    std::string error;
    const RBT_RESULT result = rbtrend_reflect_table_read(words.data(), table_size, stage, entry_point.c_str(), &store, &error);
    if (result != RBT_RESULT_OK)
    {
        RBTFUZZ_CHECK(result == RBT_RESULT_CORRUPT_DATA || result == RBT_RESULT_REND_REFLECTION_MISMATCH ||
                       result == RBT_RESULT_OUT_OF_MEMORY);
        RBTFUZZ_CHECK(!error.empty());
        return 0;
    }
    const RBTREND_SHADER_REFLECTION &view = store.view;
    RBTFUZZ_CHECK(view.stage == stage);
    RBTFUZZ_CHECK(!view.entry_point || entry_point == view.entry_point);
    for (uint32_t i = 0; i < view.resource_count; ++i)
    {
        const RBTREND_SHADER_RESOURCE &r = view.resources[i];
        RBTFUZZ_CHECK(r.name != nullptr);
        RBTFUZZ_CHECK((uint64_t)r.first_member + r.member_count <= view.member_count);
    }
    for (uint32_t i = 0; i < view.member_count; ++i)
        RBTFUZZ_CHECK(view.members[i].name != nullptr);
    for (uint32_t i = 0; i < view.input_count; ++i)
        RBTFUZZ_CHECK(view.inputs[i].name != nullptr);
    RBTFUZZ_CHECK((uint64_t)view.push_constant_first_member + view.push_constant_member_count <= view.member_count);
    RBTFUZZ_CHECK(store.output_types.size() == view.color_output_count);
    return 0;
}
