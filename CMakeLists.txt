cmake_minimum_required(VERSION 3.30)

# The version is written once, in include/apricore.h; read it from there.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/include/apricore.h" _apri_version_lines
     REGEX "^#define APRICOTFIELDS_VERSION_(MAJOR|MINOR|PATCH) [0-9]+$")
foreach(_part MAJOR MINOR PATCH)
    string(REGEX REPLACE ".*APRICOTFIELDS_VERSION_${_part} ([0-9]+).*" "\\1"
           APRICOTFIELDS_VERSION_${_part} "${_apri_version_lines}")
    if(NOT APRICOTFIELDS_VERSION_${_part} MATCHES "^[0-9]+$")
        message(FATAL_ERROR "ApricotFields: no APRICOTFIELDS_VERSION_${_part} line in include/apricore.h")
    endif()
endforeach()
project(ApricotFields VERSION ${APRICOTFIELDS_VERSION_MAJOR}.${APRICOTFIELDS_VERSION_MINOR}.${APRICOTFIELDS_VERSION_PATCH})

# CMake knows no C++26 flag for MSVC; its C++23 maps to /std:c++latest,
# the newest dialect MSVC offers.
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(CMAKE_CXX_STANDARD 23)
else()
    set(CMAKE_CXX_STANDARD 26)
endif()

add_library(ApricotFields STATIC
        src/apricotfields.cpp
        src/apricore.cpp
        src/aprihashindex.hpp
        src/apriplaindata.hpp
        src/apristructbase.hpp
        src/render/aprend_internal.hpp
        src/render/aprendcontext.cpp
        src/render/aprendbuffers.cpp
        src/render/aprendranges_internal.hpp
        src/render/aprendpools_internal.hpp
        src/render/aprendpools.cpp
        src/render/aprendimages_internal.hpp
        src/render/aprendimages.cpp
        src/render/aprendframes.cpp
        src/render/aprendcommands.cpp
        src/render/aprenddrawlist.cpp
        src/render/aprendmaterial.cpp
        src/render/aprendpass.cpp
        src/render/aprendreflect_internal.hpp
        src/render/aprendreflect.cpp
        src/render/aprendpipeline.cpp
        src/render/aprendswapchain.cpp
        src/render/aprendtransient_internal.hpp
        src/render/aprendtransient.cpp
        src/render/aprendtimer.cpp
        src/render/aprendbuiltin_internal.hpp
        src/render/aprendbuiltin.cpp
        src/render/aprendlit_internal.hpp
        src/render/aprendlit.cpp
        src/render/aprendrendertargets_internal.hpp
        src/render/aprendrendertargets.cpp
        src/archive/aparchive_internal.hpp
        src/archive/aparchive.cpp
        src/audio/apaudio.cpp
        src/audio/apaudiocues.cpp
        src/math/apmathbvh.cpp
        src/math/apmathpoly_internal.hpp
        src/math/apmathpoly.cpp
        src/sync/apsync_internal.hpp
        src/sync/apsyncnet.cpp
        src/text/aptext_internal.hpp
        src/text/aptext.cpp
        src/text/aptext_layout.cpp
        src/text/aptext_atlas.cpp
        include/apricore.h
        include/render/aprendcontext.h
        include/render/aprendbuffers.h
        include/render/aprendimages.h
        include/render/aprendframes.h
        include/render/aprendcommands.h
        include/render/aprenddrawlist.h
        include/render/aprendmaterial.h
        include/render/aprendpass.h
        include/render/aprendshader.h
        include/render/aprendpipeline.h
        include/render/aprendswapchain.h
        include/render/aprendtransient.h
        include/render/aprendtimer.h
        include/archive/aparchive.h
        include/audio/apaudio.h
        include/audio/apaudiocues.h
        include/math/apmathbvh.h
        include/math/apmathpoly.h
        include/sync/apsyncnet.h
        include/text/aptext.h
        )

target_include_directories(ApricotFields PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)


