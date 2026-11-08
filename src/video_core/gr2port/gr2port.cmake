# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

target_sources(shadps4 PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/capture.cpp
    ${CMAKE_CURRENT_LIST_DIR}/capture.h
    ${CMAKE_CURRENT_LIST_DIR}/capture_math.h
    ${CMAKE_CURRENT_LIST_DIR}/character_glass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/character_glass.h
    ${CMAKE_CURRENT_LIST_DIR}/contours.cpp
    ${CMAKE_CURRENT_LIST_DIR}/contours.h
    ${CMAKE_CURRENT_LIST_DIR}/deformers.cpp
    ${CMAKE_CURRENT_LIST_DIR}/deformers.h
    ${CMAKE_CURRENT_LIST_DIR}/device.cpp
    ${CMAKE_CURRENT_LIST_DIR}/device_lost.cpp
    ${CMAKE_CURRENT_LIST_DIR}/device_lost.h
    ${CMAKE_CURRENT_LIST_DIR}/device.h
    ${CMAKE_CURRENT_LIST_DIR}/emulated_gpu.cpp
    ${CMAKE_CURRENT_LIST_DIR}/emulated_gpu.h
    ${CMAKE_CURRENT_LIST_DIR}/fault_report.cpp
    ${CMAKE_CURRENT_LIST_DIR}/fault_report.h
    ${CMAKE_CURRENT_LIST_DIR}/effects.cpp
    ${CMAKE_CURRENT_LIST_DIR}/effects.h
    ${CMAKE_CURRENT_LIST_DIR}/enemies.cpp
    ${CMAKE_CURRENT_LIST_DIR}/enemies.h
    ${CMAKE_CURRENT_LIST_DIR}/fog.cpp
    ${CMAKE_CURRENT_LIST_DIR}/fog.h
    ${CMAKE_CURRENT_LIST_DIR}/glass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/glass.h
    ${CMAKE_CURRENT_LIST_DIR}/gr2port.cpp
    ${CMAKE_CURRENT_LIST_DIR}/gr2port.h
    ${CMAKE_CURRENT_LIST_DIR}/hooks.cpp
    ${CMAKE_CURRENT_LIST_DIR}/hooks.h
    ${CMAKE_CURRENT_LIST_DIR}/imgui_relay.cpp
    ${CMAKE_CURRENT_LIST_DIR}/imgui_relay.h
    ${CMAKE_CURRENT_LIST_DIR}/look.cpp
    ${CMAKE_CURRENT_LIST_DIR}/look.h
    ${CMAKE_CURRENT_LIST_DIR}/memory.cpp
    ${CMAKE_CURRENT_LIST_DIR}/memory.h
    ${CMAKE_CURRENT_LIST_DIR}/meshes.cpp
    ${CMAKE_CURRENT_LIST_DIR}/meshes.h
    ${CMAKE_CURRENT_LIST_DIR}/metaballs.cpp
    ${CMAKE_CURRENT_LIST_DIR}/metaballs.h
    ${CMAKE_CURRENT_LIST_DIR}/movies.cpp
    ${CMAKE_CURRENT_LIST_DIR}/movies.h
    ${CMAKE_CURRENT_LIST_DIR}/overlays.cpp
    ${CMAKE_CURRENT_LIST_DIR}/overlays.h
    ${CMAKE_CURRENT_LIST_DIR}/particle_capture.cpp
    ${CMAKE_CURRENT_LIST_DIR}/particle_capture.h
    ${CMAKE_CURRENT_LIST_DIR}/particles.cpp
    ${CMAKE_CURRENT_LIST_DIR}/particles.h
    ${CMAKE_CURRENT_LIST_DIR}/pipeline.cpp
    ${CMAKE_CURRENT_LIST_DIR}/pipeline.h
    ${CMAKE_CURRENT_LIST_DIR}/point_lights.cpp
    ${CMAKE_CURRENT_LIST_DIR}/point_lights.h
    ${CMAKE_CURRENT_LIST_DIR}/renderer.cpp
    ${CMAKE_CURRENT_LIST_DIR}/renderer.h
    ${CMAKE_CURRENT_LIST_DIR}/scene.h
    ${CMAKE_CURRENT_LIST_DIR}/scene_ring.cpp
    ${CMAKE_CURRENT_LIST_DIR}/scene_ring.h
    ${CMAKE_CURRENT_LIST_DIR}/self_translucent.cpp
    ${CMAKE_CURRENT_LIST_DIR}/self_translucent.h
    ${CMAKE_CURRENT_LIST_DIR}/shadows.cpp
    ${CMAKE_CURRENT_LIST_DIR}/shadows.h
    ${CMAKE_CURRENT_LIST_DIR}/swapchain.cpp
    ${CMAKE_CURRENT_LIST_DIR}/swapchain.h
    ${CMAKE_CURRENT_LIST_DIR}/textures.cpp
    ${CMAKE_CURRENT_LIST_DIR}/textures.h
    ${CMAKE_CURRENT_LIST_DIR}/ui.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ui.h
    ${CMAKE_CURRENT_LIST_DIR}/ui_capture.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ui_capture.h
    ${CMAKE_CURRENT_LIST_DIR}/vegetation.cpp
    ${CMAKE_CURRENT_LIST_DIR}/velocity.cpp
    ${CMAKE_CURRENT_LIST_DIR}/velocity.h
    ${CMAKE_CURRENT_LIST_DIR}/vegetation.h
    ${CMAKE_CURRENT_LIST_DIR}/window.cpp
    ${CMAKE_CURRENT_LIST_DIR}/window.h
    ${CMAKE_CURRENT_LIST_DIR}/water.cpp
    ${CMAKE_CURRENT_LIST_DIR}/water.h
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
gr2port_add_shader(self_translucent.vert self_translucent_vert)
gr2port_add_shader(self_translucent.frag self_translucent_frag)
gr2port_add_shader(character.vert character_vert)
gr2port_add_shader(character.frag character_frag)
gr2port_add_shader(character_depth.frag character_depth_frag)
gr2port_add_shader(interior.frag interior_frag)
gr2port_add_shader(lambert.vert lambert_vert)
gr2port_add_shader(lambert.frag lambert_frag)
gr2port_add_shader(vision.frag vision_frag)
gr2port_add_shader(bloom_depth.frag bloom_depth_frag)
gr2port_add_shader(contour.vert contour_vert)
gr2port_add_shader(contour.geom contour_geom)
gr2port_add_shader(contour.frag contour_frag)
gr2port_add_shader(fullscreen.vert fullscreen_vert)
gr2port_add_shader(sky.frag sky_frag)
gr2port_add_shader(luminance.comp luminance_comp)
gr2port_add_shader(light_grid.comp light_grid_comp)
gr2port_add_shader(pfx/collision_backup.comp collision_backup_comp)
gr2port_add_shader(exposure.comp exposure_comp)
gr2port_add_shader(composite.frag composite_frag)
gr2port_add_shader(tonemap.frag tonemap_frag)
gr2port_add_shader(pfx/antialias.comp antialias_comp)
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
gr2port_add_shader(world_decal.vert world_decal_vert)
gr2port_add_shader(world_decal.frag world_decal_frag)
gr2port_add_shader(glass.vert glass_vert)
gr2port_add_shader(glass.frag glass_frag)
gr2port_add_shader(world_lines.comp world_lines_comp)
gr2port_add_shader(occlusion.comp occlusion_comp)
gr2port_add_shader(velocity.comp velocity_comp)
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
gr2port_add_shader(grass.comp grass_comp)
gr2port_add_shader(grass.vert grass_vert)
gr2port_add_shader(grass.frag grass_frag)
gr2port_add_shader(branch.comp branch_comp)
gr2port_add_shader(branch.vert branch_vert)
gr2port_add_shader(branch.frag branch_frag)
gr2port_add_shader(pfx/character_glass.frag character_glass_frag)
gr2port_add_shader(pfx/character_glass.vert character_glass_vert)
gr2port_add_shader(pfx/character_glass_skinned.vert character_glass_skinned_vert)
gr2port_add_shader(pfx/effect.frag effect_frag)
gr2port_add_shader(pfx/effect_skinned.vert effect_skinned_vert)
gr2port_add_shader(pfx/effect_unskinned.vert effect_unskinned_vert)
gr2port_add_shader(pfx/enemy.frag enemy_frag)
gr2port_add_shader(pfx/enemy.vert enemy_vert)
gr2port_add_shader(pfx/enemy_boss.frag enemy_boss_frag)
gr2port_add_shader(pfx/enemy_boss.vert enemy_boss_vert)
gr2port_add_shader(pfx/enemy_boss_skinned.vert enemy_boss_skinned_vert)
gr2port_add_shader(pfx/enemy_core.frag enemy_core_frag)
gr2port_add_shader(pfx/enemy_core.vert enemy_core_vert)
gr2port_add_shader(pfx/enemy_core_skinned.vert enemy_core_skinned_vert)
gr2port_add_shader(pfx/enemy_skinned.vert enemy_skinned_vert)
gr2port_add_shader(pfx/water.frag water_frag)
gr2port_add_shader(pfx/water.vert water_vert)
gr2port_add_shader(pfx/gpu_emitter_spawn.comp gpu_emitter_spawn_comp)
gr2port_add_shader(pfx/gpu_object_update.comp gpu_object_update_comp)
gr2port_add_shader(pfx/gpu_particle_clear.comp gpu_particle_clear_comp)
gr2port_add_shader(pfx/gpu_particle_move.comp gpu_particle_move_comp)
gr2port_add_shader(pfx/gpu_sample_mesh.comp gpu_sample_mesh_comp)
gr2port_add_shader(pfx/gpu_sample_skinned_mesh.comp gpu_sample_skinned_mesh_comp)
gr2port_add_shader(pfx/model.vert model_vert)
gr2port_add_shader(pfx/model_blur.frag model_blur_frag)
gr2port_add_shader(pfx/model_blur_soft.frag model_blur_soft_frag)
gr2port_add_shader(pfx/model_distortion.frag model_distortion_frag)
gr2port_add_shader(pfx/model_distortion_soft.frag model_distortion_soft_frag)
gr2port_add_shader(pfx/model_fog.frag model_fog_frag)
gr2port_add_shader(pfx/model_fog_lit.frag model_fog_lit_frag)
gr2port_add_shader(pfx/model_fog_lit_soft.frag model_fog_lit_soft_frag)
gr2port_add_shader(pfx/model_fog_soft.frag model_fog_soft_frag)
gr2port_add_shader(pfx/model_lit.frag model_lit_frag)
gr2port_add_shader(pfx/model_lit_soft.frag model_lit_soft_frag)
gr2port_add_shader(pfx/model_plain.frag model_plain_frag)
gr2port_add_shader(pfx/model_plain_soft.frag model_plain_soft_frag)
gr2port_add_shader(pfx/model_shadow.vert model_shadow_vert)
gr2port_add_shader(pfx/particle_surface.frag particle_surface_frag)
gr2port_add_shader(pfx/particle_surface.vert particle_surface_vert)
gr2port_add_shader(pfx/particle_surface_color.frag particle_surface_color_frag)
gr2port_add_shader(pfx/particle_surface_color.vert particle_surface_color_vert)
gr2port_add_shader(pfx/particle_surface_tangent.frag particle_surface_tangent_frag)
gr2port_add_shader(pfx/particle_surface_tangent.vert particle_surface_tangent_vert)
gr2port_add_shader(pfx/particle_surface_tangent_color.frag particle_surface_tangent_color_frag)
gr2port_add_shader(pfx/particle_surface_tangent_color.vert particle_surface_tangent_color_vert)
gr2port_add_shader(pfx/sprite.vert sprite_vert)
gr2port_add_shader(pfx/sprite_blur.frag sprite_blur_frag)
gr2port_add_shader(pfx/sprite_blur_soft.frag sprite_blur_soft_frag)
gr2port_add_shader(pfx/sprite_distortion.frag sprite_distortion_frag)
gr2port_add_shader(pfx/sprite_distortion_soft.frag sprite_distortion_soft_frag)
gr2port_add_shader(pfx/sprite_ribbon.vert sprite_ribbon_vert)
gr2port_add_shader(pfx/sprite_shadow.frag sprite_shadow_frag)
gr2port_add_shader(pfx/sprite_shadow.vert sprite_shadow_vert)
gr2port_add_shader(pfx/sprite_shadow_soft.frag sprite_shadow_soft_frag)
gr2port_add_shader(pfx/sprite_fog.frag sprite_fog_frag)
gr2port_add_shader(pfx/sprite_fog_lit.frag sprite_fog_lit_frag)
gr2port_add_shader(pfx/sprite_fog_lit_soft.frag sprite_fog_lit_soft_frag)
gr2port_add_shader(pfx/sprite_fog_soft.frag sprite_fog_soft_frag)
gr2port_add_shader(pfx/sprite_lit.frag sprite_lit_frag)
gr2port_add_shader(pfx/sprite_lit_soft.frag sprite_lit_soft_frag)
gr2port_add_shader(pfx/sprite_plain.frag sprite_plain_frag)
gr2port_add_shader(pfx/sprite_plain_soft.frag sprite_plain_soft_frag)
gr2port_add_shader(pfx/color_backup.comp color_backup_comp)
gr2port_add_shader(pfx/emitter_color.comp emitter_color_comp)
gr2port_add_shader(pfx/metaball.frag metaball_frag)
gr2port_add_shader(pfx/metaball.vert metaball_vert)
gr2port_add_shader(pfx/metaball_skinned.vert metaball_skinned_vert)
gr2port_add_shader(pfx/surface_blur.frag surface_blur_frag)
gr2port_add_shader(pfx/ui_master.vert ui_master_vert)
gr2port_add_shader(pfx/ui_pass.vert ui_pass_vert)
gr2port_add_shader(pfx/ui_pass_flat.vert ui_pass_flat_vert)
gr2port_add_shader(pfx/ui_skinned.vert ui_skinned_vert)
gr2port_add_shader(pfx/ui_skinned_b.vert ui_skinned_b_vert)
gr2port_add_shader(pfx/ui_transform.vert ui_transform_vert)
gr2port_add_shader(pfx/ui_transform_b.vert ui_transform_b_vert)
gr2port_add_shader(pfx/ui_blur.frag ui_blur_frag)
gr2port_add_shader(pfx/ui_blur_history.frag ui_blur_history_frag)
gr2port_add_shader(pfx/ui_color.frag ui_color_frag)
gr2port_add_shader(pfx/ui_color_control.frag ui_color_control_frag)
gr2port_add_shader(pfx/ui_copy.frag ui_copy_frag)
gr2port_add_shader(pfx/ui_frame.frag ui_frame_frag)
gr2port_add_shader(pfx/ui_gamma.frag ui_gamma_frag)
gr2port_add_shader(pfx/ui_masked.frag ui_masked_frag)
gr2port_add_shader(pfx/ui_movie.frag ui_movie_frag)
gr2port_add_shader(pfx/ui_sprite.frag ui_sprite_frag)
gr2port_add_shader(pfx/ui_threshold.frag ui_threshold_frag)
gr2port_add_shader(ui_present.frag ui_present_frag)
gr2port_add_shader(imgui_relay.vert imgui_relay_vert)
gr2port_add_shader(imgui_relay.frag imgui_relay_frag)
gr2port_add_shader(skin.comp skin_comp)
gr2port_add_shader(effector.comp effector_comp)
gr2port_add_shader(soft_body.comp soft_body_comp)
gr2port_add_shader(soft_body_write.comp soft_body_write_comp)
add_custom_target(gr2port_shaders DEPENDS ${GR2PORT_SHADER_HEADERS})
target_include_directories(shadps4 PRIVATE ${GR2PORT_SHADER_INCLUDE})
add_dependencies(shadps4 gr2port_shaders)
