cmake_minimum_required(VERSION 3.30)

# The version is written once, in include/rbtcore.h; read it from there.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/include/rbtcore.h" _rbt_version_lines
     REGEX "^#define RABBITTUS_VERSION_(MAJOR|MINOR|PATCH) [0-9]+$")
foreach(_part MAJOR MINOR PATCH)
    string(REGEX REPLACE ".*RABBITTUS_VERSION_${_part} ([0-9]+).*" "\\1"
           RABBITTUS_VERSION_${_part} "${_rbt_version_lines}")
    if(NOT RABBITTUS_VERSION_${_part} MATCHES "^[0-9]+$")
        message(FATAL_ERROR "rabbittus: no RABBITTUS_VERSION_${_part} line in include/rbtcore.h")
    endif()
endforeach()
project(rabbittus VERSION ${RABBITTUS_VERSION_MAJOR}.${RABBITTUS_VERSION_MINOR}.${RABBITTUS_VERSION_PATCH})

# CMake knows no C++26 flag for MSVC; its C++23 maps to /std:c++latest,
# the newest dialect MSVC offers.
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(CMAKE_CXX_STANDARD 23)
else()
    set(CMAKE_CXX_STANDARD 26)
endif()

add_library(rabbittus STATIC
        src/rbtcore.cpp
        src/rbthashindex.hpp
        src/rbtplaindata.hpp
        src/rbtstructbase.hpp
        src/render/rbtrend_internal.hpp
        src/render/rbtrendcontext.cpp
        src/render/rbtrendbuffers.cpp
        src/render/rbtrendranges_internal.hpp
        src/render/rbtrendpools_internal.hpp
        src/render/rbtrendpools.cpp
        src/render/rbtrendimages_internal.hpp
        src/render/rbtrendimages.cpp
        src/render/rbtrendframes.cpp
        src/render/rbtrendcommands.cpp
        src/render/rbtrenddrawlist.cpp
        src/render/rbtrendmaterial.cpp
        src/render/rbtrendpass.cpp
        src/render/rbtrendreflect_internal.hpp
        src/render/rbtrendreflect.cpp
        src/render/rbtrendreflect_table.hpp
        src/render/rbtrendreflect_table.cpp
        src/render/rbtrendpipeline.cpp
        src/render/rbtrendswapchain.cpp
        src/render/rbtrendtransient_internal.hpp
        src/render/rbtrendtransient.cpp
        src/render/rbtrendbatch.cpp
        src/render/rbtrendtimer.cpp
        src/render/rbtrendbuiltin_internal.hpp
        src/render/rbtrendbuiltin.cpp
        src/render/rbtrendlit_internal.hpp
        src/render/rbtrendlit.cpp
        src/render/rbtrendrendertargets_internal.hpp
        src/render/rbtrendrendertargets.cpp
        src/render/rbtrendraytracing_internal.hpp
        src/render/rbtrendraytracing.cpp
        src/archive/rbtarchive_internal.hpp
        src/archive/rbtarchive.cpp
        src/audio/rbtaudio_internal.hpp
        src/audio/rbtaudio_stream.hpp
        src/audio/rbtaudio_stream_reader.h
        src/audio/rbtaudio.cpp
        src/audio/rbtaudio_decode.cpp
        src/audio/rbtaudio_encode.cpp
        src/audio/rbtaudiocues.cpp
        src/math/rbtmathbvh.cpp
        src/math/rbtmathbvh_internal.hpp
        src/math/rbtmathpoly_internal.hpp
        src/math/rbtmathpoly.cpp
        src/sync/rbtsync_internal.hpp
        src/sync/rbtsyncnet.cpp
        src/text/rbttext_internal.hpp
        src/text/rbttext.cpp
        src/text/rbttext_cadfonts.cpp
        src/text/rbttext_layout.cpp
        src/text/rbttext_atlas.cpp
        src/jobs/rbtjobs.cpp
        src/rbtoverlap.hpp
        src/rbtstale.hpp
        include/rbtcore.h
        include/rbtmath.h
        include/rbtrend.h
        include/render/rbtrendcontext.h
        include/render/rbtrendbuffers.h
        include/render/rbtrendimages.h
        include/render/rbtrendframes.h
        include/render/rbtrendcommands.h
        include/render/rbtrenddrawlist.h
        include/render/rbtrendmaterial.h
        include/render/rbtrendpass.h
        include/render/rbtrendshader.h
        include/render/rbtrendpipeline.h
        include/render/rbtrendswapchain.h
        include/render/rbtrendtransient.h
        include/render/rbtrendtimer.h
        include/render/rbtrendlit.h
        include/render/rbtrendpost.h
        include/render/rbtrendbatch.h
        include/render/rbtrendraytracing.h
        include/archive/rbtarchive.h
        include/audio/rbtaudio.h
        include/audio/rbtaudiocues.h
        include/math/rbtmathbvh.h
        include/math/rbtmatherror.h
        include/math/rbtmathpoly.h
        include/sync/rbtsyncnet.h
        include/text/rbttext.h
        include/jobs/rbtjobs.h
        )