# Add the directory for SpudLib library
if (NOT TARGET spudlib)
    add_subdirectory(../spudlib ${CMAKE_CURRENT_BINARY_DIR}/spudlib)
endif ()
# Link SpudLib library
target_link_libraries(ApricotFields PUBLIC spudlib)

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
    FetchContent_Declare(${name} GIT_REPOSITORY "${repo}" GIT_TAG "${tag}" GIT_SHALLOW TRUE
            SOURCE_SUBDIR sources-only-not-a-directory)
endfunction()

# SPIRV-Cross - SPIR-V reflection for Aprend's shaders
# (src/render/aprendreflect.cpp): only its core, which reads SPIR-V and
# writes nothing. SpudLib already builds it on the backends that
# cross-compile SPIR-V (Metal from eqdev/.deps/SPIRV-Cross, D3D12 from its
# Vulkan SDK tag); where it hasn't (Vulkan), it is built here from the same
# vendored checkout as Metal's. Its core API is the same in both.
if (NOT TARGET spirv-cross-core)
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
endif ()
# ApricotFields may be linked into a shared library too.
set_target_properties(spirv-cross-core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(ApricotFields PRIVATE spirv-cross-core)

# ApSync runs each session on a thread of its own (src/sync/apsyncnet.cpp).
find_package(Threads REQUIRED)
target_link_libraries(ApricotFields PRIVATE Threads::Threads)

# Aprend's own shaders (shaders/*.vert, *.frag, *.comp), compiled to SPIR-V at build
# time and embedded in the library as C arrays (glslc -mfmt=c): there is
# nothing to deploy or find at run time.
# src/render/aprendbuiltin.cpp includes them. glslc is needed on every
# platform, Apple included, since SpudGPU's Metal and D3D12 backends take
# SPIR-V and cross-compile it.
find_program(APRICOTFIELDS_GLSLC glslc
        HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
        REQUIRED)
set(APRICOTFIELDS_SHADER_INC_DIR ${CMAKE_CURRENT_BINARY_DIR}/aprend_shaders)
set(APRICOTFIELDS_SHADERS
        aprend_fullscreen.vert
        aprend_downsample.frag
        aprend_present.frag
        aprend_bloom_prefilter.frag
        aprend_bloom_downsample.frag
        aprend_bloom_upsample.frag
        aprend_bloom_composite.frag
        aprend_tone_map.frag
        aprend_blur.frag
        aprend_outline.frag
        aprend_lit.vert
        aprend_lit.frag
        aprend_lit_shadow.vert
        aprend_lit_depth.frag
        aprend_environment_from_cube.frag
        aprend_environment_from_equirect.frag
        aprend_environment_prefilter.frag
        aprend_environment_irradiance.frag
        aprend_environment_sky.frag
        aprend_environment_hosek.frag
        aprend_lit_depth_cutout.frag
        aprend_lit_sky.frag
        aprend_lit_cull.comp
        aprend_lit_cull_bounded.comp
        aprend_lit_brdf_lut.frag
        aprend_lit_occlusion.frag
        aprend_lit_occlusion_blur.frag
        aprend_exposure_luminance.frag
        aprend_exposure_adapt.frag
        aprend_exposure_histogram.comp
        aprend_exposure_adapt_histogram.frag
)
# What the shaders include (#include, GL_GOOGLE_include_directive), from
# shaders/: every shader is compiled again when one of them changes.
set(APRICOTFIELDS_SHADER_INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/aprend_lit_scene.glslh
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/aprend_lit_cull.glslh
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/aprend_lit_vertex.glslh
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/aprend_environment.glslh
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/aprend_lit_material.glslh
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/aprend_lit_bindings.glslh
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/aprend_lit_shadow_pass.glslh
)
foreach (shader IN LISTS APRICOTFIELDS_SHADERS)
    set(shader_inc ${APRICOTFIELDS_SHADER_INC_DIR}/${shader}.spv.inc)
    add_custom_command(
            OUTPUT ${shader_inc}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${APRICOTFIELDS_SHADER_INC_DIR}
            COMMAND ${APRICOTFIELDS_GLSLC} -mfmt=c -I ${CMAKE_CURRENT_SOURCE_DIR}/shaders ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader} -o ${shader_inc}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader} ${APRICOTFIELDS_SHADER_INCLUDES}
            COMMENT "Compiling ${shader} to embedded SPIR-V"
    )
    list(APPEND APRICOTFIELDS_SHADER_INCS ${shader_inc})
