import rbtrend_rt_common;

// Rend's GPU instance writer for a device whose instance layout is
// CRGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_ROW_MAJOR_64 (Vulkan,
// D3D12; card S14), one thread an instance: the TLAS's current record
// (RBTREND_RAY_INSTANCE) written in the device's 64 bytes, its BLAS's slot
// resolved to the BLAS's current structure through the BLAS table (D3, D5).
// The row-major 3x4 transform as it is; the 24-bit user index is the
// record's material base (D4), the 8-bit mask its mask; the table offset 0,
// and the one flag every instance has, triangle culling disabled (D8); the
// structure's 64-bit reference.

[[vk::binding(0, 0)]] readonly RWByteAddressBuffer records : register(u0, space0);
[[vk::binding(1, 0)]] readonly RWByteAddressBuffer blases : register(u1, space0);
[[vk::binding(2, 0)]] RWByteAddressBuffer instances : register(u2, space0);
[[vk::push_constant]] ConstantBuffer<WriterParams> params : register(b0, space4);

// Carrottus GPU's portable CRGPU_ACCELERATION_STRUCTURE_INSTANCE_DISABLE_TRIANGLE_CULLING,
// as both APIs lay it out in this form (D3D12's TRIANGLE_CULL_DISABLE,
// Vulkan's TRIANGLE_FACING_CULL_DISABLE).
static const uint TRIANGLE_CULL_DISABLE = 1u;

[shader("compute")]
[numthreads(64, 1, 1)]
void rbtrend_rt_instances_row64_comp(uint3 id : SV_DispatchThreadID) {
    if (id.x >= params.count)
        return;
    const uint src = id.x * RT_RECORD_BYTES;
    const uint dst = id.x * params.instance_size;
    const uint4 tail = records.Load4(src + RT_RECORD_TAIL);
    // Slot 0 is no BLAS: a record never written, an inactive instance (a
    // reference of 0, which no hit reaches).
    const uint index  = tail.x & RT_SLOT_INDEX_MASK;
    const uint4 entry = index != 0u ? blases.Load4(index * RT_BLAS_ENTRY_BYTES) : uint4(0u, 0u, 0u, 0u);
    instances.Store4(dst, records.Load4(src));
    instances.Store4(dst + 16u, records.Load4(src + 16u));
    instances.Store4(dst + 32u, records.Load4(src + 32u));
    instances.Store(dst + 48u, (tail.y & 0xFFFFFFu) | (tail.z << 24));
    instances.Store(dst + 52u, TRIANGLE_CULL_DISABLE << 24);
    instances.Store2(dst + 56u, entry.xy);
}
