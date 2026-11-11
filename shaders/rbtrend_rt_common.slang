module rbtrend_rt_common;

// What Rend's ray tracing shaders share (rbtrendraytracing.h): the layouts
// of the tables they read, each mirrored by a struct of
// src/render/rbtrendraytracing_internal.hpp whose size is static_asserted
// there. Every table is a byte-address buffer, read with Load at the byte
// offsets these constants give, as a storage buffer is on every backend.

// RBTREND_RAY_INSTANCE: 64 bytes, the 3x4 transform's rows at 0, 16 and 32,
// then the BLAS's slot, the material base, the mask and a reserved word.
public static const uint RT_RECORD_BYTES = 64u;
public static const uint RT_RECORD_TAIL = 48u;
// rbtrend_rt_blas_entry: the structure's reference (two words), where its
// geometry headers start in the arena (in words), its geometry count.
public static const uint RT_BLAS_ENTRY_BYTES = 16u;
// rbtrend_rt_geometry_header: where its vertices and indices start (in
// words), its triangle count, whether it is indexed.
public static const uint RT_GEOMETRY_HEADER_WORDS = 4u;
// RBTREND_LIT_VERTEX: 12 words, the position first, then the normal and uv.
public static const uint RT_VERTEX_WORDS = 12u;
// rbtrend_rt_lit_entry: the base color, the emission, then roughness,
// metalness and specular.
public static const uint RT_LIT_ENTRY_BYTES = 48u;
// A slot's index (D1, D4).
public static const uint RT_SLOT_INDEX_MASK = 0xFFFFFFu;

// The instance writers' push constants (rbtrend_rt_writer_params).
public struct WriterParams {
    public uint count;
    public uint instance_size;
    public uint2 padding;
};
