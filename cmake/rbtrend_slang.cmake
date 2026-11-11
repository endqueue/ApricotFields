# Shaders for Rbtrend: Slang in, each graphics API's own format out, at build
# time (cards A29, A9 and X18 in the workspace's DECISIONS.md; RBTREND_TODO.md
# item 22). Rbtrend compiles nothing at run time: a shader is made from a
# package (rbtrend_shader_create(), rbtrendshader.h) of what this step writes.
# rabbittus builds its own shaders with it, and a caller's build may
# include this file for its own.
#
# The shader step itself is Carrottus's, written once for every caller
# (cr_add_shader(), the file Carrottus's build names in CR_SHADERS_CMAKE):
# SPIR-V on every platform, DXIL on Windows, Metal source and a .metallib
# where a .metallib is made, each set of the Metal source padded to every
# binding the shader declares. This file adds only Rbtrend's package:
#
# - <stem>.reflection, the shader's reflection table (rbtrendshader.h,
#   RBTREND_SHADER_PACKAGE::reflection), written from its SPIR-V by
#   tools/rbtrend_reflect (card X18), a vertex shader's inputs completed from
#   Slang's reflection JSON (card A51: the inputs it declares and its SPIR-V
#   dropped), and <stem>.reflection.inc, it as a C array. A shader Rbtrend
#   can't bind fails here, not at run time.
# - <stem>.package.inc: includes the formats' .inc files and defines
#   `static const RBTREND_SHADER_PACKAGE <stem>_package`, for a C or C++
#   source to include after rbtrendshader.h: the reflection, and the code in
#   the format this build's device takes (GRAPHICS_BACKEND): SPIR-V for
#   Vulkan, DXIL for D3D12, the .metallib for Metal.
#
# tools/rbtrend_reflect is a program of its own, built for the build machine
# through ExternalProject (tools/rbtrend_reflect/CMakeLists.txt) in a native
# and a cross-compiling build alike, or the one RBTREND_REFLECT_EXECUTABLE
# names; its SPIRV-Cross is never a target of the build that includes this
# file, and never in the library.

include_guard(GLOBAL)

if(NOT CR_SHADERS_CMAKE)
    message(FATAL_ERROR "rbtrend_slang.cmake: Carrottus's build names no shader step (CR_SHADERS_CMAKE); add Carrottus first")
endif()
include(${CR_SHADERS_CMAKE})

set(RBTREND_REFLECT_EXECUTABLE "" CACHE FILEPATH
    "An rbtrend_reflect already built for this machine; built here when empty")
if(NOT TARGET rbtrend_reflect)
    add_executable(rbtrend_reflect IMPORTED GLOBAL)
    if(RBTREND_REFLECT_EXECUTABLE)
        set_target_properties(rbtrend_reflect PROPERTIES IMPORTED_LOCATION ${RBTREND_REFLECT_EXECUTABLE})
    else()
        include(ExternalProject)
        set(_rbtrend_reflect_spirv_cross "")
        foreach(_cand SPIRV-Cross SPIRV-Cross-src)
            set(_dir ${CMAKE_CURRENT_LIST_DIR}/../../.deps/${_cand})
            if(NOT _rbtrend_reflect_spirv_cross AND EXISTS ${_dir}/CMakeLists.txt)
                get_filename_component(_rbtrend_reflect_spirv_cross ${_dir} ABSOLUTE)
            endif()
        endforeach()
        get_filename_component(_rbtrend_carrottus_include ${CR_SHADERS_CMAKE} DIRECTORY)
        get_filename_component(_rbtrend_carrottus_include ${_rbtrend_carrottus_include}/../include ABSOLUTE)
        set(_rbtrend_reflect_dir ${CMAKE_BINARY_DIR}/rbtrend_reflect_host)
        # Nothing of this build's toolchain is passed on: built with the
        # build machine's compiler, in Release, for the build machine.
        ExternalProject_Add(rbtrend_reflect_build
            SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../tools/rbtrend_reflect
            BINARY_DIR ${_rbtrend_reflect_dir}
            CMAKE_ARGS -DRBTREND_REFLECT_SPIRV_CROSS_DIR=${_rbtrend_reflect_spirv_cross}
                       -DRBTREND_REFLECT_CARROTTUS_INCLUDE=${_rbtrend_carrottus_include} -DCMAKE_BUILD_TYPE=Release
            BUILD_COMMAND ${CMAKE_COMMAND} --build ${_rbtrend_reflect_dir} --config Release
            INSTALL_COMMAND ""
            # Asked every time, so an edit to the tool or the reflection it
            # moved from is built; nothing is remade when nothing changed.
            BUILD_ALWAYS ON
            BUILD_BYPRODUCTS ${_rbtrend_reflect_dir}/bin/rbtrend_reflect${CMAKE_HOST_EXECUTABLE_SUFFIX})
        set_target_properties(rbtrend_reflect PROPERTIES
            IMPORTED_LOCATION ${_rbtrend_reflect_dir}/bin/rbtrend_reflect${CMAKE_HOST_EXECUTABLE_SUFFIX})
        add_dependencies(rbtrend_reflect rbtrend_reflect_build)
    endif()
