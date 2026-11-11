import rbtrend_rt_common;

// Rend's GPU instance writer for a device whose instance layout is
// CRGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_COLUMN_MAJOR_72 (Metal's
// indirect instance descriptor; card S14), one thread an instance: as
// rbtrend_rt_instances_row64_comp.slang, in Metal's 72 bytes. The transform
// as four columns of three floats; then the options (triangle culling
// disabled, D8), the mask, the intersection function table offset (0), the
// user id (the material base, D4) and the structure's resource id.
// Written and built on Windows; it runs on Metal only.

[[vk::binding(0, 0)]] readonly RWByteAddressBuffer records : register(u0, space0);
[[vk::binding(1, 0)]] readonly RWByteAddressBuffer blases : register(u1, space0);
[[vk::binding(2, 0)]] RWByteAddressBuffer instances : register(u2, space0);
[[vk::push_constant]] ConstantBuffer<WriterParams> params : register(b0, space4);

// MTLAccelerationStructureInstanceOptionDisableTriangleCulling.
static const uint OPTION_DISABLE_TRIANGLE_CULLING = 1u;

[shader("compute")]
[numthreads(64, 1, 1)]
void rbtrend_rt_instances_col72_comp(uint3 id : SV_DispatchThreadID) {
    if (id.x >= params.count)
        return;
    const uint src = id.x * RT_RECORD_BYTES;
    const uint dst = id.x * params.instance_size;
    const uint4 rows[3] = {records.Load4(src), records.Load4(src + 16u), records.Load4(src + 32u)};
    const uint4 tail = records.Load4(src + RT_RECORD_TAIL);
    // Slot 0 is no BLAS: an inactive instance, as the 64-byte writer's.
    const uint index  = tail.x & RT_SLOT_INDEX_MASK;
    const uint4 entry = index != 0u ? blases.Load4(index * RT_BLAS_ENTRY_BYTES) : uint4(0u, 0u, 0u, 0u);
    // Column c, row r.
    for (uint c = 0; c < 4u; ++c)
        for (uint r = 0; r < 3u; ++r)
            instances.Store(dst + (c * 3u + r) * 4u, rows[r][c]);
    instances.Store(dst + 48u, OPTION_DISABLE_TRIANGLE_CULLING);
    instances.Store(dst + 52u, tail.z);
    instances.Store(dst + 56u, 0u);
    instances.Store(dst + 60u, tail.y & 0xFFFFFFu);
    instances.Store2(dst + 64u, entry.xy);
}
