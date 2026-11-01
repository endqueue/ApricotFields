# Shaders for Aprend: Slang in, each graphics API's own format out, at build
# time (cards A29 and A9 in the workspace's DECISIONS.md; APREND_TODO.md item
# 22). Aprend compiles nothing at run time: a shader is made from a package
# (aprend_shader_create(), aprendshader.h) of what this step writes.
# ApricotFields builds its own shaders with it, and a caller's build may
# include this file for its own.
#
# For each shader <stem> (<stem>.slang, its entry point named <stem>), in the
# OUT_DIR given:
# - <stem>.spv, SPIR-V 1.5, checked by spirv-val; on every platform, since
#   Aprend reflects it on every backend.
# - <stem>.dxil, DXIL: Slang writes HLSL and DXC compiles it, its validator
#   signing what it passes; on Windows, where the D3D12 backend builds.
# - <stem>.slang.metal and <stem>.cross.metal, Metal source by both of card
#   A29's routes (Slang's own, and Slang to SPIR-V to SpudLib's
#   spudgpu_msl, card S20, in the Metal backend's binding layout), kept as
#   build outputs where the spudgpu_msl target exists. Metal 3.1.
# - <stem>.metallib on Apple platforms, from the route the shader names
#   (spudgpu_msl's by default, card S21).
# - <stem>.spv.inc, .dxil.inc, .metallib.inc: each binary as a C array of
#   uint32_t words (aprend_embed.cmake) with <stem>_<format>_size.
# - <stem>.package.inc: includes those and defines
#   `static const APREND_SHADER_PACKAGE <stem>_package`, for a C or C++
#   source to include after aprendshader.h.
#
# The tools come from the Vulkan SDK (VULKAN_SDK); their versions are
# printed at configure time.

include_guard(GLOBAL)

