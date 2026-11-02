# Shaders for Aprend: Slang in, each graphics API's own format out, at build
# time (cards A29, A9 and X18 in the workspace's DECISIONS.md; APREND_TODO.md
# item 22). Aprend compiles nothing at run time: a shader is made from a
# package (aprend_shader_create(), aprendshader.h) of what this step writes.
# ApricotFields builds its own shaders with it, and a caller's build may
# include this file for its own.
#
# The shader step itself is SpudLib's, written once for every caller
# (spud_add_shader(), the file SpudLib's build names in SPUD_SHADERS_CMAKE):
# SPIR-V on every platform, DXIL on Windows, Metal source and a .metallib
# where a .metallib is made, each set of the Metal source padded to every
# binding the shader declares. This file adds only Aprend's package:
#
# - <stem>.reflection, the shader's reflection table (aprendshader.h,
#   APREND_SHADER_PACKAGE::reflection), written from its SPIR-V by
#   tools/aprend_reflect (card X18), and <stem>.reflection.inc, it as a C
#   array. A shader Aprend can't bind fails here, not at run time.
# - <stem>.package.inc: includes the formats' .inc files and defines
#   `static const APREND_SHADER_PACKAGE <stem>_package`, for a C or C++
#   source to include after aprendshader.h: the reflection, and the code in
#   the format this build's device takes (GRAPHICS_BACKEND): SPIR-V for
#   Vulkan, DXIL for D3D12, the .metallib for Metal.
#
# tools/aprend_reflect is a program of its own, built for the build machine
# through ExternalProject (tools/aprend_reflect/CMakeLists.txt) in a native
# and a cross-compiling build alike, or the one APREND_REFLECT_EXECUTABLE
# names; its SPIRV-Cross is never a target of the build that includes this
# file, and never in the library.

include_guard(GLOBAL)

if(NOT SPUD_SHADERS_CMAKE)
    message(FATAL_ERROR "aprend_slang.cmake: SpudLib's build names no shader step (SPUD_SHADERS_CMAKE); add SpudLib first")
endif()
include(${SPUD_SHADERS_CMAKE})

set(APREND_REFLECT_EXECUTABLE "" CACHE FILEPATH
    "An aprend_reflect already built for this machine; built here when empty")
if(NOT TARGET aprend_reflect)
    add_executable(aprend_reflect IMPORTED GLOBAL)
    if(APREND_REFLECT_EXECUTABLE)
        set_target_properties(aprend_reflect PROPERTIES IMPORTED_LOCATION ${APREND_REFLECT_EXECUTABLE})
    else()
        include(ExternalProject)
        set(_aprend_reflect_spirv_cross "")
        foreach(_cand SPIRV-Cross SPIRV-Cross-src)
            set(_dir ${CMAKE_CURRENT_LIST_DIR}/../../.deps/${_cand})
            if(NOT _aprend_reflect_spirv_cross AND EXISTS ${_dir}/CMakeLists.txt)
                get_filename_component(_aprend_reflect_spirv_cross ${_dir} ABSOLUTE)
            endif()
        endforeach()
        get_filename_component(_aprend_spudlib_include ${SPUD_SHADERS_CMAKE} DIRECTORY)
        get_filename_component(_aprend_spudlib_include ${_aprend_spudlib_include}/../include ABSOLUTE)
        set(_aprend_reflect_dir ${CMAKE_BINARY_DIR}/aprend_reflect_host)
        # Nothing of this build's toolchain is passed on: built with the
        # build machine's compiler, in Release, for the build machine.
        ExternalProject_Add(aprend_reflect_build
            SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../tools/aprend_reflect
            BINARY_DIR ${_aprend_reflect_dir}
            CMAKE_ARGS -DAPREND_REFLECT_SPIRV_CROSS_DIR=${_aprend_reflect_spirv_cross}
                       -DAPREND_REFLECT_SPUDLIB_INCLUDE=${_aprend_spudlib_include} -DCMAKE_BUILD_TYPE=Release
            BUILD_COMMAND ${CMAKE_COMMAND} --build ${_aprend_reflect_dir} --config Release
            INSTALL_COMMAND ""
            # Asked every time, so an edit to the tool or the reflection it
            # moved from is built; nothing is remade when nothing changed.
            BUILD_ALWAYS ON
            BUILD_BYPRODUCTS ${_aprend_reflect_dir}/bin/aprend_reflect${CMAKE_HOST_EXECUTABLE_SUFFIX})
        set_target_properties(aprend_reflect PROPERTIES
            IMPORTED_LOCATION ${_aprend_reflect_dir}/bin/aprend_reflect${CMAKE_HOST_EXECUTABLE_SUFFIX})
        add_dependencies(aprend_reflect aprend_reflect_build)
    endif()
endif()

