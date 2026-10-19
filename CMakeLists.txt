cmake_minimum_required(VERSION 3.30)
project(ApricotFields)

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
        HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin" "C:/VulkanSDK/1.4.341.1/Bin"
        REQUIRED)
set(APRICOTFIELDS_SHADER_INC_DIR ${CMAKE_CURRENT_BINARY_DIR}/aprend_shaders)
set(APRICOTFIELDS_SHADERS
        aprend_fullscreen.vert
        aprend_downsample.frag
        aprend_bloom_prefilter.frag
        aprend_bloom_downsample.frag
        aprend_bloom_upsample.frag
        aprend_bloom_composite.frag
        aprend_tone_map.frag
        aprend_lit.vert
        aprend_lit.frag
        aprend_lit_shadow.vert
        aprend_lit_depth.frag
        aprend_environment_from_cube.frag
        aprend_environment_from_equirect.frag
        aprend_environment_prefilter.frag
        aprend_environment_irradiance.frag
        aprend_environment_sky.frag
        aprend_lit_depth_cutout.frag
        aprend_lit_sky.frag
        aprend_lit_cull.comp
        aprend_lit_brdf_lut.frag
        aprend_lit_occlusion.frag
        aprend_lit_occlusion_blur.frag
        aprend_exposure_luminance.frag
        aprend_exposure_adapt.frag
)
# What the shaders include (#include, GL_GOOGLE_include_directive), from
# shaders/: every shader is compiled again when one of them changes.
set(APRICOTFIELDS_SHADER_INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/aprend_lit_scene.glslh
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
# Pinned to a commit, not a branch, so a fetched build is the same every time:
# 52971fe5 (v9.0.0-22, 2026-09-29), the commit vendored in eqdev/.deps/glaze,
# so a fetched build and a vendored one use the same glaze. Move both together.
declare_vendored_or_fetch(glaze glaze https://github.com/stephenberry/glaze.git 52971fe5fc97d0bac361c20c9cf4cf22e4dbdd12)
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

# No floating-point contraction where ApMath's polygons compute in doubles:
# results must be the same bits on every platform. MSVC doesn't contract
# unless asked (/fp:contract), so only Clang and GCC need telling.
if (NOT MSVC)
    target_compile_options(apmath_clipper2 PRIVATE -ffp-contract=off)
    set_source_files_properties(src/math/apmathpoly.cpp PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
endif ()

# Headless tests - no GPU, no window. Run with ctest, or the executable.
option(APRICOTFIELDS_BUILD_TESTS "Build ApricotFields' headless tests" ON)
if (APRICOTFIELDS_BUILD_TESTS)
    enable_testing()
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
    # SPIR-V reflection, and writing a member where the reflection lays it
    # out: no device either, so it sees src/render too.
    add_executable(aprendreflect_tests tests/aprendreflect_tests.cpp)
    target_include_directories(aprendreflect_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/render)
    target_link_libraries(aprendreflect_tests PRIVATE ApricotFields)
    add_test(NAME aprendreflect_tests COMMAND aprendreflect_tests)
endif ()
