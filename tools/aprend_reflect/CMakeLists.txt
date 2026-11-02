# aprend_reflect on its own, for the build machine: what
# cmake/aprend_slang.cmake builds through ExternalProject wherever the shader
# step runs, in a native and a cross-compiling build alike (card X18), so its
# SPIRV-Cross is never a target of the build that runs it.
#
# APREND_REFLECT_SPUDLIB_INCLUDE names spudlib's include directory (the tool
# reads spudgpu.h's enums). APREND_REFLECT_SPIRV_CROSS_DIR names a
# SPIRV-Cross checkout (the workspace's vendored one); without it,
# SPIRV-Cross is fetched at APREND_REFLECT_SPIRV_CROSS_TAG. The tool is
# written to bin/ of this build, the same path in every configuration.

cmake_minimum_required(VERSION 3.25)
project(aprend_reflect CXX)

set(APREND_REFLECT_SPUDLIB_INCLUDE "" CACHE PATH "spudlib's include directory")
set(APREND_REFLECT_SPIRV_CROSS_DIR "" CACHE PATH "A SPIRV-Cross checkout; fetched when empty")
set(APREND_REFLECT_SPIRV_CROSS_TAG "vulkan-sdk-1.4.363.0" CACHE STRING "The SPIRV-Cross tag fetched when no checkout is named")
if(NOT APREND_REFLECT_SPUDLIB_INCLUDE)
    message(FATAL_ERROR "aprend_reflect: APREND_REFLECT_SPUDLIB_INCLUDE names no directory")
endif()

include(FetchContent)
set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_GLSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_MSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_UTIL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
if(APREND_REFLECT_SPIRV_CROSS_DIR)
    FetchContent_Declare(spirv_cross SOURCE_DIR "${APREND_REFLECT_SPIRV_CROSS_DIR}")
else()
    FetchContent_Declare(spirv_cross GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
        GIT_TAG "${APREND_REFLECT_SPIRV_CROSS_TAG}")
endif()
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
FetchContent_MakeAvailable(spirv_cross)

add_executable(aprend_reflect aprend_reflect.cpp aprend_reflect_spirv.cpp aprend_reflect_write.cpp)
target_compile_features(aprend_reflect PRIVATE cxx_std_17)
target_include_directories(aprend_reflect PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/render
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${APREND_REFLECT_SPUDLIB_INCLUDE})
target_link_libraries(aprend_reflect PRIVATE spirv-cross-core)
# The generator expression keeps a multi-config generator from adding a
# per-configuration folder.
set_target_properties(aprend_reflect PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin$<0:>")