# aprend_add_slang_shader(<out_var> SOURCE <file.slang> OUT_DIR <dir>
#     [INCLUDE_DIRS <dir>...] [DEPENDS <file>...] [DXIL_PROFILE <profile>]
#     [SPIRV_CAPABILITIES <cap>...] [METAL_ROUTE SLANG <reason>])
#
# SOURCE is absolute or relative to the calling directory. Its stem ends in
# _vert, _frag, _comp or _mesh, which names the package's stage; the source
# names it too ([shader("vertex")]...). INCLUDE_DIRS, DEPENDS,
# SPIRV_CAPABILITIES and METAL_ROUTE are spud_add_shader()'s. DXIL_PROFILE
# is the shader model DXC compiles for: vs_6_0, ps_6_0 and cs_6_0 unless
# given, ms_6_5 for a mesh shader. Appends the shader's outputs to
# <out_var> in the caller's scope.
function(aprend_add_slang_shader out_var)
    cmake_parse_arguments(ARG "" "SOURCE;OUT_DIR;DXIL_PROFILE"
        "INCLUDE_DIRS;DEPENDS;SPIRV_CAPABILITIES;METAL_ROUTE" ${ARGN})
    get_filename_component(entry ${ARG_SOURCE} NAME_WE)
    if(entry MATCHES "_vert$")
        set(stage SPUDGPU_SHADER_STAGE_VERTEX)
        set(stage_name vertex)
        set(profile vs_6_0)
    elseif(entry MATCHES "_frag$")
        set(stage SPUDGPU_SHADER_STAGE_FRAGMENT)
        set(stage_name fragment)
        set(profile ps_6_0)
    elseif(entry MATCHES "_comp$")
        set(stage SPUDGPU_SHADER_STAGE_COMPUTE)
        set(stage_name compute)
        set(profile cs_6_0)
    elseif(entry MATCHES "_mesh$")
        set(stage SPUDGPU_SHADER_STAGE_MESH)
        set(stage_name mesh)
        set(profile ms_6_5)
    else()
        message(FATAL_ERROR "aprend_add_slang_shader: ${entry} doesn't end in _vert, _frag, _comp or _mesh")
    endif()
    if(ARG_DXIL_PROFILE)
        set(profile ${ARG_DXIL_PROFILE})
    endif()
    set(route "")
    if(ARG_METAL_ROUTE)
        set(route METAL_ROUTE ${ARG_METAL_ROUTE})
    endif()
    set(outputs "")
    spud_add_shader(${entry} SOURCE ${ARG_SOURCE} OUT_DIR ${ARG_OUT_DIR} DXIL_PROFILE ${profile}
        INCLUDE_DIRS ${ARG_INCLUDE_DIRS} DEPENDS ${ARG_DEPENDS} SPIRV_CAPABILITIES ${ARG_SPIRV_CAPABILITIES}
        ${route} OUTPUTS outputs)

    # The reflection, from the SPIR-V.
    set(out ${ARG_OUT_DIR}/${entry})
    get_filename_component(embed ${SPUD_SHADERS_CMAKE} DIRECTORY)
    set(embed ${embed}/spud_embed.cmake)
    add_custom_command(
        OUTPUT ${out}.reflection ${out}.reflection.inc
        COMMAND aprend_reflect --stage ${stage_name} ${out}.spv ${out}.reflection
        COMMAND ${CMAKE_COMMAND} -DIN=${out}.reflection -DOUT=${out}.reflection.inc -DNAME=${entry}_reflection -P ${embed}
        DEPENDS ${out}.spv aprend_reflect ${embed}
        COMMENT "Aprend: ${entry}'s reflection")
    list(APPEND outputs ${out}.reflection.inc)

    # The code in the format this build's device takes, alone.
    set(includes "#include \"${entry}.reflection.inc\"\n")
    set(spirv "NULL, 0")
    set(dxil "NULL, 0")
    set(metallib "NULL, 0")
    if(GRAPHICS_BACKEND STREQUAL "Vulkan")
        string(APPEND includes "#include \"${entry}.spv.inc\"\n")
        set(spirv "${entry}_spirv, ${entry}_spirv_size")
    elseif(GRAPHICS_BACKEND STREQUAL "D3D12" AND "dxil" IN_LIST ${entry}_FORMATS)
        string(APPEND includes "#include \"${entry}.dxil.inc\"\n")
        set(dxil "${entry}_dxil, ${entry}_dxil_size")
    elseif(GRAPHICS_BACKEND STREQUAL "Metal" AND "metallib" IN_LIST ${entry}_FORMATS)
        string(APPEND includes "#include \"${entry}.metallib.inc\"\n")
        set(metallib "${entry}_metallib, ${entry}_metallib_size")
    endif()

    # The package, written at configure time: it only names the arrays
    # above, so it changes only when the formats this platform builds do.
    file(CONFIGURE OUTPUT ${ARG_OUT_DIR}/${entry}.package.inc CONTENT
        "// Written at configure time by aprend_slang.cmake: not by hand.\n${includes}static const APREND_SHADER_PACKAGE ${entry}_package = {\n\t{APRI_STRUCT_TYPE_APREND_SHADER_PACKAGE, NULL},\n\t${stage},\n\t\"${entry}\",\n\t${spirv},\n\t${dxil},\n\t${metallib},\n\t${entry}_reflection, ${entry}_reflection_size\n};\n"
        @ONLY)

    set(${out_var} ${${out_var}} ${outputs} PARENT_SCOPE)
endfunction()