set(_aprend_sdk_bin "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
find_program(APREND_SLANGC slangc HINTS ${_aprend_sdk_bin} REQUIRED)
find_program(APREND_SPIRV_VAL spirv-val HINTS ${_aprend_sdk_bin} REQUIRED)
if(WIN32)
    find_program(APREND_DXC dxc HINTS ${_aprend_sdk_bin} REQUIRED)
endif()
execute_process(COMMAND ${APREND_SLANGC} -version OUTPUT_VARIABLE _aprend_v ERROR_VARIABLE _aprend_v
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
message(STATUS "Aprend shaders: slangc ${_aprend_v} (${APREND_SLANGC})")
if(WIN32)
    execute_process(COMMAND ${APREND_DXC} --version OUTPUT_VARIABLE _aprend_v OUTPUT_STRIP_TRAILING_WHITESPACE)
    message(STATUS "Aprend shaders: ${_aprend_v} (${APREND_DXC})")
endif()

# aprend_add_slang_shader(<out_var> SOURCE <file.slang> OUT_DIR <dir>
#     [INCLUDE_DIRS <dir>...] [DEPENDS <file>...] [DXIL_PROFILE <profile>]
#     [SPIRV_CAPABILITIES <cap>...] [METAL_ROUTE SLANG <reason>])
#
# SOURCE is absolute or relative to the calling directory. Its stem ends in
# _vert, _frag, _comp or _mesh, which names the package's stage; the source
# names it too ([shader("vertex")]...). INCLUDE_DIRS are where its imports
# are found, and DEPENDS the modules it imports, so a change to one builds
# it again. DXIL_PROFILE is the shader model DXC compiles for: vs_6_0,
# ps_6_0 and cs_6_0 unless given, ms_6_5 for a mesh shader. SPIR-V is 1.5,
# which Vulkan 1.2 and later take, plus the capabilities named.
# METAL_ROUTE SLANG <reason> makes the .metallib from Slang's own Metal
# source instead of spudgpu_msl's (card S21), with the reason written
# beside the call. Appends the package's
# include file to <out_var> in the caller's scope.
function(aprend_add_slang_shader out_var)
    cmake_parse_arguments(ARG "" "SOURCE;OUT_DIR;DXIL_PROFILE"
        "INCLUDE_DIRS;DEPENDS;SPIRV_CAPABILITIES;METAL_ROUTE" ${ARGN})
    get_filename_component(src ${ARG_SOURCE} ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
    # The entry point is the file's stem (aprend_blur_frag.slang:
    # aprend_blur_frag) in every format, never main, which Metal reserves.
    get_filename_component(entry ${src} NAME_WE)
    if(entry MATCHES "_vert$")
        set(stage SPUDGPU_SHADER_STAGE_VERTEX)
        set(profile vs_6_0)
    elseif(entry MATCHES "_frag$")
        set(stage SPUDGPU_SHADER_STAGE_FRAGMENT)
        set(profile ps_6_0)
    elseif(entry MATCHES "_comp$")
        set(stage SPUDGPU_SHADER_STAGE_COMPUTE)
        set(profile cs_6_0)
    elseif(entry MATCHES "_mesh$")
        set(stage SPUDGPU_SHADER_STAGE_MESH)
        set(profile ms_6_5)
    else()
        message(FATAL_ERROR "aprend_add_slang_shader: ${entry} doesn't end in _vert, _frag, _comp or _mesh")
    endif()
    if(ARG_DXIL_PROFILE)
        set(profile ${ARG_DXIL_PROFILE})
    endif()
    set(out ${ARG_OUT_DIR}/${entry})
    set(embed ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/aprend_embed.cmake)
    set(flags "")
    foreach(dir IN LISTS ARG_INCLUDE_DIRS)
        get_filename_component(dir ${dir} ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
        list(APPEND flags -I ${dir})
    endforeach()
    set(caps "")
    foreach(cap IN LISTS ARG_SPIRV_CAPABILITIES)
        list(APPEND caps -capability ${cap})
    endforeach()
    set(depends ${src} ${ARG_DEPENDS} ${embed})

    add_custom_command(
        OUTPUT ${out}.spv ${out}.spv.inc
        COMMAND ${CMAKE_COMMAND} -E make_directory ${ARG_OUT_DIR}
        COMMAND ${APREND_SLANGC} ${src} ${flags} -entry ${entry} -target spirv -profile spirv_1_5 ${caps}
                -fvk-use-entrypoint-name -matrix-layout-column-major
                -warnings-as-errors all -o ${out}.spv
        COMMAND ${APREND_SPIRV_VAL} --target-env vulkan1.2 ${out}.spv
        COMMAND ${CMAKE_COMMAND} -DIN=${out}.spv -DOUT=${out}.spv.inc -DNAME=${entry}_spirv -P ${embed}
        DEPENDS ${depends}
        COMMENT "Slang: ${entry} to SPIR-V (spirv-val)")
    set(outputs ${out}.spv.inc)
    set(includes "#include \"${entry}.spv.inc\"\n")
    set(dxil "NULL, 0")
    set(metallib "NULL, 0")

    if(WIN32)
        add_custom_command(
            OUTPUT ${out}.hlsl ${out}.dxil ${out}.dxil.inc
            COMMAND ${CMAKE_COMMAND} -E make_directory ${ARG_OUT_DIR}
            COMMAND ${APREND_SLANGC} ${src} ${flags} -entry ${entry} -target hlsl -profile ${profile}
                    -matrix-layout-column-major
                    -warnings-as-errors all -o ${out}.hlsl
            COMMAND ${APREND_DXC} -nologo -T ${profile} -E ${entry} -WX ${out}.hlsl -Fo ${out}.dxil
            COMMAND ${CMAKE_COMMAND} -DIN=${out}.dxil -DOUT=${out}.dxil.inc -DNAME=${entry}_dxil -P ${embed}
            DEPENDS ${depends}
            COMMENT "Slang: ${entry} to DXIL (DXC, validated)")
        list(APPEND outputs ${out}.dxil.inc)
        string(APPEND includes "#include \"${entry}.dxil.inc\"\n")
        set(dxil "${entry}_dxil, ${entry}_dxil_size")
    endif()

    # Both Metal routes' source, kept (card S21: which one a .metallib is
    # made from).
    if(TARGET spudgpu_msl)
        add_custom_command(
            OUTPUT ${out}.slang.metal ${out}.cross.metal
            COMMAND ${APREND_SLANGC} ${src} ${flags} -entry ${entry} -target metal -matrix-layout-column-major
                    -o ${out}.slang.metal
            COMMAND spudgpu_msl --platform $<IF:$<PLATFORM_ID:iOS>,ios,macos> --msl-version 30100
                    ${out}.spv ${out}.cross.metal
            DEPENDS ${depends} ${out}.spv spudgpu_msl
            COMMENT "Slang: ${entry} to Metal source, both routes")
        list(APPEND outputs ${out}.slang.metal ${out}.cross.metal)
    endif()

    if(APPLE)
        set(metal_src ${out}.cross.metal)
        if("SLANG" IN_LIST ARG_METAL_ROUTE)
            set(metal_src ${out}.slang.metal)
        endif()
        add_custom_command(
            OUTPUT ${out}.metallib ${out}.metallib.inc
            COMMAND xcrun -sdk $<IF:$<PLATFORM_ID:iOS>,iphoneos,macosx> metal -std=metal3.1 -Werror -c ${metal_src} -o ${out}.air
            COMMAND xcrun -sdk $<IF:$<PLATFORM_ID:iOS>,iphoneos,macosx> metallib ${out}.air -o ${out}.metallib
            COMMAND ${CMAKE_COMMAND} -DIN=${out}.metallib -DOUT=${out}.metallib.inc -DNAME=${entry}_metallib -P ${embed}
            DEPENDS ${metal_src} ${embed}
            COMMENT "Metal: ${entry} to a metallib")
        list(APPEND outputs ${out}.metallib.inc)
        string(APPEND includes "#include \"${entry}.metallib.inc\"\n")
        set(metallib "${entry}_metallib, ${entry}_metallib_size")
    endif()

    # The package, written at configure time: it only names the arrays
    # above, so it changes only when the formats this platform builds do.
    file(CONFIGURE OUTPUT ${out}.package.inc CONTENT
        "// Written at configure time by aprend_slang.cmake: not by hand.\n${includes}static const APREND_SHADER_PACKAGE ${entry}_package = {\n\t{APRI_STRUCT_TYPE_APREND_SHADER_PACKAGE, NULL},\n\t${stage},\n\t\"${entry}\",\n\t${entry}_spirv, ${entry}_spirv_size,\n\t${dxil},\n\t${metallib}\n};\n"
        @ONLY)

    set(${out_var} ${${out_var}} ${outputs} PARENT_SCOPE)
endfunction()
