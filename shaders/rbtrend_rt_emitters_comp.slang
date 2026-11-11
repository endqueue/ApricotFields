import rbtrend_rt_common;

// The emitter table of a TLAS (D6), for RBTREND_COMMAND_LIT_PATH_TRACE's
// light sampling: for each instance of its last build, the emissive
// triangles of the instances before it, then the total. A triangle is
// emissive where its geometry's material has an emission above 0 in any
// channel (a lit material's values, from the lit-material table). Built by
// the first path trace after anything it reads changed: a set with writes,
// the material table, or a lit material's values.
//
// One group of 256 threads, each over a run of instances in order: its
// sum, an exclusive scan of the sums across the group, then each run's
// prefixes written. The same records give the same table, whatever the
// device's scheduling.

[[vk::binding(0, 0)]] readonly RWByteAddressBuffer records : register(u0, space0);
[[vk::binding(1, 0)]] readonly RWByteAddressBuffer materials : register(u1, space0);
[[vk::binding(2, 0)]] readonly RWByteAddressBuffer blases : register(u2, space0);
[[vk::binding(3, 0)]] readonly RWByteAddressBuffer arena : register(u3, space0);
[[vk::binding(4, 0)]] readonly RWByteAddressBuffer lit : register(u4, space0);
[[vk::binding(5, 0)]] RWByteAddressBuffer emitters : register(u5, space0);

// rbtrend_rt_emitter_params: the instances of the last build, and the
// material table's length.
struct EmitterParams {
    uint count;
    uint material_count;
    uint2 padding;
};
[[vk::push_constant]] ConstantBuffer<EmitterParams> params : register(b0, space4);

static const uint GROUP = 256u;
groupshared uint s_sums[GROUP];

// The emissive triangles of instance [i].
uint emissive_triangles(uint i) {
    const uint4 tail  = records.Load4(i * RT_RECORD_BYTES + RT_RECORD_TAIL);
    // Slot 0 is no BLAS: an instance with no triangles.
    if ((tail.x & RT_SLOT_INDEX_MASK) == 0u)
        return 0u;
    const uint4 entry = blases.Load4((tail.x & RT_SLOT_INDEX_MASK) * RT_BLAS_ENTRY_BYTES);
    uint total = 0u;
    for (uint g = 0u; g < entry.w; ++g) {
        const uint m = tail.y + g;
        if (m >= params.material_count)
            continue;
        const uint slot = materials.Load(m * 4u);
        if (slot == 0u)
            continue;
        const float3 emission = asfloat(lit.Load3((slot & RT_SLOT_INDEX_MASK) * RT_LIT_ENTRY_BYTES + 16u));
        if (!any(emission > float3(0.0, 0.0, 0.0)))
            continue;
        const uint4 header = arena.Load4((entry.z + g * RT_GEOMETRY_HEADER_WORDS) * 4u);
        total += header.z;
    }
    return total;
}

[shader("compute")]
[numthreads(256, 1, 1)]
void rbtrend_rt_emitters_comp(uint3 thread : SV_GroupThreadID) {
    const uint n     = params.count;
    const uint per   = (n + GROUP - 1u) / GROUP;
    const uint begin = min(thread.x * per, n);
    const uint end   = min(begin + per, n);
    uint sum = 0u;
    for (uint i = begin; i < end; ++i)
        sum += emissive_triangles(i);
    s_sums[thread.x] = sum;
    GroupMemoryBarrierWithGroupSync();
    // Hillis and Steele's inclusive scan over the group's sums.
    for (uint offset = 1u; offset < GROUP; offset <<= 1u) {
        const uint before = thread.x >= offset ? s_sums[thread.x - offset] : 0u;
        GroupMemoryBarrierWithGroupSync();
        s_sums[thread.x] += before;
        GroupMemoryBarrierWithGroupSync();
    }
    uint prefix = s_sums[thread.x] - sum;
    for (uint i = begin; i < end; ++i) {
        emitters.Store(i * 4u, prefix);
        prefix += emissive_triangles(i);
    }
    if (thread.x == GROUP - 1u)
        emitters.Store(n * 4u, s_sums[GROUP - 1u]);
}