endif()

# rbtrend_add_slang_shader(<out_var> SOURCE <file.slang> OUT_DIR <dir>
#     [INCLUDE_DIRS <dir>...] [DEPENDS <file>...] [DXIL_PROFILE <profile>]
#     [SPIRV_CAPABILITIES <cap>...] [METAL_ROUTE SLANG <reason>])
#
# SOURCE is absolute or relative to the calling directory. Its stem ends in
# _vert, _frag, _comp or _mesh, which names the package's stage; the source
# names it too ([shader("vertex")]...). INCLUDE_DIRS, DEPENDS,
# SPIRV_CAPABILITIES and METAL_ROUTE are cr_add_shader()'s. DXIL_PROFILE
# is the shader model DXC compiles for: vs_6_0, ps_6_0 and cs_6_0 unless
# given, ms_6_5 for a mesh shader. Appends the shader's outputs to
# <out_var> in the caller's scope.
function(rbtrend_add_slang_shader out_var)
    cmake_parse_arguments(ARG "" "SOURCE;OUT_DIR;DXIL_PROFILE"
        "INCLUDE_DIRS;DEPENDS;SPIRV_CAPABILITIES;METAL_ROUTE" ${ARGN})
    get_filename_component(entry ${ARG_SOURCE} NAME_WE)
    if(entry MATCHES "_vert$")
        set(stage CRGPU_SHADER_STAGE_VERTEX)
        set(stage_name vertex)
        set(profile vs_6_0)
    elseif(entry MATCHES "_frag$")
        set(stage CRGPU_SHADER_STAGE_FRAGMENT)
        set(stage_name fragment)
        set(profile ps_6_0)
    elseif(entry MATCHES "_comp$")
        set(stage CRGPU_SHADER_STAGE_COMPUTE)
        set(stage_name compute)
        set(profile cs_6_0)
    elseif(entry MATCHES "_mesh$")
        set(stage CRGPU_SHADER_STAGE_MESH)
        set(stage_name mesh)
        set(profile ms_6_5)
    else()
        message(FATAL_ERROR "rbtrend_add_slang_shader: ${entry} doesn't end in _vert, _frag, _comp or _mesh")
    endif()
    if(ARG_DXIL_PROFILE)
        set(profile ${ARG_DXIL_PROFILE})
    endif()
    set(route "")
    if(ARG_METAL_ROUTE)
        set(route METAL_ROUTE ${ARG_METAL_ROUTE})
    endif()
    set(outputs "")
    cr_add_shader(${entry} SOURCE ${ARG_SOURCE} OUT_DIR ${ARG_OUT_DIR} DXIL_PROFILE ${profile}
        INCLUDE_DIRS ${ARG_INCLUDE_DIRS} DEPENDS ${ARG_DEPENDS} SPIRV_CAPABILITIES ${ARG_SPIRV_CAPABILITIES}
        ${route} OUTPUTS outputs)

    # The reflection, from the SPIR-V. It describes the code the package
    # carries (below): where that is DXIL, a vertex shader's inputs are its
    # DXIL signature's, every one the shader declares, read from Slang's
    # reflection JSON beside the SPIR-V (card A51), since D3D12 requires an
    # input layout to feed each; SPIR-V, and the Metal made from it, keep only
    # those the entry point reads, and a Vulkan pipeline feeding another is
    # told the attribute isn't consumed.
    set(out ${ARG_OUT_DIR}/${entry})
    set(declared "")
    if(stage_name STREQUAL "vertex" AND GRAPHICS_BACKEND STREQUAL "D3D12" AND "dxil" IN_LIST ${entry}_FORMATS)
        set(declared --declared ${out}.reflection.json)
    endif()
    get_filename_component(embed ${CR_SHADERS_CMAKE} DIRECTORY)
    set(embed ${embed}/cr_embed.cmake)
    add_custom_command(
        OUTPUT ${out}.reflection ${out}.reflection.inc
        COMMAND rbtrend_reflect --stage ${stage_name} ${declared} ${out}.spv ${out}.reflection
        COMMAND ${CMAKE_COMMAND} -DIN=${out}.reflection -DOUT=${out}.reflection.inc -DNAME=${entry}_reflection -P ${embed}
        DEPENDS ${out}.spv ${out}.reflection.json rbtrend_reflect ${embed}
        COMMENT "Rbtrend: ${entry}'s reflection")
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
        "// Written at configure time by rbtrend_slang.cmake: not by hand.\n${includes}static const RBTREND_SHADER_PACKAGE ${entry}_package = {\n\t{RBT_STRUCT_TYPE_REND_SHADER_PACKAGE, NULL},\n\t${stage},\n\t\"${entry}\",\n\t${spirv},\n\t${dxil},\n\t${metallib},\n\t${entry}_reflection, ${entry}_reflection_size\n};\n"
        @ONLY)

    set(${out_var} ${${out_var}} ${outputs} PARENT_SCOPE)
endfunction()