endforeach ()
add_custom_target(apricotfields_shaders DEPENDS ${APRICOTFIELDS_SHADER_INCS})
add_dependencies(ApricotFields apricotfields_shaders)
target_include_directories(ApricotFields PRIVATE ${APRICOTFIELDS_SHADER_INC_DIR})

# GLM — header-only math library used internally by the C++ backends
declare_vendored_or_fetch(glm glm https://github.com/g-truc/glm.git 1.0.1)
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
FetchContent_MakeAvailable(glm)
target_link_libraries(ApricotFields PUBLIC glm::glm)

# libdeflate - DEFLATE and CRC-32 for ApArchive. Vendored under
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
# ApricotFields may be linked into a shared library too.
set_target_properties(libdeflate_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(ApricotFields PRIVATE libdeflate::libdeflate_static)

# glaze - JSON, for ApAudio's sound bank manifests (src/audio/apaudiocues.cpp).
# Header only, vendored under eqdev/.deps/glaze like libdeflate. Private: no
# glaze type crosses ApricotFields' C API.
# Pinned to the release v9.0.0, by its commit (a tag can be moved upstream, a
# commit can't). eqdev/.deps/glaze is checked out at the same tag, so a fetched
# build and a vendored one use the same glaze. Move both together.
declare_vendored_or_fetch(glaze glaze https://github.com/stephenberry/glaze.git d78832c82289c61a9315bfbc35332cec9f4e93ca) # v9.0.0
FetchContent_MakeAvailable(glaze)
target_link_libraries(ApricotFields PRIVATE glaze::glaze)

# Clipper2 - polygon booleans for ApMath's polygons (src/math/apmathpoly.cpp).
# Boost licence. Only its library sources are used, built here: its own
# CMakeLists.txt treats warnings as errors and builds tests and examples.
# Pinned on purpose, like libdeflate: where its intersections land on the grid
# is part of what makes a boolean's result the same on every build. Built with
# floating-point contraction off for the same reason - a fused multiply-add
# rounds differently from the two operations it replaces (see the note at the
# top of apmathpoly.cpp).
declare_vendored_or_fetch_sources(clipper2 Clipper2
        https://github.com/AngusJohnson/Clipper2.git Clipper2_1.5.4
        CPP/Clipper2Lib/src/clipper.engine.cpp)
FetchContent_MakeAvailable(clipper2)
add_library(apmath_clipper2 STATIC
        ${clipper2_SOURCE_DIR}/CPP/Clipper2Lib/src/clipper.engine.cpp
        ${clipper2_SOURCE_DIR}/CPP/Clipper2Lib/src/clipper.offset.cpp
        ${clipper2_SOURCE_DIR}/CPP/Clipper2Lib/src/clipper.rectclip.cpp)
target_include_directories(apmath_clipper2 PUBLIC ${clipper2_SOURCE_DIR}/CPP/Clipper2Lib/include)
set_target_properties(apmath_clipper2 PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(ApricotFields PRIVATE apmath_clipper2)

# earcut.hpp - triangulation for ApMath's polygons. ISC licence, header only;
# its CMakeLists.txt builds its tests, so only the header is used. Pinned:
# which triangles it makes is part of a triangulation's result.
declare_vendored_or_fetch_sources(earcut earcut.hpp
        https://github.com/mapbox/earcut.hpp.git v2.2.4
        include/mapbox/earcut.hpp)
FetchContent_MakeAvailable(earcut)
target_include_directories(ApricotFields PRIVATE ${earcut_SOURCE_DIR}/include)

# eqstd - the containers ApText's sources grow their arrays with (eq::array,
# eq::vector; CLAUDE.md, "Containers: eqstd"). Header-only, PRIVATE: no
# public header includes it. SpudLib, built before this, has usually added
# the target already from the same workspace checkout.
if (NOT TARGET eqstd)
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../eqstd/CMakeLists.txt")
        message(STATUS "eqstd: using the workspace's ${CMAKE_CURRENT_SOURCE_DIR}/../eqstd")
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../eqstd" "${CMAKE_CURRENT_BINARY_DIR}/eqstd")
    else ()
        message(STATUS "eqstd: no ../eqstd beside ApricotFields, fetching endqueue/eqstd")
        FetchContent_Declare(eqstd GIT_REPOSITORY https://github.com/endqueue/eqstd.git
                GIT_TAG b302f96b6aa644ab59a8b9cd23d88aaa4aa5b2dd) # eqstd 0.1.0
        FetchContent_MakeAvailable(eqstd)
    endif ()
endif ()
target_link_libraries(ApricotFields PRIVATE eqstd)

# ApText's four dependencies (APTEXT_TODO.md, "Dependencies"), each built
# here from its sources as a static library of its own, linked PRIVATE: no
# HarfBuzz, msdfgen, SheenBidi or libunibreak type is in include/. Pinned,
# because what each computes is part of ApText's output: a layout and an
# atlas's pixels must be the same bits on every build. Upgrading one is a
# decision, with the tests' expected values checked again. Each is built
# with floating-point contraction off, as ApMath's polygons are.
#
# HarfBuzz 14.6.0 (MIT): shaping, and glyph outlines through its draw
# callbacks, so FreeType isn't needed. Its one-file amalgamation, with no
# optional dependency (no FreeType, GLib, ICU or CoreText): its own Unicode
# data, the same on every platform.
declare_vendored_or_fetch_sources(harfbuzz harfbuzz
        https://github.com/harfbuzz/harfbuzz.git 14.6.0
        src/harfbuzz.cc)
FetchContent_MakeAvailable(harfbuzz)
add_library(aptext_harfbuzz STATIC ${harfbuzz_SOURCE_DIR}/src/harfbuzz.cc)
target_include_directories(aptext_harfbuzz PUBLIC ${harfbuzz_SOURCE_DIR}/src)
set_target_properties(aptext_harfbuzz PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_STANDARD 17)
target_link_libraries(ApricotFields PRIVATE aptext_harfbuzz)

# msdfgen 1.13 (MIT): the MTSDF generator, its core only - not its ext
# (FreeType and file loading) and not msdf-atlas-gen (whose packer assumes a
# fixed glyph set).
declare_vendored_or_fetch_sources(msdfgen msdfgen
        https://github.com/Chlumsky/msdfgen.git v1.13
        core/msdfgen.cpp)
FetchContent_MakeAvailable(msdfgen)
set(APTEXT_MSDFGEN_SOURCES Contour DistanceMapping EdgeHolder MSDFErrorCorrection Projection Scanline Shape
        contour-combiners convergent-curve-ordering edge-coloring edge-segments edge-selectors equation-solver
        msdf-error-correction msdfgen rasterization sdf-error-estimation)
list(TRANSFORM APTEXT_MSDFGEN_SOURCES PREPEND ${msdfgen_SOURCE_DIR}/core/)
list(TRANSFORM APTEXT_MSDFGEN_SOURCES APPEND .cpp)
add_library(aptext_msdfgen STATIC ${APTEXT_MSDFGEN_SOURCES})
target_include_directories(aptext_msdfgen PUBLIC ${msdfgen_SOURCE_DIR})
# Defined empty, in place of the msdfgen-config.h its own CMakeLists.txt
# writes: a static library exports nothing.
target_compile_definitions(aptext_msdfgen PUBLIC "MSDFGEN_PUBLIC=")
set_target_properties(aptext_msdfgen PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_STANDARD 17)
target_link_libraries(ApricotFields PRIVATE aptext_msdfgen)

# SheenBidi 2.9.0 (Apache 2.0): Unicode's bidirectional algorithm and
# script runs. 2.9, not 3.0: 3.0 adds a process-wide default allocator a
# caller can replace, writable global state ApricotFields may not have.
declare_vendored_or_fetch_sources(sheenbidi SheenBidi
        https://github.com/Tehreer/SheenBidi.git v2.9.0
        Source/SheenBidi.c)
FetchContent_MakeAvailable(sheenbidi)
add_library(aptext_sheenbidi STATIC ${sheenbidi_SOURCE_DIR}/Source/SheenBidi.c)
target_compile_definitions(aptext_sheenbidi PRIVATE SB_CONFIG_UNITY)
target_include_directories(aptext_sheenbidi PUBLIC ${sheenbidi_SOURCE_DIR}/Headers
        PRIVATE ${sheenbidi_SOURCE_DIR}/Source)
set_target_properties(aptext_sheenbidi PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(ApricotFields PRIVATE aptext_sheenbidi)

# libunibreak 8.0 (zlib): Unicode's line breaking algorithm.
declare_vendored_or_fetch_sources(libunibreak libunibreak
        https://github.com/adah1972/libunibreak.git libunibreak_8_0
        src/linebreak.c)
FetchContent_MakeAvailable(libunibreak)
add_library(aptext_unibreak STATIC
        ${libunibreak_SOURCE_DIR}/src/linebreak.c
        ${libunibreak_SOURCE_DIR}/src/linebreakdata.c
        ${libunibreak_SOURCE_DIR}/src/linebreakdef.c
        ${libunibreak_SOURCE_DIR}/src/linebreakauxdata.c
        ${libunibreak_SOURCE_DIR}/src/eastasianwidthdata.c
        ${libunibreak_SOURCE_DIR}/src/eastasianwidthdef.c
        ${libunibreak_SOURCE_DIR}/src/emojidata.c
        ${libunibreak_SOURCE_DIR}/src/emojidef.c
        ${libunibreak_SOURCE_DIR}/src/unibreakbase.c
        ${libunibreak_SOURCE_DIR}/src/unibreakdef.c)
target_include_directories(aptext_unibreak PUBLIC ${libunibreak_SOURCE_DIR}/src)
set_target_properties(aptext_unibreak PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(ApricotFields PRIVATE aptext_unibreak)

# Hosek and Wilkie's sky model, version 1.4a (APREND_COMMAND_LIT_SKY's
# HOSEK_WILKIE): its RGB coefficients alone, which aprendlit.cpp cooks for a
# sun and an air. 3-clause BSD licence, in the data file itself and in its
# README.txt. The authors' release is a zip, so it is pinned by its hash.
# The data file declares its tables as writable globals, which a library
# here may not have, so it is read here as it is and written into the build
# directory with every table static and const (aprend_hosek_rgb.inc); the
# file itself is left untouched.
set(APRICOTFIELDS_HOSEK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.deps/HosekWilkie-1.4a")
if (EXISTS "${APRICOTFIELDS_HOSEK_DIR}/ArHosekSkyModelData_RGB.h")
    message(STATUS "hosek_wilkie: using vendored ${APRICOTFIELDS_HOSEK_DIR}")
else ()
    message(STATUS "hosek_wilkie: no vendored copy under .deps/, fetching the 1.4a release")
    FetchContent_Declare(hosek_wilkie
            URL https://cgg.mff.cuni.cz/projects/SkylightModelling/HosekWilkie_SkylightModel_C_Source.1.4a.zip
            URL_HASH SHA256=743e81a7fcbed06408490a303dcf1315083d7988a11fc69608e66f6e5417f9de
            SOURCE_SUBDIR sources-only-not-a-directory)
    FetchContent_MakeAvailable(hosek_wilkie)
    # The zip holds the release's folder beside a __MACOSX one.
    set(APRICOTFIELDS_HOSEK_DIR "${hosek_wilkie_SOURCE_DIR}/HosekWilkie_SkylightModel_C_Source.1.4a")
endif ()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${APRICOTFIELDS_HOSEK_DIR}/ArHosekSkyModelData_RGB.h")
file(READ "${APRICOTFIELDS_HOSEK_DIR}/ArHosekSkyModelData_RGB.h" _hosek_rgb)
string(REPLACE "datasetsRGB" "aprend_hosek_datasetsRGB" _hosek_rgb "${_hosek_rgb}")
string(REPLACE "datasetRGB" "aprend_hosek_datasetRGB" _hosek_rgb "${_hosek_rgb}")
string(REPLACE "double* aprend_hosek_" "static const double *const aprend_hosek_" _hosek_rgb "${_hosek_rgb}")
string(REPLACE "double aprend_hosek_" "static const double aprend_hosek_" _hosek_rgb "${_hosek_rgb}")
file(MAKE_DIRECTORY ${APRICOTFIELDS_SHADER_INC_DIR})
file(CONFIGURE OUTPUT ${APRICOTFIELDS_SHADER_INC_DIR}/aprend_hosek_rgb.inc CONTENT "${_hosek_rgb}" @ONLY)
# For a caller's tests that work the model out in C to compare with.
set(APRICOTFIELDS_HOSEK_RGB_INC ${APRICOTFIELDS_SHADER_INC_DIR}/aprend_hosek_rgb.inc CACHE INTERNAL "")

# No floating-point contraction where ApMath's polygons compute in doubles:
# results must be the same bits on every platform. MSVC doesn't contract
# unless asked (/fp:contract), so only Clang and GCC need telling.
if (NOT MSVC)
    target_compile_options(apmath_clipper2 PRIVATE -ffp-contract=off)
    set_source_files_properties(src/math/apmathpoly.cpp PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
    # The same for ApText: a layout's positions and a glyph's distance field
    # are the same bits on every platform (aptext.h).
    foreach (target aptext_harfbuzz aptext_msdfgen aptext_sheenbidi aptext_unibreak)
        target_compile_options(${target} PRIVATE -ffp-contract=off -w)
    endforeach ()
    set_source_files_properties(src/text/aptext.cpp src/text/aptext_layout.cpp src/text/aptext_atlas.cpp
            PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
endif ()

# Headless tests - no GPU, no window. Run with ctest, or the executable.
option(APRICOTFIELDS_BUILD_TESTS "Build ApricotFields' headless tests" ON)
if (APRICOTFIELDS_BUILD_TESTS)
    enable_testing()
    add_executable(apricore_tests tests/apricore_tests.c)
    target_link_libraries(apricore_tests PRIVATE ApricotFields)
    target_compile_definitions(apricore_tests PRIVATE
        APRICORE_TEST_CMAKE_VERSION="${PROJECT_VERSION}")
    add_test(NAME apricore_tests COMMAND apricore_tests)
    add_executable(aparchive_tests tests/aparchive_tests.cpp)
    target_link_libraries(aparchive_tests PRIVATE ApricotFields)
    add_test(NAME aparchive_tests COMMAND aparchive_tests)
    add_executable(apaudio_tests tests/apaudio_tests.c)
    target_link_libraries(apaudio_tests PRIVATE ApricotFields)
    add_test(NAME apaudio_tests COMMAND apaudio_tests)
    # Two sessions talking over the loopback interface: no GPU, no window,
    # but it does open a listening socket on 127.0.0.1.
    add_executable(apsync_tests tests/apsync_tests.c)
    target_link_libraries(apsync_tests PRIVATE ApricotFields)
    add_test(NAME apsync_tests COMMAND apsync_tests)
    # ApText's contract (APTEXT_TODO.md): written before the implementation,
    # a behavioural test reports pending while its call isn't built.
    add_executable(aptext_tests tests/aptext_tests.c)
    target_link_libraries(aptext_tests PRIVATE ApricotFields)
    add_test(NAME aptext_tests COMMAND aptext_tests)
    # The same layout and atlas bits on every platform, against values
    # recorded on macOS.
    add_executable(aptext_bits_tests tests/aptext_bits_tests.c)
    target_link_libraries(aptext_bits_tests PRIVATE ApricotFields)
    add_test(NAME aptext_bits_tests COMMAND aptext_bits_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/aptext)
    add_executable(apmathbvh_tests tests/apmathbvh_tests.c)
    target_link_libraries(apmathbvh_tests PRIVATE ApricotFields)
    add_test(NAME apmathbvh_tests COMMAND apmathbvh_tests)
    add_executable(apmathpoly_tests tests/apmathpoly_tests.c)
    target_link_libraries(apmathpoly_tests PRIVATE ApricotFields)
    add_test(NAME apmathpoly_tests COMMAND apmathpoly_tests)
    # The range list under Aprend's pools: an internal header with no GPU in
    # it, which is why this one test sees src/render.
    add_executable(aprendranges_tests tests/aprendranges_tests.cpp)
    target_include_directories(aprendranges_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/render)
    add_test(NAME aprendranges_tests COMMAND aprendranges_tests)
    # The open-addressing table under Aprend's command lists, an internal
    # header too.
    add_executable(aprihashindex_tests tests/aprihashindex_tests.cpp)
    target_include_directories(aprihashindex_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME aprihashindex_tests COMMAND aprihashindex_tests)
    # SPIR-V reflection, and writing a member where the reflection lays it
    # out: no device either, so it sees src/render too.
    add_executable(aprendreflect_tests tests/aprendreflect_tests.cpp)
    target_include_directories(aprendreflect_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/render)
    target_link_libraries(aprendreflect_tests PRIVATE ApricotFields)
    add_test(NAME aprendreflect_tests COMMAND aprendreflect_tests)
    # The features Aprend's own code uses, from C.
    add_executable(aprendfeatures_tests tests/aprendfeatures_tests.c)
    target_link_libraries(aprendfeatures_tests PRIVATE ApricotFields)
    add_test(NAME aprendfeatures_tests COMMAND aprendfeatures_tests)
endif ()

# Fuzz targets (tests/fuzz/aprifuzz.hpp). Each is built two ways: as a
# ctest that replays its seed corpus and every input the fuzzer once
# crashed on, with whatever compiler builds the tests; and, under
# APRICOTFIELDS_BUILD_FUZZERS, as a libFuzzer executable, which needs Clang
# with -fsanitize=fuzzer (the apricotfields-*-fuzz presets: Homebrew's LLVM
# on macOS, since Apple's Clang has no libFuzzer). The presets build every
# target with -fsanitize=fuzzer-no-link,address,undefined; only the fuzz
# executables link libFuzzer itself.
set(APRICOTFIELDS_FUZZ_TARGETS aparchive_fuzz aptext_fuzz)
if (APRICOTFIELDS_BUILD_TESTS)
    foreach (target IN LISTS APRICOTFIELDS_FUZZ_TARGETS)
        add_executable(${target}_replay tests/fuzz/aprifuzz_replay.cpp tests/fuzz/${target}.cpp)
        target_link_libraries(${target}_replay PRIVATE ApricotFields)
        add_test(NAME ${target}_regressions COMMAND ${target}_replay
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/${target}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/regressions/${target})
    endforeach ()
endif ()
option(APRICOTFIELDS_BUILD_FUZZERS "Build ApricotFields' libFuzzer targets (Clang only)" OFF)
if (APRICOTFIELDS_BUILD_FUZZERS)
    foreach (target IN LISTS APRICOTFIELDS_FUZZ_TARGETS)
        add_executable(${target} tests/fuzz/${target}.cpp)
        target_link_libraries(${target} PRIVATE ApricotFields)
        target_link_options(${target} PRIVATE -fsanitize=fuzzer)
    endforeach ()
endif ()
