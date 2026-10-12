cmake_minimum_required(VERSION 3.30)
project(ApricotFields)

set(CMAKE_CXX_STANDARD 26)

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
        src/archive/aparchive_internal.hpp
        src/archive/aparchive.cpp
        src/audio/apaudio.cpp
        src/audio/apaudiocues.cpp
        src/math/apmathbvh.cpp
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
        include/archive/aparchive.h
        include/audio/apaudio.h
        include/audio/apaudiocues.h
        include/math/apmathbvh.h
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

# Vulkan stuff -- only needed on Windows/Linux, where SpudGPU's Vulkan
# backend is selected and shaders are compiled to SPIR-V via glslc. ApricotFields
# never includes Vulkan headers directly (it only calls spudgpu.h's
# backend-agnostic C API), so this must not run on Apple, where SpudGPU
# uses the Metal backend instead.
if (WIN32 OR (UNIX AND NOT APPLE))
    # Manually hint where Vulkan is located on Windows
    # On Linux, it is automatically /usr/include and /usr/lib64
    if (WIN32)
        set(Vulkan_INCLUDE_DIR "C:/VulkanSDK/1.4.341.1/Include")
        set(Vulkan_LIBRARY "C:/VulkanSDK/1.4.341.1/Lib/vulkan-1.lib")
        set(Vulkan_GLSLC_EXECUTABLE "C:/VulkanSDK/1.4.341.1/Bin/glslc.exe")
    endif ()

    find_package(Vulkan REQUIRED)
    target_include_directories(ApricotFields PRIVATE ${Vulkan_INCLUDE_DIRS})
    target_link_libraries(ApricotFields PRIVATE Vulkan::Vulkan)

    if (NOT Vulkan_GLSLC_EXECUTABLE)
        message(FATAL_ERROR "glslc not found. Install the Vulkan SDK.")
    endif ()

    # Shaders are compiled to the parent project's runtime output directory
    # (set by whatever project adds this directory).  Fall back to the local
    # build dir when ApricotFields is built standalone.
    if (CMAKE_RUNTIME_OUTPUT_DIRECTORY)
        set(APRICOT_SHADER_OUTDIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders")
    else ()
        set(APRICOT_SHADER_OUTDIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
    endif ()

    function(apricot_compile_shader TARGET SHADER)
        get_filename_component(NAME ${SHADER} NAME)
        set(SPV "${APRICOT_SHADER_OUTDIR}/${NAME}.spv")
        add_custom_command(
                OUTPUT ${SPV}
                COMMAND ${CMAKE_COMMAND} -E make_directory "${APRICOT_SHADER_OUTDIR}"
                COMMAND ${Vulkan_GLSLC_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/${SHADER} -o ${SPV}
                DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${SHADER}
        )
        target_sources(${TARGET} PRIVATE ${SPV})
    endfunction()

    apricot_compile_shader(ApricotFields shaders/apricot_default.vert)
    apricot_compile_shader(ApricotFields shaders/apricot_default.frag)
endif ()

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
# NOTE: the fetch fallback tag is a placeholder - pin a real glaze release once
# one is chosen; the vendored eqdev/.deps/glaze path is unaffected by it.
declare_vendored_or_fetch(glaze glaze https://github.com/stephenberry/glaze.git main)
FetchContent_MakeAvailable(glaze)
target_link_libraries(ApricotFields PRIVATE glaze::glaze)

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
