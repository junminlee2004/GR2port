# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

target_sources(shadps4 PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/capture.cpp
    ${CMAKE_CURRENT_LIST_DIR}/capture.h
    ${CMAKE_CURRENT_LIST_DIR}/contours.cpp
    ${CMAKE_CURRENT_LIST_DIR}/contours.h
    ${CMAKE_CURRENT_LIST_DIR}/deformers.cpp
    ${CMAKE_CURRENT_LIST_DIR}/deformers.h
    ${CMAKE_CURRENT_LIST_DIR}/device.cpp
    ${CMAKE_CURRENT_LIST_DIR}/device.h
    ${CMAKE_CURRENT_LIST_DIR}/fault_report.cpp
    ${CMAKE_CURRENT_LIST_DIR}/fault_report.h
    ${CMAKE_CURRENT_LIST_DIR}/fog.cpp
    ${CMAKE_CURRENT_LIST_DIR}/fog.h
    ${CMAKE_CURRENT_LIST_DIR}/gr2port.cpp
    ${CMAKE_CURRENT_LIST_DIR}/gr2port.h
    ${CMAKE_CURRENT_LIST_DIR}/hooks.cpp
    ${CMAKE_CURRENT_LIST_DIR}/hooks.h
    ${CMAKE_CURRENT_LIST_DIR}/look.cpp
    ${CMAKE_CURRENT_LIST_DIR}/look.h
    ${CMAKE_CURRENT_LIST_DIR}/memory.cpp
    ${CMAKE_CURRENT_LIST_DIR}/memory.h
    ${CMAKE_CURRENT_LIST_DIR}/meshes.cpp
    ${CMAKE_CURRENT_LIST_DIR}/meshes.h
    ${CMAKE_CURRENT_LIST_DIR}/overlays.cpp
    ${CMAKE_CURRENT_LIST_DIR}/overlays.h
    ${CMAKE_CURRENT_LIST_DIR}/pipeline.cpp
    ${CMAKE_CURRENT_LIST_DIR}/pipeline.h
    ${CMAKE_CURRENT_LIST_DIR}/renderer.cpp
    ${CMAKE_CURRENT_LIST_DIR}/renderer.h
    ${CMAKE_CURRENT_LIST_DIR}/scene.h
    ${CMAKE_CURRENT_LIST_DIR}/scene_ring.cpp
    ${CMAKE_CURRENT_LIST_DIR}/scene_ring.h
    ${CMAKE_CURRENT_LIST_DIR}/shadows.cpp
    ${CMAKE_CURRENT_LIST_DIR}/shadows.h
    ${CMAKE_CURRENT_LIST_DIR}/swapchain.cpp
    ${CMAKE_CURRENT_LIST_DIR}/swapchain.h
    ${CMAKE_CURRENT_LIST_DIR}/textures.cpp
    ${CMAKE_CURRENT_LIST_DIR}/textures.h
    ${CMAKE_CURRENT_LIST_DIR}/vegetation.cpp
    ${CMAKE_CURRENT_LIST_DIR}/vegetation.h
    ${CMAKE_CURRENT_LIST_DIR}/window.cpp
    ${CMAKE_CURRENT_LIST_DIR}/window.h
    ${CMAKE_CURRENT_LIST_DIR}/world.cpp
    ${CMAKE_CURRENT_LIST_DIR}/world.h)

# The renderer's shaders, compiled like the host shaders.
if (TARGET glslang-standalone)
    set(GR2PORT_GLSLANG glslang-standalone)
elseif (TARGET glslang::glslang-standalone)
    set(GR2PORT_GLSLANG glslang::glslang-standalone)
endif()
set(GR2PORT_DIR ${CMAKE_CURRENT_LIST_DIR})
set(GR2PORT_HEADER_SCRIPT
    ${PROJECT_SOURCE_DIR}/src/video_core/host_shaders/AddShaderHeaderInclude.cmake)
