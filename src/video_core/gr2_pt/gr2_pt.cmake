# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

target_sources(shadps4 PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/device.cpp
    ${CMAKE_CURRENT_LIST_DIR}/device.h
    ${CMAKE_CURRENT_LIST_DIR}/gr2_pt.cpp
    ${CMAKE_CURRENT_LIST_DIR}/gr2_pt.h
    ${CMAKE_CURRENT_LIST_DIR}/hooks.cpp
    ${CMAKE_CURRENT_LIST_DIR}/hooks.h
    ${CMAKE_CURRENT_LIST_DIR}/renderer.cpp
    ${CMAKE_CURRENT_LIST_DIR}/renderer.h
    ${CMAKE_CURRENT_LIST_DIR}/swapchain.cpp
    ${CMAKE_CURRENT_LIST_DIR}/swapchain.h
    ${CMAKE_CURRENT_LIST_DIR}/window.cpp
    ${CMAKE_CURRENT_LIST_DIR}/window.h)

# The renderer's shaders, compiled like the host shaders.
if (TARGET glslang-standalone)
    set(GR2_PT_GLSLANG glslang-standalone)
elseif (TARGET glslang::glslang-standalone)
    set(GR2_PT_GLSLANG glslang::glslang-standalone)
endif()
set(GR2_PT_DIR ${CMAKE_CURRENT_LIST_DIR})
set(GR2_PT_HEADER_SCRIPT
    ${PROJECT_SOURCE_DIR}/src/video_core/host_shaders/AddShaderHeaderInclude.cmake)
set(GR2_PT_SHADER_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/gr2_pt_shaders/include)
set(GR2_PT_SHADER_DIR ${GR2_PT_SHADER_INCLUDE}/video_core/gr2_pt/shaders)
set(GR2_PT_SHADER_HEADERS "")
function(gr2_pt_add_shader FILENAME SHADER_NAME)
    set(HEADER_FILE ${GR2_PT_SHADER_DIR}/${SHADER_NAME}.h)
    set(DEPFILE ${HEADER_FILE}.d)
    string(TOUPPER ${SHADER_NAME} SYMBOL)
    add_custom_command(
        OUTPUT ${HEADER_FILE}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${GR2_PT_SHADER_DIR}
        COMMAND ${GR2_PT_GLSLANG} --target-env vulkan1.3
                -o ${HEADER_FILE} --vn ${SYMBOL} --depfile ${DEPFILE}
                ${GR2_PT_DIR}/shaders/${FILENAME}
        COMMAND ${CMAKE_COMMAND} "-DHEADER_FILE=${HEADER_FILE}" -P ${GR2_PT_HEADER_SCRIPT}
        DEPENDS ${GR2_PT_GLSLANG} ${GR2_PT_DIR}/shaders/${FILENAME}
                ${GR2_PT_HEADER_SCRIPT}
        DEPFILE ${DEPFILE}
        BYPRODUCTS ${DEPFILE}
        COMMENT "Building GR2 PT shader ${FILENAME}"
        VERBATIM)
    set(GR2_PT_SHADER_HEADERS ${GR2_PT_SHADER_HEADERS} ${HEADER_FILE} PARENT_SCOPE)
endfunction()
add_custom_target(gr2_pt_shaders DEPENDS ${GR2_PT_SHADER_HEADERS})
target_include_directories(shadps4 PRIVATE ${GR2_PT_SHADER_INCLUDE})
add_dependencies(shadps4 gr2_pt_shaders)