target_include_directories(rabbittus PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# Internal helpers may throw std::bad_alloc and the C entry points catch it
# ("Writing a new module"), and many helpers are defined inside the
# entry points' extern "C" blocks. MSVC's default /EHsc assumes a function
# of C linkage never throws, may drop the catch around a call to one, and
# warns (C4297): /EHs without the c takes that assumption away, for every
# target in this directory. Clang and GCC never make it.
if (MSVC)
    string(REPLACE "/EHsc" "/EHs" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif ()

# Add the directory for Carrottus library
if (NOT TARGET carrottus)
    add_subdirectory(../carrottus ${CMAKE_CURRENT_BINARY_DIR}/carrottus)
endif ()
# Link Carrottus library
target_link_libraries(rabbittus PUBLIC carrottus)

# --- vendored-or-fetched dependency ----------------------------------------
# Probe eqdev/.deps/<dir> for a hand-vendored checkout and use it if present
# (one copy shared across the whole workspace, no per-repo duplication),
# otherwise fetch <repo> at the pinned <tag> so a standalone clone of this repo
# still configures with nothing vendored by hand. Probe <dir>/CMakeLists.txt,
# not just the directory, so an empty placeholder folder doesn't match.
include(FetchContent)
function(declare_vendored_or_fetch name dir repo tag)
    # Accept either the plain `git clone` folder name (<dir>) or the
    # FetchContent-style <dir>-src, so a vendored checkout is found without
    # renaming. Probe <cand>/CMakeLists.txt, not just the directory, so an
    # empty placeholder folder doesn't match.
    foreach (_cand "${dir}" "${dir}-src")
        set(_vendored "${CMAKE_CURRENT_SOURCE_DIR}/../.deps/${_cand}")
        if (EXISTS "${_vendored}/CMakeLists.txt")
            message(STATUS "${name}: using vendored ${_vendored}")
            FetchContent_Declare(${name} SOURCE_DIR "${_vendored}")
            return ()
        endif ()
    endforeach ()
    message(STATUS "${name}: no vendored copy under .deps/, fetching ${repo}@${tag}")
    FetchContent_Declare(${name} GIT_REPOSITORY "${repo}" GIT_TAG "${tag}")
endfunction()

# The same for a dependency whose sources are built here rather than through
# its own CMakeLists.txt: probes for [probe], a file inside the checkout, and
# points SOURCE_SUBDIR at a directory that doesn't exist, so
# FetchContent_MakeAvailable only downloads it (or finds the vendored copy)
# and <name>_SOURCE_DIR says where.
function(declare_vendored_or_fetch_sources name dir repo tag probe)
    foreach (_cand "${dir}" "${dir}-src")
        set(_vendored "${CMAKE_CURRENT_SOURCE_DIR}/../.deps/${_cand}")
        if (EXISTS "${_vendored}/${probe}")
            message(STATUS "${name}: using vendored ${_vendored}")
            FetchContent_Declare(${name} SOURCE_DIR "${_vendored}" SOURCE_SUBDIR sources-only-not-a-directory)
            return ()
        endif ()
    endforeach ()
    message(STATUS "${name}: no vendored copy under .deps/, fetching ${repo}@${tag}")
    # A shallow clone holds only the tip of the ref it names, so it can't
    # check out a commit hash once upstream has moved past it (CMake's
    # GIT_SHALLOW is for tag and branch names): a pinned commit is cloned
    # whole. Only sources are taken, so no submodule is cloned: a project's
    # own (dr_libs' tests/external/miniaudio, a repository that no longer
    # clones) would fail the configure for files never used.
    if (tag MATCHES "^[0-9a-f]+$")
        set(_shallow FALSE)
    else ()
        set(_shallow TRUE)
    endif ()
    FetchContent_Declare(${name} GIT_REPOSITORY "${repo}" GIT_TAG "${tag}" GIT_SHALLOW ${_shallow}
            GIT_SUBMODULES "" SOURCE_SUBDIR sources-only-not-a-directory)
endfunction()

# RbtSync runs each session on a thread of its own (src/sync/rbtsyncnet.cpp).
find_package(Threads REQUIRED)
target_link_libraries(rabbittus PRIVATE Threads::Threads)

# Rbtrend's own shaders (shaders/*_vert.slang, _frag, _comp), Slang compiled at
# build time to SPIR-V, DXIL on Windows and a .metallib on Apple platforms
# (cmake/rbtrend_slang.cmake), and embedded in the library as one package
# each: there is nothing to deploy or find at run time, and nothing is
# compiled at run time. src/render/rbtrendbuiltin.cpp, rbtrendbatch.cpp and
# rbtrendlit.cpp include them.
include(cmake/rbtrend_slang.cmake)
set(RABBITTUS_SHADER_INC_DIR ${CMAKE_CURRENT_BINARY_DIR}/rbtrend_shaders)
set(RABBITTUS_SHADERS
        rbtrend_fullscreen_vert
        rbtrend_downsample_frag
        rbtrend_present_frag
        rbtrend_bloom_prefilter_frag
        rbtrend_bloom_downsample_frag
        rbtrend_bloom_upsample_frag
        rbtrend_bloom_composite_frag
        rbtrend_tone_map_frag
        rbtrend_blur_frag
        rbtrend_outline_frag
        rbtrend_lit_vert
        rbtrend_lit_color_vert
        rbtrend_lit_frag
        rbtrend_lit_surface_reflect_frag
        rbtrend_lit_shadow_vert
        rbtrend_lit_shadow_color_vert
        rbtrend_lit_depth_frag
        rbtrend_environment_from_cube_frag
        rbtrend_environment_from_equirect_frag
        rbtrend_environment_prefilter_frag
        rbtrend_environment_irradiance_frag
        rbtrend_environment_sky_frag
        rbtrend_environment_hosek_frag
        rbtrend_lit_depth_cutout_frag
        rbtrend_lit_sky_frag
        rbtrend_lit_cull_comp
        rbtrend_lit_cull_bounded_comp
        rbtrend_lit_brdf_lut_frag
        rbtrend_lit_occlusion_frag
        rbtrend_lit_occlusion_blur_frag
        rbtrend_lit_hiz_frag
        rbtrend_lit_hiz_down_frag
        rbtrend_lit_ssr_comp
        rbtrend_lit_reflect_frag
        rbtrend_exposure_luminance_frag
        rbtrend_exposure_adapt_frag
        rbtrend_exposure_histogram_comp
        rbtrend_exposure_adapt_histogram_frag
        rbtrend_batch_vert
        rbtrend_batch_quad_frag
        rbtrend_batch_circle_frag
        rbtrend_batch_line_frag
        rbtrend_batch_glyph_frag
        rbtrend_rt_instances_row64_comp
        rbtrend_rt_instances_col72_comp
        rbtrend_rt_emitters_comp
)
# The modules the shaders import, from shaders/: every shader is compiled
# again when one of them changes.
set(RABBITTUS_SHADER_MODULES
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/rbtrend_lit_scene.slang
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/rbtrend_lit_cull.slang
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/rbtrend_lit_vertex.slang
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/rbtrend_environment.slang
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/rbtrend_lit_material.slang
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/rbtrend_lit_bindings.slang
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/rbtrend_lit_surface.slang
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/rbtrend_lit_shadow_pass.slang
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/rbtrend_batch.slang
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/rbtrend_batch_inputs.slang
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/rbtrend_rt_common.slang
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/rbtrend_rt_trace.slang
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/rbtrend_rt_shadow_mask.slang
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/rbtrend_rt_path_trace.slang
)
# The shaders that trace (rbtrendraytracing.h): inline ray queries, SPIR-V's
# ray query capability (SPV_KHR_ray_query) and DXIL's shader model 6.5. Made
# only on a device with CRGPU_FEATURE_RAY_QUERY.
set(RABBITTUS_TRACE_SHADERS
        rbtrend_rt_shadow_mask_r8_comp
        rbtrend_rt_shadow_mask_r16f_comp
        rbtrend_rt_path_trace_f32_f32_comp
        rbtrend_rt_path_trace_f32_f16_comp
        rbtrend_rt_path_trace_f16_f32_comp
        rbtrend_rt_path_trace_f16_f16_comp
        rbtrend_rt_reflections_comp
)
foreach (shader IN LISTS RABBITTUS_SHADERS)
    rbtrend_add_slang_shader(RABBITTUS_SHADER_OUTPUTS
            SOURCE shaders/${shader}.slang
            OUT_DIR ${RABBITTUS_SHADER_INC_DIR}
            INCLUDE_DIRS shaders
            DEPENDS ${RABBITTUS_SHADER_MODULES})
endforeach ()
foreach (shader IN LISTS RABBITTUS_TRACE_SHADERS)
    rbtrend_add_slang_shader(RABBITTUS_SHADER_OUTPUTS
            SOURCE shaders/${shader}.slang
            OUT_DIR ${RABBITTUS_SHADER_INC_DIR}
            INCLUDE_DIRS shaders
            DEPENDS ${RABBITTUS_SHADER_MODULES}
            DXIL_PROFILE cs_6_5
            SPIRV_CAPABILITIES spvRayQueryKHR)
endforeach ()
add_custom_target(rabbittus_shaders DEPENDS ${RABBITTUS_SHADER_OUTPUTS})
add_dependencies(rabbittus rabbittus_shaders)
target_include_directories(rabbittus PRIVATE ${RABBITTUS_SHADER_INC_DIR})

# GLM — header-only math library used internally by the C++ backends
declare_vendored_or_fetch(glm glm https://github.com/g-truc/glm.git 1.0.1)
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
FetchContent_MakeAvailable(glm)
target_link_libraries(rabbittus PUBLIC glm::glm)

# libdeflate - DEFLATE and CRC-32 for RbtArchive. Vendored under
# eqdev/.deps/libdeflate (git checkout of tag v1.26), like glm and
# SPIRV-Cross. Pinned on purpose: DEFLATE output is part of what makes an
# archive byte-identical, and a libdeflate upgrade can change it. Only the
# static library is built. Gzip support stays on because libdeflate only
# compiles its CRC-32 with it; the zlib wrapper is off.
set(LIBDEFLATE_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_ZLIB_SUPPORT OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_GZIP_SUPPORT ON CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_GZIP OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_INSTALL OFF CACHE BOOL "" FORCE)
declare_vendored_or_fetch(libdeflate libdeflate
        https://github.com/ebiggers/libdeflate.git v1.26)
FetchContent_MakeAvailable(libdeflate)
# rabbittus may be linked into a shared library too.
set_target_properties(libdeflate_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(rabbittus PRIVATE libdeflate::libdeflate_static)

# zlib-ng 2.3.3 (zlib licence), native mode - streamed DEFLATE for
# RbtArchive's entries written and read in pieces (card A24, decided
# 2026-10-28). libdeflate has no streaming and stays for whole entries.
# Pinned for the reason libdeflate is: a streamed entry's bytes are fixed by
# zlib-ng's version and level (tests/rbtarchive_tests.cpp records them).
# Static, through its own CMakeLists.txt, with no zlib-compatible names, no
# gz file functions and nothing else.
# BUILD_SHARED_LIBS and BUILD_TESTING are set for its directory alone, as
# normal variables its options honour, and put back after.
set(ZLIB_COMPAT OFF CACHE BOOL "" FORCE)
set(ZLIB_ALIASES OFF CACHE BOOL "" FORCE)
set(WITH_GTEST OFF CACHE BOOL "" FORCE)
set(WITH_GZFILEOP OFF CACHE BOOL "" FORCE)
set(WITH_FUZZERS OFF CACHE BOOL "" FORCE)
set(WITH_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(WITH_BENCHMARK_APPS OFF CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
foreach (_var BUILD_SHARED_LIBS BUILD_TESTING)
    if (DEFINED ${_var})
        set(_apricot_saved_${_var} "${${_var}}")
    endif ()
    set(${_var} OFF)
endforeach ()
declare_vendored_or_fetch(zlibng zlib-ng https://github.com/zlib-ng/zlib-ng.git 2.3.3)
FetchContent_MakeAvailable(zlibng)
foreach (_var BUILD_SHARED_LIBS BUILD_TESTING)
    if (DEFINED _apricot_saved_${_var})
        set(${_var} "${_apricot_saved_${_var}}")
        unset(_apricot_saved_${_var})
    else ()
        unset(${_var})
    endif ()
endforeach ()
set_target_properties(zlib-ng PROPERTIES POSITION_INDEPENDENT_CODE ON)
# GCC's -Wtsan: zlib-ng's functable.c ends its CPU-dispatch setup with a
# sequentially consistent fence, which ThreadSanitizer doesn't model. Every
# store before it is already a sequentially consistent atomic store, which
# it does, so the fence hides nothing from it; the file is zlib-ng's, not
# edited. GCC only (Clang has no such warning), and only under the
# sanitizer.
if (CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_FLAGS MATCHES "-fsanitize=thread")
    target_compile_options(zlib-ng PRIVATE -Wno-tsan)
endif ()
target_link_libraries(rabbittus PRIVATE zlib-ng)

# glaze - JSON, for RbtAudio's sound bank manifests (src/audio/rbtaudiocues.cpp).
# Header only, vendored under eqdev/.deps/glaze like libdeflate. Private: no
# glaze type crosses rabbittus' C API.
# Pinned to the release v9.0.0, by its commit (a tag can be moved upstream, a
# commit can't). eqdev/.deps/glaze is checked out at the same tag, so a fetched
# build and a vendored one use the same glaze. Move both together.
declare_vendored_or_fetch(glaze glaze https://github.com/stephenberry/glaze.git d78832c82289c61a9315bfbc35332cec9f4e93ca) # v9.0.0
FetchContent_MakeAvailable(glaze)
target_link_libraries(rabbittus PRIVATE glaze::glaze)

# Clipper2 - polygon booleans for RbtMath's polygons (src/math/rbtmathpoly.cpp).
# Boost licence. Only its library sources are used, built here: its own
# CMakeLists.txt treats warnings as errors and builds tests and examples.
# Pinned on purpose, like libdeflate: where its intersections land on the grid
# is part of what makes a boolean's result the same on every build. Built with
# floating-point contraction off for the same reason - a fused multiply-add
# rounds differently from the two operations it replaces (see the note at the
# top of rbtmathpoly.cpp).
declare_vendored_or_fetch_sources(clipper2 Clipper2
        https://github.com/AngusJohnson/Clipper2.git Clipper2_1.5.4
        CPP/Clipper2Lib/src/clipper.engine.cpp)
FetchContent_MakeAvailable(clipper2)
add_library(rbtmath_clipper2 STATIC
        ${clipper2_SOURCE_DIR}/CPP/Clipper2Lib/src/clipper.engine.cpp
        ${clipper2_SOURCE_DIR}/CPP/Clipper2Lib/src/clipper.offset.cpp
        ${clipper2_SOURCE_DIR}/CPP/Clipper2Lib/src/clipper.rectclip.cpp)
target_include_directories(rbtmath_clipper2 PUBLIC ${clipper2_SOURCE_DIR}/CPP/Clipper2Lib/include)
set_target_properties(rbtmath_clipper2 PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(rabbittus PRIVATE rbtmath_clipper2)

# earcut.hpp - triangulation for RbtMath's polygons. ISC licence, header only;
# its CMakeLists.txt builds its tests, so only the header is used. Pinned:
# which triangles it makes is part of a triangulation's result.
declare_vendored_or_fetch_sources(earcut earcut.hpp
        https://github.com/mapbox/earcut.hpp.git v2.2.4
        include/mapbox/earcut.hpp)
FetchContent_MakeAvailable(earcut)
target_include_directories(rabbittus PRIVATE ${earcut_SOURCE_DIR}/include)

# eqstd - the containers RbtText's sources grow their arrays with (eq::array,
# eq::vector; CLAUDE.md, "Containers: eqstd"). Header-only, PRIVATE: no
# public header includes it. Carrottus, built before this, has usually added
# the target already from the same workspace checkout.
if (NOT TARGET eqstd)
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../eqstd/CMakeLists.txt")
        message(STATUS "eqstd: using the workspace's ${CMAKE_CURRENT_SOURCE_DIR}/../eqstd")
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../eqstd" "${CMAKE_CURRENT_BINARY_DIR}/eqstd")
    else ()
        message(STATUS "eqstd: no ../eqstd beside rabbittus, fetching endqueue/eqstd")
        FetchContent_Declare(eqstd GIT_REPOSITORY https://github.com/endqueue/eqstd.git
                GIT_TAG b302f96b6aa644ab59a8b9cd23d88aaa4aa5b2dd) # eqstd 0.1.0
        FetchContent_MakeAvailable(eqstd)
    endif ()
endif ()
target_link_libraries(rabbittus PRIVATE eqstd)

# RbtText's four dependencies (RBTTEXT_TODO.md, "Dependencies"), each built
# here from its sources as a static library of its own, linked PRIVATE: no
# HarfBuzz, msdfgen, SheenBidi or libunibreak type is in include/. Pinned,
# because what each computes is part of RbtText's output: a layout and an
# atlas's pixels must be the same bits on every build. Upgrading one is a
# decision, with the tests' expected values checked again. Each is built
# with floating-point contraction off, as RbtMath's polygons are.
#
# HarfBuzz 14.6.0 (MIT): shaping, and glyph outlines through its draw
# callbacks, so FreeType isn't needed. Its one-file amalgamation, with no
# optional dependency (no FreeType, GLib, ICU or CoreText): its own Unicode
# data, the same on every platform.
declare_vendored_or_fetch_sources(harfbuzz harfbuzz
        https://github.com/harfbuzz/harfbuzz.git 14.6.0
        src/harfbuzz.cc)
FetchContent_MakeAvailable(harfbuzz)
add_library(rbttext_harfbuzz STATIC ${harfbuzz_SOURCE_DIR}/src/harfbuzz.cc)
target_include_directories(rbttext_harfbuzz PUBLIC ${harfbuzz_SOURCE_DIR}/src)
set_target_properties(rbttext_harfbuzz PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_STANDARD 17)
target_link_libraries(rabbittus PRIVATE rbttext_harfbuzz)

# msdfgen 1.13 (MIT): the MTSDF generator, its core only - not its ext
# (FreeType and file loading) and not msdf-atlas-gen (whose packer assumes a
# fixed glyph set).
declare_vendored_or_fetch_sources(msdfgen msdfgen
        https://github.com/Chlumsky/msdfgen.git v1.13
        core/msdfgen.cpp)
FetchContent_MakeAvailable(msdfgen)
set(RBTTEXT_MSDFGEN_SOURCES Contour DistanceMapping EdgeHolder MSDFErrorCorrection Projection Scanline Shape
        contour-combiners convergent-curve-ordering edge-coloring edge-segments edge-selectors equation-solver
        msdf-error-correction msdfgen rasterization sdf-error-estimation)
list(TRANSFORM RBTTEXT_MSDFGEN_SOURCES PREPEND ${msdfgen_SOURCE_DIR}/core/)
list(TRANSFORM RBTTEXT_MSDFGEN_SOURCES APPEND .cpp)
add_library(rbttext_msdfgen STATIC ${RBTTEXT_MSDFGEN_SOURCES})
target_include_directories(rbttext_msdfgen PUBLIC ${msdfgen_SOURCE_DIR})
# Defined empty, in place of the msdfgen-config.h its own CMakeLists.txt
# writes: a static library exports nothing.
target_compile_definitions(rbttext_msdfgen PUBLIC "MSDFGEN_PUBLIC=")
set_target_properties(rbttext_msdfgen PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_STANDARD 17)
target_link_libraries(rabbittus PRIVATE rbttext_msdfgen)

# SheenBidi 2.9.0 (Apache 2.0): Unicode's bidirectional algorithm and
# script runs. 2.9, not 3.0: 3.0 adds a process-wide default allocator a
# caller can replace, writable global state rabbittus may not have.
declare_vendored_or_fetch_sources(sheenbidi SheenBidi
        https://github.com/Tehreer/SheenBidi.git v2.9.0
        Source/SheenBidi.c)
FetchContent_MakeAvailable(sheenbidi)
add_library(rbttext_sheenbidi STATIC ${sheenbidi_SOURCE_DIR}/Source/SheenBidi.c)
target_compile_definitions(rbttext_sheenbidi PRIVATE SB_CONFIG_UNITY)
target_include_directories(rbttext_sheenbidi PUBLIC ${sheenbidi_SOURCE_DIR}/Headers
        PRIVATE ${sheenbidi_SOURCE_DIR}/Source)
set_target_properties(rbttext_sheenbidi PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(rabbittus PRIVATE rbttext_sheenbidi)

# libunibreak 8.0 (zlib): Unicode's line breaking algorithm. The sources
# its own Makefile.am compiles, less grapheme and word breaking: its
# linebreakauxdata.c, eastasianwidthdata.c and emojidata.c are included by
# linebreak.c, eastasianwidthdef.c and emojidef.c, and compiled on their
# own they define ub_is_op_east_asian twice.
declare_vendored_or_fetch_sources(libunibreak libunibreak
        https://github.com/adah1972/libunibreak.git libunibreak_8_0
        src/linebreak.c)
FetchContent_MakeAvailable(libunibreak)
add_library(rbttext_unibreak STATIC
        ${libunibreak_SOURCE_DIR}/src/linebreak.c
        ${libunibreak_SOURCE_DIR}/src/linebreakdata.c
        ${libunibreak_SOURCE_DIR}/src/linebreakdef.c
        ${libunibreak_SOURCE_DIR}/src/eastasianwidthdef.c
        ${libunibreak_SOURCE_DIR}/src/emojidef.c
        ${libunibreak_SOURCE_DIR}/src/unibreakbase.c
        ${libunibreak_SOURCE_DIR}/src/unibreakdef.c)
target_include_directories(rbttext_unibreak PUBLIC ${libunibreak_SOURCE_DIR}/src)
set_target_properties(rbttext_unibreak PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(rabbittus PRIVATE rbttext_unibreak)

# RbtAudio's decoders (RBTAUDIO_TODO.md item 15, card A26): dr_flac (FLAC),
# stb_vorbis (Ogg Vorbis), libopus with opusfile and libogg (Ogg Opus), each
# its own static library linked PRIVATE, with its warnings off: the code is
# theirs. Both single-file decoders write a global on every open - stb_vorbis
# its CRC table, dr_flac its CPU capability flags - which two decodes on two
# threads would race on, and a library here may not have writable globals.
# So each is read as it is and written into the build directory changed
# (the file itself is left untouched, as Hosek and Wilkie's below): stb_vorbis
# takes the table as constants (src/audio/rbtaudio_vorbis_crc.inc), and
# dr_flac, built without SIMD, never calls its capability check.
declare_vendored_or_fetch_sources(dr_libs dr_libs
        https://github.com/mackron/dr_libs.git flac-0.13.3
        dr_flac.h)
FetchContent_MakeAvailable(dr_libs)
file(READ "${dr_libs_SOURCE_DIR}/dr_flac.h" RBTAUDIO_DRFLAC)
string(REPLACE "    drflac__init_cpu_caps();" "    /* rabbittus: no CPU capability flags written. */" RBTAUDIO_DRFLAC "${RBTAUDIO_DRFLAC}")
# Seeking, which only a streamed sound does, guesses byte offsets in floats
# from what the file states and a ratio that can divide by zero, casts them
# to an unsigned 64-bit offset and adds a base: an infinite, NaN, negative or
# too large one is undefined, and a sum that wraps lands below the search's
# range, which dr_flac asserts it never does (both found by rbtaudio_fuzz's
# streamed mode, 2026-10-31). Each of the three is a sum that clamps
# instead, never below its base; the search then clamps it to its range's
# end as before. The offset is only a guess the search checks.
string(REPLACE "static drflac_bool32 drflac__seek_to_pcm_frame__binary_search_internal("
        "static drflac_uint64 drflac__apricot_offset(drflac_uint64 base, float guess) /* rabbittus */\n{\n    drflac_uint64 offset;\n    if (!(guess > 0.0f)) return base;\n    if (guess >= 18446744073709551615.0f) return ~(drflac_uint64)0;\n    offset = (drflac_uint64)guess;\n    return offset > ~(drflac_uint64)0 - base ? ~(drflac_uint64)0 : base + offset;\n}\n\nstatic drflac_bool32 drflac__seek_to_pcm_frame__binary_search_internal("
        RBTAUDIO_DRFLAC "${RBTAUDIO_DRFLAC}")
string(REPLACE "targetByte = byteRangeLo + (drflac_uint64)(((drflac_int64)((pcmFrameIndex - pFlac->currentPCMFrame)"
        "targetByte = drflac__apricot_offset(byteRangeLo, ((drflac_int64)((pcmFrameIndex - pFlac->currentPCMFrame)"
        RBTAUDIO_DRFLAC "${RBTAUDIO_DRFLAC}")
string(REPLACE "targetByte = lastSuccessfulSeekOffset + (drflac_uint64)(((drflac_int64)((pcmFrameIndex-pcmRangeLo)"
        "targetByte = drflac__apricot_offset(lastSuccessfulSeekOffset, ((drflac_int64)((pcmFrameIndex-pcmRangeLo)"
        RBTAUDIO_DRFLAC "${RBTAUDIO_DRFLAC}")
string(REPLACE "byteRangeHi = pFlac->firstFLACFramePosInBytes + (drflac_uint64)((drflac_int64)(pFlac->totalPCMFrameCount"
        "byteRangeHi = drflac__apricot_offset(pFlac->firstFLACFramePosInBytes, (drflac_int64)(pFlac->totalPCMFrameCount"
        RBTAUDIO_DRFLAC "${RBTAUDIO_DRFLAC}")
foreach (_fix "/* rabbittus: no CPU capability flags written. */"
        "static drflac_uint64 drflac__apricot_offset(drflac_uint64 base, float guess) /* rabbittus */"
        "targetByte = drflac__apricot_offset(byteRangeLo, ((drflac_int64)((pcmFrameIndex - pFlac->currentPCMFrame)"
        "targetByte = drflac__apricot_offset(lastSuccessfulSeekOffset, ((drflac_int64)((pcmFrameIndex-pcmRangeLo)"
        "byteRangeHi = drflac__apricot_offset(pFlac->firstFLACFramePosInBytes, (drflac_int64)(pFlac->totalPCMFrameCount")
    string(FIND "${RBTAUDIO_DRFLAC}" "${_fix}" _at)
    if (_at EQUAL -1)
        message(FATAL_ERROR "dr_flac.h changed: rabbittus' fix \"${_fix}\" no longer applies (CMakeLists.txt)")
    endif ()
endforeach ()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/rbtaudio_decoders/dr_flac.h" "${RBTAUDIO_DRFLAC}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/rbtaudio_decoders/rbtaudio_drflac.c"
        "#define DR_FLAC_IMPLEMENTATION\n#define DR_FLAC_NO_SIMD\n#define DR_FLAC_NO_STDIO\n#define DR_FLAC_NO_OGG\n#include \"dr_flac.h\"\n")
add_library(rbtaudio_drflac STATIC "${CMAKE_CURRENT_BINARY_DIR}/rbtaudio_decoders/rbtaudio_drflac.c")
target_include_directories(rbtaudio_drflac PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/rbtaudio_decoders")
target_compile_definitions(rbtaudio_drflac PUBLIC DR_FLAC_NO_SIMD DR_FLAC_NO_STDIO DR_FLAC_NO_OGG)

declare_vendored_or_fetch_sources(stb stb
        https://github.com/nothings/stb.git 003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5
        stb_vorbis.c)
FetchContent_MakeAvailable(stb)
file(READ "${stb_SOURCE_DIR}/stb_vorbis.c" RBTAUDIO_STB_VORBIS)
string(REPLACE "static uint32 crc_table[256];"
        "static const uint32 crc_table[256] = {\n#include \"rbtaudio_vorbis_crc.inc\"\n};" RBTAUDIO_STB_VORBIS "${RBTAUDIO_STB_VORBIS}")
string(REPLACE "      crc_table[i] = s;" "      (void)s; /* rabbittus: the table is constant. */" RBTAUDIO_STB_VORBIS "${RBTAUDIO_STB_VORBIS}")
string(REPLACE "static uint8 ogg_page_header[4]" "static const uint8 ogg_page_header[4]" RBTAUDIO_STB_VORBIS "${RBTAUDIO_STB_VORBIS}")
# A vendor or comment length past 2^31 reads as negative: len + 1 then
# wraps, the setup allocators hand back memory outside their buffer, and
# vendor[len] writes before it (found by rbtaudio_fuzz, 2026-10-30). Each
# length is refused below 0, and the allocators refuse a size below 0 or
# one rounding would wrap.
string(REPLACE "static void *setup_malloc(vorb *f, int sz)\n{\n"
        "static void *setup_malloc(vorb *f, int sz)\n{\n   if (sz < 0 || sz > 0x7FFFFFF0) return NULL; /* rabbittus */\n"
        RBTAUDIO_STB_VORBIS "${RBTAUDIO_STB_VORBIS}")
string(REPLACE "static void *setup_temp_malloc(vorb *f, int sz)\n{\n"
        "static void *setup_temp_malloc(vorb *f, int sz)\n{\n   if (sz < 0 || sz > 0x7FFFFFF0) return NULL; /* rabbittus */\n"
        RBTAUDIO_STB_VORBIS "${RBTAUDIO_STB_VORBIS}")
string(REPLACE "   len = get32_packet(f);\n   f->vendor"
        "   len = get32_packet(f);\n   if (len < 0) return error(f, VORBIS_invalid_setup); /* rabbittus */\n   f->vendor"
        RBTAUDIO_STB_VORBIS "${RBTAUDIO_STB_VORBIS}")
string(REPLACE "      len = get32_packet(f);\n      f->comment_list[i]"
        "      len = get32_packet(f);\n      if (len < 0) return error(f, VORBIS_invalid_setup); /* rabbittus */\n      f->comment_list[i]"
        RBTAUDIO_STB_VORBIS "${RBTAUDIO_STB_VORBIS}")
# A codebook of dimension 0 divides by it, and casts the infinity to int
# (the same day): refused as an invalid setup, as a lookup that can't be
# made is. Its signature table is never written: const.
string(REPLACE "static int lookup1_values(int entries, int dim)\n{\n"
        "static int lookup1_values(int entries, int dim)\n{\n   if (entries <= 0 || dim <= 0) return -1; /* rabbittus */\n"
        RBTAUDIO_STB_VORBIS "${RBTAUDIO_STB_VORBIS}")
string(REPLACE "static uint8 vorbis[6] = " "static const uint8 vorbis[6] = " RBTAUDIO_STB_VORBIS "${RBTAUDIO_STB_VORBIS}")
# And a comment list whose allocation failed is NULL with its length set,
# which tearing down then reads (the same day).
string(REPLACE "   for (i=0; i < p->comment_list_length; ++i) {\n      setup_free(p, p->comment_list[i]);"
        "   for (i=0; p->comment_list && i < p->comment_list_length; ++i) { /* rabbittus */\n      setup_free(p, p->comment_list[i]);"
        RBTAUDIO_STB_VORBIS "${RBTAUDIO_STB_VORBIS}")
# Streamed sounds (RBTAUDIO_TODO.md item 15) read a caller's source as they
# play, and stb_vorbis reads only memory or a FILE *: its FILE * path is kept
# (no STB_VORBIS_NO_STDIO) and FILE made RbtAudio's reader of a source
# (src/audio/rbtaudio_stream_reader.h), its fgetc, fread, fseek and ftell the
# reader's calls, so a stream decodes with stb_vorbis's own pull decoder and
# seeks exactly. Opening a file by name is refused (fopen gives NULL).
string(PREPEND RBTAUDIO_STB_VORBIS
        "/* rabbittus: FILE is a reader of a caller's source (rbtaudio_stream_reader.h). */\n"
        "#include <assert.h>\n#include <limits.h>\n#include <math.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
        "#include \"rbtaudio_stream_reader.h\"\n"
        "#define FILE rbtaudio_stream_reader\n#define fgetc rbtaudio_vorbis_fgetc\n#define fread rbtaudio_vorbis_fread\n"
        "#define fseek rbtaudio_vorbis_fseek\n#define ftell rbtaudio_vorbis_ftell\n#define fclose rbtaudio_vorbis_fclose\n"
        "#define fopen rbtaudio_vorbis_fopen\n#define fopen_s rbtaudio_vorbis_fopen_s\n")
# Seeking, which only a stream does, asserts on what the file's pages say:
# each is an error return instead, as for any other bad file (the same day).
string(REPLACE "   right = f->p_last;\n   assert(right.last_decoded_sample != ~0U);"
        "   right = f->p_last;\n   if (right.last_decoded_sample == ~0U) return error(f, VORBIS_seek_failed); /* rabbittus */"
        RBTAUDIO_STB_VORBIS "${RBTAUDIO_STB_VORBIS}")
string(REPLACE "      assert(left.page_end < right.page_start);"
        "      if (left.page_end >= right.page_start) goto error; /* rabbittus */"
        RBTAUDIO_STB_VORBIS "${RBTAUDIO_STB_VORBIS}")
string(REPLACE "         assert(mid.page_start < right.page_start);"
        "         if (mid.page_start >= right.page_start) goto error; /* rabbittus */"
        RBTAUDIO_STB_VORBIS "${RBTAUDIO_STB_VORBIS}")
string(REPLACE "   end_pos = f->end_seg_with_known_loc;\n   assert(end_pos >= 0);"
        "   end_pos = f->end_seg_with_known_loc;\n   if (end_pos < 0) return error(f, VORBIS_seek_failed); /* rabbittus */"
        RBTAUDIO_STB_VORBIS "${RBTAUDIO_STB_VORBIS}")
string(REPLACE "   assert(f->current_loc_valid);\n   assert(f->current_loc <= sample_number);"
        "   if (!f->current_loc_valid || f->current_loc > sample_number) return error(f, VORBIS_seek_failed); /* rabbittus */"
        RBTAUDIO_STB_VORBIS "${RBTAUDIO_STB_VORBIS}")
string(REPLACE "      assert(sample_number > frame_start);\n      assert(f->channel_buffer_start + (int) (sample_number-frame_start) <= f->channel_buffer_end);"
        "      if (sample_number <= frame_start || f->channel_buffer_start + (int) (sample_number-frame_start) > f->channel_buffer_end) return error(f, VORBIS_seek_failed); /* rabbittus */"
        RBTAUDIO_STB_VORBIS "${RBTAUDIO_STB_VORBIS}")
foreach (_fix "if (sz < 0 || sz > 0x7FFFFFF0) return NULL; /* rabbittus */" "if (len < 0) return error(f, VORBIS_invalid_setup); /* rabbittus */"
        "p->comment_list && i < p->comment_list_length; ++i) { /* rabbittus */"
        "if (entries <= 0 || dim <= 0) return -1; /* rabbittus */" "static const uint8 vorbis[6] = "
        "if (right.last_decoded_sample == ~0U) return error(f, VORBIS_seek_failed); /* rabbittus */"
        "if (left.page_end >= right.page_start) goto error; /* rabbittus */"
        "if (mid.page_start >= right.page_start) goto error; /* rabbittus */"
        "if (end_pos < 0) return error(f, VORBIS_seek_failed); /* rabbittus */"
        "if (!f->current_loc_valid || f->current_loc > sample_number) return error(f, VORBIS_seek_failed); /* rabbittus */"
        "if (sample_number <= frame_start || f->channel_buffer_start + (int) (sample_number-frame_start) > f->channel_buffer_end) return error(f, VORBIS_seek_failed); /* rabbittus */"
        "   int c = fgetc(z->f);" "   if (fread(data, n, 1, z->f) == 1)" "      fseek(z->f, x+n, SEEK_SET);"
        "   if (!fseek(f->f, loc, SEEK_SET))" "   return (unsigned int) (ftell(f->f) - f->f_start);")
    string(FIND "${RBTAUDIO_STB_VORBIS}" "${_fix}" _at)
    if (_at EQUAL -1)
        message(FATAL_ERROR "stb_vorbis.c changed: rabbittus' fix \"${_fix}\" no longer applies (CMakeLists.txt)")
    endif ()
endforeach ()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/rbtaudio_decoders/stb_vorbis.c" "${RBTAUDIO_STB_VORBIS}")
# The reader is rabbittus' own code, built at its warning level as a
# library of its own, which stb_vorbis's FILE * path links.
add_library(rbtaudio_stream_reader STATIC src/audio/rbtaudio_stream_reader.c)
set_target_properties(rbtaudio_stream_reader PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(rbtaudio_stb_vorbis STATIC "${CMAKE_CURRENT_BINARY_DIR}/rbtaudio_decoders/stb_vorbis.c")
target_include_directories(rbtaudio_stb_vorbis PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/audio")
target_compile_definitions(rbtaudio_stb_vorbis PUBLIC STB_VORBIS_NO_PUSHDATA_API)
target_link_libraries(rbtaudio_stb_vorbis PUBLIC rbtaudio_stream_reader)

# libogg 1.3.6 (BSD): Ogg pages and packets, from its two sources; its
# config_types.h, which its own build generates, is written here from
# <stdint.h>.
declare_vendored_or_fetch_sources(ogg ogg
        https://github.com/xiph/ogg.git v1.3.6
        src/framing.c)
FetchContent_MakeAvailable(ogg)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/rbtaudio_decoders/ogg/config_types.h"
        "#ifndef __CONFIG_TYPES_H__\n#define __CONFIG_TYPES_H__\n#include <stdint.h>\ntypedef int16_t ogg_int16_t;\ntypedef uint16_t ogg_uint16_t;\ntypedef int32_t ogg_int32_t;\ntypedef uint32_t ogg_uint32_t;\ntypedef int64_t ogg_int64_t;\ntypedef uint64_t ogg_uint64_t;\n#endif\n")
add_library(rbtaudio_ogg STATIC ${ogg_SOURCE_DIR}/src/bitwise.c ${ogg_SOURCE_DIR}/src/framing.c)
target_include_directories(rbtaudio_ogg PUBLIC ${ogg_SOURCE_DIR}/include "${CMAKE_CURRENT_BINARY_DIR}/rbtaudio_decoders")

# libopus 1.5.2 (BSD), through its own CMakeLists.txt: float, static, no
# programs, tests or install modules.
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
declare_vendored_or_fetch(opus opus https://github.com/xiph/opus.git v1.5.2)
FetchContent_MakeAvailable(opus)

# opusfile 0.12 (BSD): Ogg Opus files over libogg and libopus, from its
# sources, without its HTTP streaming.
declare_vendored_or_fetch_sources(opusfile opusfile
        https://github.com/xiph/opusfile.git v0.12
        src/opusfile.c)
FetchContent_MakeAvailable(opusfile)
add_library(rbtaudio_opusfile STATIC ${opusfile_SOURCE_DIR}/src/info.c ${opusfile_SOURCE_DIR}/src/internal.c
        ${opusfile_SOURCE_DIR}/src/opusfile.c ${opusfile_SOURCE_DIR}/src/stream.c)
target_include_directories(rbtaudio_opusfile PUBLIC ${opusfile_SOURCE_DIR}/include)
target_compile_definitions(rbtaudio_opusfile PRIVATE OP_DISABLE_HTTP OP_DISABLE_DOCS)
target_link_libraries(rbtaudio_opusfile PUBLIC rbtaudio_ogg opus)

foreach (target rbtaudio_drflac rbtaudio_stb_vorbis rbtaudio_ogg rbtaudio_opusfile opus)
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if (MSVC)
        target_compile_options(${target} PRIVATE /W0)
    else ()
        target_compile_options(${target} PRIVATE -w)
    endif ()
endforeach ()
target_link_libraries(rabbittus PRIVATE rbtaudio_drflac rbtaudio_stb_vorbis rbtaudio_opusfile)
# stb_vorbis shifts signed values its reads return (-1 at a packet's end, a
# byte promoted to int) past their sign bit, wraps signed sums, and casts
# out-of-range floats to int, all through: undefined in C, defined by every compiler and CPU
# here, and harmless where it doesn't lead to memory (the cases that did
# are fixed above). Under the sanitizer presets those three UBSan checks are
# off for it alone; ASan still watches its memory (rbtaudio_fuzz,
# 2026-10-30).
if (NOT MSVC)
    target_compile_options(rbtaudio_stb_vorbis PRIVATE -fno-sanitize=shift,float-cast-overflow,signed-integer-overflow)
endif ()

# Hosek and Wilkie's sky model, version 1.4a (RBTREND_COMMAND_LIT_SKY's
# HOSEK_WILKIE): its RGB coefficients alone, which rbtrendlit.cpp cooks for a
# sun and an air. 3-clause BSD licence, in the data file itself and in its
# README.txt. The authors' release is a zip, so it is pinned by its hash.
# The data file declares its tables as writable globals, which a library
# here may not have, so it is read here as it is and written into the build
# directory with every table static and const (rbtrend_hosek_rgb.inc); the
# file itself is left untouched.
set(RABBITTUS_HOSEK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.deps/HosekWilkie-1.4a")
if (EXISTS "${RABBITTUS_HOSEK_DIR}/ArHosekSkyModelData_RGB.h")
    message(STATUS "hosek_wilkie: using vendored ${RABBITTUS_HOSEK_DIR}")
else ()
    message(STATUS "hosek_wilkie: no vendored copy under .deps/, fetching the 1.4a release")
    FetchContent_Declare(hosek_wilkie
            URL https://cgg.mff.cuni.cz/projects/SkylightModelling/HosekWilkie_SkylightModel_C_Source.1.4a.zip
            URL_HASH SHA256=743e81a7fcbed06408490a303dcf1315083d7988a11fc69608e66f6e5417f9de
            SOURCE_SUBDIR sources-only-not-a-directory)
    FetchContent_MakeAvailable(hosek_wilkie)
    # The zip holds the release's folder beside a __MACOSX one.
    set(RABBITTUS_HOSEK_DIR "${hosek_wilkie_SOURCE_DIR}/HosekWilkie_SkylightModel_C_Source.1.4a")
endif ()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${RABBITTUS_HOSEK_DIR}/ArHosekSkyModelData_RGB.h")
file(READ "${RABBITTUS_HOSEK_DIR}/ArHosekSkyModelData_RGB.h" _hosek_rgb)
string(REPLACE "datasetsRGB" "rbtrend_hosek_datasetsRGB" _hosek_rgb "${_hosek_rgb}")
string(REPLACE "datasetRGB" "rbtrend_hosek_datasetRGB" _hosek_rgb "${_hosek_rgb}")
string(REPLACE "double* rbtrend_hosek_" "static const double *const rbtrend_hosek_" _hosek_rgb "${_hosek_rgb}")
string(REPLACE "double rbtrend_hosek_" "static const double rbtrend_hosek_" _hosek_rgb "${_hosek_rgb}")
file(MAKE_DIRECTORY ${RABBITTUS_SHADER_INC_DIR})
file(CONFIGURE OUTPUT ${RABBITTUS_SHADER_INC_DIR}/rbtrend_hosek_rgb.inc CONTENT "${_hosek_rgb}" @ONLY)
# For a caller's tests that work the model out in C to compare with.
set(RABBITTUS_HOSEK_RGB_INC ${RABBITTUS_SHADER_INC_DIR}/rbtrend_hosek_rgb.inc CACHE INTERNAL "")

# No floating-point contraction where RbtMath's polygons compute in doubles:
# results must be the same bits on every platform. MSVC doesn't contract
# unless asked (/fp:contract), so only Clang and GCC need telling.
if (NOT MSVC)
    target_compile_options(rbtmath_clipper2 PRIVATE -ffp-contract=off)
    set_source_files_properties(src/math/rbtmathpoly.cpp PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
    # The same for RbtMath's BVHs: a cast, an overlap and a sweep give the
    # same bits everywhere (rbtmathbvh.h, "Same bits").
    set_source_files_properties(src/math/rbtmathbvh.cpp PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
    # The same for RbtText: a layout's positions and a glyph's distance field
    # are the same bits on every platform (rbttext.h).
    foreach (target rbttext_harfbuzz rbttext_msdfgen rbttext_sheenbidi rbttext_unibreak)
        target_compile_options(${target} PRIVATE -ffp-contract=off -w)
    endforeach ()
    set_source_files_properties(src/text/rbttext.cpp src/text/rbttext_layout.cpp src/text/rbttext_atlas.cpp
            PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
    # The same for Rend's lit code on the CPU (card A49): a cascade fit's
    # arithmetic is the same on every compiler, leaving only libm's rounding
    # of std::pow and std::tan, which rbtrend_contract_tests allows for.
    set_source_files_properties(src/render/rbtrendlit.cpp PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
else ()
    # RbtText's dependencies are third-party code: their warnings are theirs.
    # HarfBuzz's one-file amalgamation has more sections than an object file
    # holds by default. /W0, not /w: Visual Studio's generator makes /W0 the
    # project's warning level, where /w would sit beside its default /W1.
    foreach (target rbttext_harfbuzz rbttext_msdfgen rbttext_sheenbidi rbttext_unibreak)
        target_compile_options(${target} PRIVATE /W0)
    endforeach ()
    target_compile_options(rbttext_harfbuzz PRIVATE /bigobj)
endif ()

# Headless tests - no GPU, no window. Run with ctest, or the executable.
option(RABBITTUS_BUILD_TESTS "Build rabbittus' headless tests" ON)
if (RABBITTUS_BUILD_TESTS)
    enable_testing()
    add_executable(rbtcore_tests tests/rbtcore_tests.c)
    target_link_libraries(rbtcore_tests PRIVATE rabbittus)
    target_compile_definitions(rbtcore_tests PRIVATE
        RBTCORE_TEST_CMAKE_VERSION="${PROJECT_VERSION}")
    add_test(NAME rbtcore_tests COMMAND rbtcore_tests)
    add_executable(rbtarchive_tests tests/rbtarchive_tests.cpp)
    target_link_libraries(rbtarchive_tests PRIVATE rabbittus)
    add_test(NAME rbtarchive_tests COMMAND rbtarchive_tests)
    # Every public function with every argument zeroed (ROADMAP.md, section
    # 3.1): generated from abi.txt by tests/misuse/make_misuse_tests.py.
    # No allocation per frame (ROADMAP.md, section 3.1): each module's steady
    # state under the platform's allocation hook.
    add_executable(rbtnoalloc_tests tests/rbtnoalloc_tests.c)
    target_link_libraries(rbtnoalloc_tests PRIVATE rabbittus)
    # glibc declares dladdr and Dl_info (tests/alloctrace.h) only under
    # _GNU_SOURCE, which must come before the first system header; and dladdr
    # names only an executable's exported symbols, so it exports them all.
    if (UNIX AND NOT APPLE)
        target_compile_definitions(rbtnoalloc_tests PRIVATE _GNU_SOURCE)
        set_target_properties(rbtnoalloc_tests PROPERTIES ENABLE_EXPORTS ON)
    endif ()
    add_test(NAME rbtnoalloc_tests COMMAND rbtnoalloc_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/rbttext
             ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/rbtaudio)

    add_executable(rbt_misuse_tests tests/rbt_misuse_tests.c)
    target_link_libraries(rbt_misuse_tests PRIVATE rabbittus)
    add_test(NAME rbt_misuse_tests COMMAND rbt_misuse_tests)

    add_executable(rbtaudio_tests tests/rbtaudio_tests.c)
    target_link_libraries(rbtaudio_tests PRIVATE rabbittus)
    add_test(NAME rbtaudio_tests COMMAND rbtaudio_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/rbtaudio)
    # Two sessions talking over the loopback interface: no GPU, no window,
    # but it does open a listening socket on 127.0.0.1.
    add_executable(rbtsync_tests tests/rbtsync_tests.c)
    target_link_libraries(rbtsync_tests PRIVATE rabbittus)
    add_test(NAME rbtsync_tests COMMAND rbtsync_tests)
    # RbtText's contract (RBTTEXT_TODO.md): written before the implementation,
    # a behavioural test reports pending while its call isn't built.
    add_executable(rbttext_tests tests/rbttext_tests.c)
    target_link_libraries(rbttext_tests PRIVATE rabbittus)
    add_test(NAME rbttext_tests COMMAND rbttext_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/rbttext)
    # The same layout and atlas bits on every platform, against values
    # recorded on macOS.
    add_executable(rbttext_bits_tests tests/rbttext_bits_tests.c)
    target_link_libraries(rbttext_bits_tests PRIVATE rabbittus)
    add_test(NAME rbttext_bits_tests COMMAND rbttext_bits_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/rbttext)
    # RbtJobs' contract (card L12): the scheduler check, serial runs and the
    # reference pool, which starts threads; run under -tsan too.
    # The Debug builds' overlap check every module opens its calls with
    # (src/rbtoverlap.hpp), driven with threads; skips itself in Release.
    add_executable(rbtoverlap_tests tests/rbtoverlap_tests.cpp)
    # Rbtrend's ray tracing calls too, on objects of its internal headers.
    target_link_libraries(rbtoverlap_tests PRIVATE rabbittus eqstd)
    add_test(NAME rbtoverlap_tests COMMAND rbtoverlap_tests)
    add_executable(rbtjobs_tests tests/rbtjobs_tests.c)
    target_link_libraries(rbtjobs_tests PRIVATE rabbittus)
    add_test(NAME rbtjobs_tests COMMAND rbtjobs_tests)
    # RbtJobs' groups (rbtjobs_groups_draft.h): trees, chains and graphs of
    # tasks that enqueue tasks, on the pool at 0-7 threads and on the
    # reference scheduler over an executor with no worker indices. C++ for
    # std::thread and std::atomic; it calls the C API only. Run under -tsan
    # too; the timeout catches a wait that never ends.
    add_executable(rbtjobs_groups_tests tests/rbtjobs_groups_tests.cpp)
    target_link_libraries(rbtjobs_groups_tests PRIVATE rabbittus Threads::Threads)
    add_test(NAME rbtjobs_groups_tests COMMAND rbtjobs_groups_tests)
    set_tests_properties(rbtjobs_groups_tests PROPERTIES TIMEOUT 120)
    add_executable(rbtmathbvh_tests tests/rbtmathbvh_tests.c)
    target_link_libraries(rbtmathbvh_tests PRIVATE rabbittus)
    add_test(NAME rbtmathbvh_tests COMMAND rbtmathbvh_tests)
    # Numbers, not a test: casts, sweeps and overlaps a second, and refit
    # against rebuild against create (RBTMATH_TODO.md). Run by hand, Release.
    # Numbers, not a test: an update and a render with 32, 256 and 1,024
    # moving voices, by angles and in the world (RBTAUDIO_TODO.md item 16).
    # Run by hand, Release.
    add_executable(rbtaudio_bench tests/rbtaudio_bench.c)
    target_link_libraries(rbtaudio_bench PRIVATE rabbittus)
    add_executable(rbtmathbvh_bench tests/rbtmathbvh_bench.c)
    target_link_libraries(rbtmathbvh_bench PRIVATE rabbittus)
    add_executable(rbtmathpoly_tests tests/rbtmathpoly_tests.c)
    target_link_libraries(rbtmathpoly_tests PRIVATE rabbittus)
    add_test(NAME rbtmathpoly_tests COMMAND rbtmathpoly_tests)
    # The same bits on every platform: a fixed run of every polygon call,
    # hashed, against values recorded on macOS (tests/data/rbtmathpoly).
    add_executable(rbtmathpoly_bits_tests tests/rbtmathpoly_bits_tests.c)
    target_link_libraries(rbtmathpoly_bits_tests PRIVATE rabbittus)
    add_test(NAME rbtmathpoly_bits_tests COMMAND rbtmathpoly_bits_tests)
    # The same for the BVHs' casts, overlaps and sweeps, against values
    # recorded on the first machine to run it (tests/data/rbtmathbvh).
    add_executable(rbtmathbvh_bits_tests tests/rbtmathbvh_bits_tests.c)
    target_link_libraries(rbtmathbvh_bits_tests PRIVATE rabbittus)
    # Its inputs are worked out in the test, so with no contraction either:
    # clang fuses a multiply and an add on arm64 by default, which made the
    # inputs, not the BVH, differ from MSVC's (found on macOS 2026-11-09).
    if (NOT MSVC)
        target_compile_options(rbtmathbvh_bits_tests PRIVATE -ffp-contract=off)
    endif ()
    add_test(NAME rbtmathbvh_bits_tests COMMAND rbtmathbvh_bits_tests)
    # The range list under Rbtrend's pools: an internal header with no GPU in
    # it, which is why this one test sees src/render.
    add_executable(rbtrendranges_tests tests/rbtrendranges_tests.cpp)
    target_include_directories(rbtrendranges_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/render)
    add_test(NAME rbtrendranges_tests COMMAND rbtrendranges_tests)
    # The open-addressing table under Rbtrend's command lists, an internal
    # header too.
    add_executable(rbthashindex_tests tests/rbthashindex_tests.cpp)
    target_include_directories(rbthashindex_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME rbthashindex_tests COMMAND rbthashindex_tests)
    # SPIR-V reflection, and writing a member where the reflection lays it
    # out: no device either, so it sees src/render too.
    # The reflection is made at build time (tools/rbtrend_reflect): the test
    # builds the tool's reflection and table writer in, over SPIRV-Cross's
    # core, which is never in the library (card X18), and reads each table
    # back through the library's reader.
    include(FetchContent)
    set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_GLSL OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_MSL OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_UTIL OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
    declare_vendored_or_fetch(spirv_cross SPIRV-Cross
            https://github.com/KhronosGroup/SPIRV-Cross.git vulkan-sdk-1.4.363.0)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
    FetchContent_MakeAvailable(spirv_cross)
    add_executable(rbtrendreflect_tests tests/rbtrendreflect_tests.cpp
            tools/rbtrend_reflect/rbtrend_reflect_spirv.cpp tools/rbtrend_reflect/rbtrend_reflect_inputs.cpp
            tools/rbtrend_reflect/rbtrend_reflect_write.cpp)
    target_include_directories(rbtrendreflect_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/render
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/rbtrend_reflect ${RABBITTUS_SHADER_INC_DIR})
    target_link_libraries(rbtrendreflect_tests PRIVATE rabbittus spirv-cross-core)
    add_test(NAME rbtrendreflect_tests COMMAND rbtrendreflect_tests)
    # The shadow vertex shader's reflection JSON, embedded: a shader whose
    # SPIR-V drops inputs it declares, for the tests of card A51's half.
    get_filename_component(_rbtrend_embed ${CR_SHADERS_CMAKE} DIRECTORY)
    set(_rbtrend_shadow_json ${RABBITTUS_SHADER_INC_DIR}/rbtrend_lit_shadow_vert.reflection.json)
    add_custom_command(
        OUTPUT ${_rbtrend_shadow_json}.inc
        COMMAND ${CMAKE_COMMAND} -DIN=${_rbtrend_shadow_json} -DOUT=${_rbtrend_shadow_json}.inc
                -DNAME=rbtrend_lit_shadow_vert_reflection_json -P ${_rbtrend_embed}/cr_embed.cmake
        DEPENDS ${_rbtrend_shadow_json} ${_rbtrend_embed}/cr_embed.cmake
        COMMENT "Rbtrend: rbtrend_lit_shadow_vert's reflection JSON, for rbtrendreflect_tests")
    target_sources(rbtrendreflect_tests PRIVATE ${_rbtrend_shadow_json}.inc)
    # Whether this build's packages carry DXIL, whose vertex reflections list
    # every declared input (cmake/rbtrend_slang.cmake): what the tables the
    # build wrote are checked against.
    if(GRAPHICS_BACKEND STREQUAL "D3D12")
        target_compile_definitions(rbtrendreflect_tests PRIVATE RBTREND_TEST_PACKAGES_DXIL=1)
    else()
        target_compile_definitions(rbtrendreflect_tests PRIVATE RBTREND_TEST_PACKAGES_DXIL=0)
    endif()
    # The features Rbtrend's own code uses, from C.
    add_executable(rbtrendfeatures_tests tests/rbtrendfeatures_tests.c)
    target_link_libraries(rbtrendfeatures_tests PRIVATE rabbittus)
    add_test(NAME rbtrendfeatures_tests COMMAND rbtrendfeatures_tests)
    # Rbtrend's contract: what its converted calls refuse before a device is
    # touched, from C.
    add_executable(rbtrend_contract_tests tests/rbtrend_contract_tests.c)
    target_link_libraries(rbtrend_contract_tests PRIVATE rabbittus)
    add_test(NAME rbtrend_contract_tests COMMAND rbtrend_contract_tests)
endif ()

# Fuzz targets (tests/fuzz/rbtfuzz.hpp). Each is built two ways: as a
# ctest that replays its seed corpus and every input the fuzzer once
# crashed on, with whatever compiler builds the tests; and, under
# RABBITTUS_BUILD_FUZZERS, as a libFuzzer executable, which needs Clang
# with -fsanitize=fuzzer (the rabbittus-*-fuzz presets: Homebrew's LLVM
# on macOS, since Apple's Clang has no libFuzzer). The presets build every
# target with -fsanitize=fuzzer-no-link,address,undefined; only the fuzz
# executables link libFuzzer itself.
set(RABBITTUS_FUZZ_TARGETS rbtarchive_fuzz rbttext_fuzz rbtaudio_fuzz rbtrend_reflection_fuzz)
if (RABBITTUS_BUILD_TESTS)
    foreach (target IN LISTS RABBITTUS_FUZZ_TARGETS)
        add_executable(${target}_replay tests/fuzz/rbtfuzz_replay.cpp tests/fuzz/${target}.cpp)
        target_link_libraries(${target}_replay PRIVATE rabbittus)
        # rbtrend_reflection_fuzz reads Rbtrend's reflection table through its internal reader.
        target_include_directories(${target}_replay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/render)
        add_test(NAME ${target}_regressions COMMAND ${target}_replay
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/${target}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/regressions/${target})
    endforeach ()
endif ()
option(RABBITTUS_BUILD_FUZZERS "Build rabbittus' libFuzzer targets (Clang only)" OFF)
if (RABBITTUS_BUILD_FUZZERS)
    foreach (target IN LISTS RABBITTUS_FUZZ_TARGETS)
        add_executable(${target} tests/fuzz/${target}.cpp)
        target_link_libraries(${target} PRIVATE rabbittus)
        target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/render)
        target_link_options(${target} PRIVATE -fsanitize=fuzzer)
    endforeach ()
endif ()
