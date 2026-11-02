// aprend_reflection_fuzz - Aprend's reader of a shader package's reflection
// table (src/render/aprendreflect_table.cpp, aprend_reflect_table_read()),
// which aprend_shader_create() runs on bytes a caller vouches for only by
// having built them.
//
// The input is a stage (one byte: 0 vertex, 1 fragment, 2 compute, 3
// mesh), the package's entry point (bytes up to a 0), then the table. The
// seeds are tables tools/aprend_reflect wrote for Aprend's own shaders.
// Beyond not crashing, it checks:
// - a failed read gives a reason and one of the reader's codes;
// - a read table's view indexes its own arrays: every block's members, the
//   push constants' members, names that are strings.

#include "aprifuzz.hpp"

#include "aprendreflect_internal.hpp"

#include <string.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 2)
        return 0;
    static const SPUDGPU_SHADER_STAGE stages[4] = {SPUDGPU_SHADER_STAGE_VERTEX, SPUDGPU_SHADER_STAGE_FRAGMENT,
                                                   SPUDGPU_SHADER_STAGE_COMPUTE, SPUDGPU_SHADER_STAGE_MESH};
    const SPUDGPU_SHADER_STAGE stage = stages[data[0] % 4];
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

    aprend_reflection_store store;
    std::string error;
    const APRESULT result = aprend_reflect_table_read(words.data(), table_size, stage, entry_point.c_str(), &store, &error);
    if (result != APRESULT_OK)
    {
        APRIFUZZ_CHECK(result == APRESULT_CORRUPT_DATA || result == APRESULT_APREND_REFLECTION_MISMATCH ||
                       result == APRESULT_OUT_OF_MEMORY);
        APRIFUZZ_CHECK(!error.empty());
        return 0;
    }
    const APREND_SHADER_REFLECTION &view = store.view;
    APRIFUZZ_CHECK(view.stage == stage);
    APRIFUZZ_CHECK(!view.entry_point || entry_point == view.entry_point);
    for (uint32_t i = 0; i < view.resource_count; ++i)
    {
        const APREND_SHADER_RESOURCE &r = view.resources[i];
        APRIFUZZ_CHECK(r.name != nullptr);
        APRIFUZZ_CHECK((uint64_t)r.first_member + r.member_count <= view.member_count);
    }
    for (uint32_t i = 0; i < view.member_count; ++i)
        APRIFUZZ_CHECK(view.members[i].name != nullptr);
    for (uint32_t i = 0; i < view.input_count; ++i)
        APRIFUZZ_CHECK(view.inputs[i].name != nullptr);
    APRIFUZZ_CHECK((uint64_t)view.push_constant_first_member + view.push_constant_member_count <= view.member_count);
    APRIFUZZ_CHECK(store.output_types.size() == view.color_output_count);
    return 0;
}