set(GR2PORT_SHADER_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/gr2port_shaders/include)
set(GR2PORT_SHADER_DIR ${GR2PORT_SHADER_INCLUDE}/video_core/gr2port/shaders)
set(GR2PORT_SHADER_HEADERS "")
function(gr2port_add_shader FILENAME SHADER_NAME)
    set(HEADER_FILE ${GR2PORT_SHADER_DIR}/${SHADER_NAME}.h)
    set(DEPFILE ${HEADER_FILE}.d)
    string(TOUPPER ${SHADER_NAME} SYMBOL)
    add_custom_command(
        OUTPUT ${HEADER_FILE}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${GR2PORT_SHADER_DIR}
        COMMAND ${GR2PORT_GLSLANG} --target-env vulkan1.3
                -o ${HEADER_FILE} --vn ${SYMBOL} --depfile ${DEPFILE}
                ${GR2PORT_DIR}/shaders/${FILENAME}
        COMMAND ${CMAKE_COMMAND} "-DHEADER_FILE=${HEADER_FILE}" -P ${GR2PORT_HEADER_SCRIPT}
        DEPENDS ${GR2PORT_GLSLANG} ${GR2PORT_DIR}/shaders/${FILENAME}
                ${GR2PORT_HEADER_SCRIPT}
        DEPFILE ${DEPFILE}
        BYPRODUCTS ${DEPFILE}
        COMMENT "Building GR2port shader ${FILENAME}"
        VERBATIM)
    set(GR2PORT_SHADER_HEADERS ${GR2PORT_SHADER_HEADERS} ${HEADER_FILE} PARENT_SCOPE)
endfunction()
gr2port_add_shader(character.vert character_vert)
gr2port_add_shader(character.frag character_frag)
gr2port_add_shader(character_depth.frag character_depth_frag)
gr2port_add_shader(interior.frag interior_frag)
gr2port_add_shader(contour.vert contour_vert)
gr2port_add_shader(contour.geom contour_geom)
gr2port_add_shader(contour.frag contour_frag)
gr2port_add_shader(fullscreen.vert fullscreen_vert)
gr2port_add_shader(sky.frag sky_frag)
gr2port_add_shader(luminance.comp luminance_comp)
gr2port_add_shader(exposure.comp exposure_comp)
gr2port_add_shader(composite.frag composite_frag)
gr2port_add_shader(bloom_extract.frag bloom_extract_frag)
gr2port_add_shader(bloom_blur.frag bloom_blur_frag)
gr2port_add_shader(fog_bake.comp fog_bake_comp)
gr2port_add_shader(fog_darkness.comp fog_darkness_comp)
gr2port_add_shader(fog_density.comp fog_density_comp)
gr2port_add_shader(fog_shadow.comp fog_shadow_comp)
gr2port_add_shader(fog_scatter.comp fog_scatter_comp)
gr2port_add_shader(fog_apply.frag fog_apply_frag)
gr2port_add_shader(world.vert world_vert)
gr2port_add_shader(world.frag world_frag)
gr2port_add_shader(world_alpha.frag world_alpha_frag)
gr2port_add_shader(world_lines.comp world_lines_comp)
gr2port_add_shader(occlusion.comp occlusion_comp)
gr2port_add_shader(occlusion_depth.comp occlusion_depth_comp)
gr2port_add_shader(deferred.frag deferred_frag)
gr2port_add_shader(shadow.vert shadow_vert)
gr2port_add_shader(shadow_alpha.vert shadow_alpha_vert)
gr2port_add_shader(shadow_alpha.frag shadow_alpha_frag)
gr2port_add_shader(leaf.vert leaf_vert)
gr2port_add_shader(leaf.tesc leaf_tesc)
gr2port_add_shader(leaf.tese leaf_tese)
gr2port_add_shader(leaf.geom leaf_geom)
gr2port_add_shader(leaf.frag leaf_frag)
gr2port_add_shader(leaf_shadow.tese leaf_shadow_tese)
gr2port_add_shader(leaf_shadow.geom leaf_shadow_geom)
gr2port_add_shader(skin.comp skin_comp)
gr2port_add_shader(effector.comp effector_comp)
gr2port_add_shader(soft_body.comp soft_body_comp)
gr2port_add_shader(soft_body_write.comp soft_body_write_comp)
add_custom_target(gr2port_shaders DEPENDS ${GR2PORT_SHADER_HEADERS})
target_include_directories(shadps4 PRIVATE ${GR2PORT_SHADER_INCLUDE})
add_dependencies(shadps4 gr2port_shaders)
