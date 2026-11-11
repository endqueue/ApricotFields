# rbtrend_reflect on its own, for the build machine: what
# cmake/rbtrend_slang.cmake builds through ExternalProject wherever the shader
# step runs, in a native and a cross-compiling build alike (card X18), so its
# SPIRV-Cross is never a target of the build that runs it.
#
# RBTREND_REFLECT_CARROTTUS_INCLUDE names carrottus's include directory (the tool
# reads crgpu.h's enums). RBTREND_REFLECT_SPIRV_CROSS_DIR names a
# SPIRV-Cross checkout (the workspace's vendored one); without it,
# SPIRV-Cross is fetched at RBTREND_REFLECT_SPIRV_CROSS_TAG. The tool is
# written to bin/ of this build, the same path in every configuration.

cmake_minimum_required(VERSION 3.25)
project(rbtrend_reflect CXX)

set(RBTREND_REFLECT_CARROTTUS_INCLUDE "" CACHE PATH "carrottus's include directory")
set(RBTREND_REFLECT_SPIRV_CROSS_DIR "" CACHE PATH "A SPIRV-Cross checkout; fetched when empty")
set(RBTREND_REFLECT_SPIRV_CROSS_TAG "vulkan-sdk-1.4.363.0" CACHE STRING "The SPIRV-Cross tag fetched when no checkout is named")
if(NOT RBTREND_REFLECT_CARROTTUS_INCLUDE)
    message(FATAL_ERROR "rbtrend_reflect: RBTREND_REFLECT_CARROTTUS_INCLUDE names no directory")
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
if(RBTREND_REFLECT_SPIRV_CROSS_DIR)
    FetchContent_Declare(spirv_cross SOURCE_DIR "${RBTREND_REFLECT_SPIRV_CROSS_DIR}")
else()
    FetchContent_Declare(spirv_cross GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
        GIT_TAG "${RBTREND_REFLECT_SPIRV_CROSS_TAG}")
endif()
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
FetchContent_MakeAvailable(spirv_cross)

add_executable(rbtrend_reflect rbtrend_reflect.cpp rbtrend_reflect_spirv.cpp rbtrend_reflect_inputs.cpp rbtrend_reflect_write.cpp)
target_compile_features(rbtrend_reflect PRIVATE cxx_std_17)
target_include_directories(rbtrend_reflect PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/render
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${RBTREND_REFLECT_CARROTTUS_INCLUDE})
target_link_libraries(rbtrend_reflect PRIVATE spirv-cross-core)
# The generator expression keeps a multi-config generator from adding a
# per-configuration folder.
set_target_properties(rbtrend_reflect PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin$<0:>")
