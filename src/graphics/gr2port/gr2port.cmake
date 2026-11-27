# SPDX-FileCopyrightText: Copyright 2026 GR2port
# SPDX-License-Identifier: GPL-2.0-or-later

target_sources(gr2port PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/camera_menu.cpp
    ${CMAKE_CURRENT_LIST_DIR}/camera_menu.h
    ${CMAKE_CURRENT_LIST_DIR}/camera_reach.cpp
    ${CMAKE_CURRENT_LIST_DIR}/camera_reach.h
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
    ${CMAKE_CURRENT_LIST_DIR}/catcher.cpp
    ${CMAKE_CURRENT_LIST_DIR}/catcher.h
    ${CMAKE_CURRENT_LIST_DIR}/catcher_counts.cpp
    ${CMAKE_CURRENT_LIST_DIR}/catcher_counts.h
    ${CMAKE_CURRENT_LIST_DIR}/catcher_crash.cpp
    ${CMAKE_CURRENT_LIST_DIR}/catcher_crash.h
    ${CMAKE_CURRENT_LIST_DIR}/catcher_marks.cpp
    ${CMAKE_CURRENT_LIST_DIR}/catcher_marks.h
    ${CMAKE_CURRENT_LIST_DIR}/catcher_outside.cpp
    ${CMAKE_CURRENT_LIST_DIR}/catcher_outside.h
    ${CMAKE_CURRENT_LIST_DIR}/catcher_stacks.cpp
    ${CMAKE_CURRENT_LIST_DIR}/catcher_stacks.h
    ${CMAKE_CURRENT_LIST_DIR}/catcher_system.cpp
    ${CMAKE_CURRENT_LIST_DIR}/catcher_system.h
    ${CMAKE_CURRENT_LIST_DIR}/catcher_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/catcher_test.h
    ${CMAKE_CURRENT_LIST_DIR}/device_lost.cpp
    ${CMAKE_CURRENT_LIST_DIR}/device_lost.h
    ${CMAKE_CURRENT_LIST_DIR}/device.h
    ${CMAKE_CURRENT_LIST_DIR}/exit_rows.cpp
    ${CMAKE_CURRENT_LIST_DIR}/exit_rows.h
    ${CMAKE_CURRENT_LIST_DIR}/fault_report.cpp
    ${CMAKE_CURRENT_LIST_DIR}/fault_report.h
    ${CMAKE_CURRENT_LIST_DIR}/effects.cpp
    ${CMAKE_CURRENT_LIST_DIR}/effects.h
    ${CMAKE_CURRENT_LIST_DIR}/enemies.cpp
    ${CMAKE_CURRENT_LIST_DIR}/enemies.h
    ${CMAKE_CURRENT_LIST_DIR}/fog.cpp
    ${CMAKE_CURRENT_LIST_DIR}/fog.h
    ${CMAKE_CURRENT_LIST_DIR}/frame_graph_draws.cpp
    ${CMAKE_CURRENT_LIST_DIR}/frame_graph_draws.h
    ${CMAKE_CURRENT_LIST_DIR}/frame_stats.cpp
    ${CMAKE_CURRENT_LIST_DIR}/frame_stats.h
    ${CMAKE_CURRENT_LIST_DIR}/glass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/glass.h
    ${CMAKE_CURRENT_LIST_DIR}/gpu_writes.cpp
    ${CMAKE_CURRENT_LIST_DIR}/gpu_writes.h
    ${CMAKE_CURRENT_LIST_DIR}/gr2port.cpp
    ${CMAKE_CURRENT_LIST_DIR}/gr2port.h
    ${CMAKE_CURRENT_LIST_DIR}/graphics_menu.cpp
    ${CMAKE_CURRENT_LIST_DIR}/graphics_menu.h
    ${CMAKE_CURRENT_LIST_DIR}/hooks.cpp
    ${CMAKE_CURRENT_LIST_DIR}/hooks.h
    ${CMAKE_CURRENT_LIST_DIR}/imgui_relay.cpp
    ${CMAKE_CURRENT_LIST_DIR}/imgui_relay.h
    ${CMAKE_CURRENT_LIST_DIR}/look.cpp
    ${CMAKE_CURRENT_LIST_DIR}/look.h
    ${CMAKE_CURRENT_LIST_DIR}/loading_tip.cpp
    ${CMAKE_CURRENT_LIST_DIR}/loading_tip.h
    ${CMAKE_CURRENT_LIST_DIR}/memory.cpp
    ${CMAKE_CURRENT_LIST_DIR}/memory.h
    ${CMAKE_CURRENT_LIST_DIR}/menu_kit.cpp
    ${CMAKE_CURRENT_LIST_DIR}/menu_kit.h
    ${CMAKE_CURRENT_LIST_DIR}/meshes.cpp
    ${CMAKE_CURRENT_LIST_DIR}/meshes.h
    ${CMAKE_CURRENT_LIST_DIR}/metaballs.cpp
    ${CMAKE_CURRENT_LIST_DIR}/metaballs.h
    ${CMAKE_CURRENT_LIST_DIR}/mouse_camera.cpp
    ${CMAKE_CURRENT_LIST_DIR}/mouse_camera.h
    ${CMAKE_CURRENT_LIST_DIR}/movies.cpp
    ${CMAKE_CURRENT_LIST_DIR}/movies.h
    ${CMAKE_CURRENT_LIST_DIR}/overlays.cpp
    ${CMAKE_CURRENT_LIST_DIR}/overlays.h
    ${CMAKE_CURRENT_LIST_DIR}/part_hits.cpp
    ${CMAKE_CURRENT_LIST_DIR}/part_hits.h
    ${CMAKE_CURRENT_LIST_DIR}/particle_capture.cpp
    ${CMAKE_CURRENT_LIST_DIR}/particle_capture.h
    ${CMAKE_CURRENT_LIST_DIR}/motion_blur.cpp
    ${CMAKE_CURRENT_LIST_DIR}/motion_blur.h
    ${CMAKE_CURRENT_LIST_DIR}/photo.cpp
    ${CMAKE_CURRENT_LIST_DIR}/photo.h
    ${CMAKE_CURRENT_LIST_DIR}/hud_parts.cpp
    ${CMAKE_CURRENT_LIST_DIR}/hud_parts.h
    ${CMAKE_CURRENT_LIST_DIR}/photo_ui.cpp
    ${CMAKE_CURRENT_LIST_DIR}/photo_ui.h
    ${CMAKE_CURRENT_LIST_DIR}/particles.cpp
    ${CMAKE_CURRENT_LIST_DIR}/particles.h
    ${CMAKE_CURRENT_LIST_DIR}/mob_float.cpp
    ${CMAKE_CURRENT_LIST_DIR}/mob_float.h
    ${CMAKE_CURRENT_LIST_DIR}/motion_ends.cpp
    ${CMAKE_CURRENT_LIST_DIR}/motion_ends.h
    ${CMAKE_CURRENT_LIST_DIR}/gravity_kick.cpp
    ${CMAKE_CURRENT_LIST_DIR}/gravity_kick.h
    ${CMAKE_CURRENT_LIST_DIR}/gravity_slide.cpp
    ${CMAKE_CURRENT_LIST_DIR}/gravity_slide.h
    ${CMAKE_CURRENT_LIST_DIR}/comic_camera.cpp
    ${CMAKE_CURRENT_LIST_DIR}/comic_camera.h
    ${CMAKE_CURRENT_LIST_DIR}/cord_bend.cpp
    ${CMAKE_CURRENT_LIST_DIR}/cord_bend.h
    ${CMAKE_CURRENT_LIST_DIR}/layout_clock.cpp
    ${CMAKE_CURRENT_LIST_DIR}/layout_clock.h
    ${CMAKE_CURRENT_LIST_DIR}/marker_fit.cpp
    ${CMAKE_CURRENT_LIST_DIR}/marker_fit.h
    ${CMAKE_CURRENT_LIST_DIR}/scripted_moves.cpp
    ${CMAKE_CURRENT_LIST_DIR}/scripted_moves.h
    ${CMAKE_CURRENT_LIST_DIR}/frame_windows.cpp
    ${CMAKE_CURRENT_LIST_DIR}/frame_windows.h
    ${CMAKE_CURRENT_LIST_DIR}/hair_flutter.cpp
    ${CMAKE_CURRENT_LIST_DIR}/hair_flutter.h
    ${CMAKE_CURRENT_LIST_DIR}/water_splashes.cpp
    ${CMAKE_CURRENT_LIST_DIR}/water_splashes.h
    ${CMAKE_CURRENT_LIST_DIR}/smoke_and_dust.cpp
    ${CMAKE_CURRENT_LIST_DIR}/smoke_and_dust.h
    ${CMAKE_CURRENT_LIST_DIR}/smoke_effects.h
    ${CMAKE_CURRENT_LIST_DIR}/field_of_view.cpp
    ${CMAKE_CURRENT_LIST_DIR}/field_of_view.h
    ${CMAKE_CURRENT_LIST_DIR}/float_spin.cpp
    ${CMAKE_CURRENT_LIST_DIR}/float_spin.h
    ${CMAKE_CURRENT_LIST_DIR}/pass_times.cpp
    ${CMAKE_CURRENT_LIST_DIR}/pass_times.h
    ${CMAKE_CURRENT_LIST_DIR}/pipeline.cpp
    ${CMAKE_CURRENT_LIST_DIR}/presenter.cpp
    ${CMAKE_CURRENT_LIST_DIR}/presenter.h
    ${CMAKE_CURRENT_LIST_DIR}/player_control.cpp
    ${CMAKE_CURRENT_LIST_DIR}/player_control.h
    ${CMAKE_CURRENT_LIST_DIR}/pipeline.h
    ${CMAKE_CURRENT_LIST_DIR}/point_lights.cpp
    ${CMAKE_CURRENT_LIST_DIR}/point_lights.h
    ${CMAKE_CURRENT_LIST_DIR}/renderer.cpp
    ${CMAKE_CURRENT_LIST_DIR}/renderer.h
    ${CMAKE_CURRENT_LIST_DIR}/scene.h
    ${CMAKE_CURRENT_LIST_DIR}/scene_ring.cpp
    ${CMAKE_CURRENT_LIST_DIR}/scene_ring.h
    ${CMAKE_CURRENT_LIST_DIR}/screen_shake.cpp
    ${CMAKE_CURRENT_LIST_DIR}/screen_shake.h
    ${CMAKE_CURRENT_LIST_DIR}/screenshot.cpp
    ${CMAKE_CURRENT_LIST_DIR}/screenshot.h
    ${CMAKE_CURRENT_LIST_DIR}/script_timers.cpp
    ${CMAKE_CURRENT_LIST_DIR}/script_timers.h
    ${CMAKE_CURRENT_LIST_DIR}/self_translucent.cpp
    ${CMAKE_CURRENT_LIST_DIR}/self_translucent.h
    ${CMAKE_CURRENT_LIST_DIR}/settings.cpp
    ${CMAKE_CURRENT_LIST_DIR}/settings.h
    ${CMAKE_CURRENT_LIST_DIR}/effect_emitter.cpp
    ${CMAKE_CURRENT_LIST_DIR}/effect_emitter.h
    ${CMAKE_CURRENT_LIST_DIR}/shadow_distance.cpp
    ${CMAKE_CURRENT_LIST_DIR}/shadow_distance.h
    ${CMAKE_CURRENT_LIST_DIR}/shadow_trace.cpp
    ${CMAKE_CURRENT_LIST_DIR}/shadow_trace.h
    ${CMAKE_CURRENT_LIST_DIR}/shadows.cpp
    ${CMAKE_CURRENT_LIST_DIR}/shadows.h
    ${CMAKE_CURRENT_LIST_DIR}/sound_delay.cpp
    ${CMAKE_CURRENT_LIST_DIR}/sound_delay.h
    ${CMAKE_CURRENT_LIST_DIR}/splash.cpp
    ${CMAKE_CURRENT_LIST_DIR}/splash.h
    ${CMAKE_CURRENT_LIST_DIR}/special_cameras.cpp
    ${CMAKE_CURRENT_LIST_DIR}/special_cameras.h
    ${CMAKE_CURRENT_LIST_DIR}/stick_tilt.cpp
    ${CMAKE_CURRENT_LIST_DIR}/stick_tilt.h
    ${CMAKE_CURRENT_LIST_DIR}/swapchain.cpp
    ${CMAKE_CURRENT_LIST_DIR}/swapchain.h
    ${CMAKE_CURRENT_LIST_DIR}/textures.cpp
    ${CMAKE_CURRENT_LIST_DIR}/textures.h
    ${CMAKE_CURRENT_LIST_DIR}/timeline.cpp
    ${CMAKE_CURRENT_LIST_DIR}/timeline.h
    ${CMAKE_CURRENT_LIST_DIR}/ui.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ui.h
    ${CMAKE_CURRENT_LIST_DIR}/ui_capture.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ui_capture.h
    ${CMAKE_CURRENT_LIST_DIR}/ui_items.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ui_items.h
    ${CMAKE_CURRENT_LIST_DIR}/ui_items_elements.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ui_items_internal.h
    ${CMAKE_CURRENT_LIST_DIR}/ui_items_passes.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ui_items_text.cpp
    ${CMAKE_CURRENT_LIST_DIR}/upscaler.cpp
    ${CMAKE_CURRENT_LIST_DIR}/upscaler.h
    ${CMAKE_CURRENT_LIST_DIR}/vblank.cpp
    ${CMAKE_CURRENT_LIST_DIR}/vblank.h
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

# The renderer's shaders: glslang compiles each to a SPIR-V module, spirv-opt optimises the module
# for speed, and the module's words become a header. A driver's compiler can take minutes over
# a module glslang leaves as it is.
if (TARGET glslang-standalone)
    set(GR2PORT_GLSLANG glslang-standalone)
elseif (TARGET glslang::glslang-standalone)
    set(GR2PORT_GLSLANG glslang::glslang-standalone)
endif()
set(GR2PORT_DIR ${CMAKE_CURRENT_LIST_DIR})
set(GR2PORT_HEADER_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/shader_header.cmake)
set(GR2PORT_SHADER_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/gr2port_shaders/include)
set(GR2PORT_SHADER_DIR ${GR2PORT_SHADER_INCLUDE}/graphics/gr2port/shaders)
set(GR2PORT_SHADER_HEADERS "")
# Arguments after a shader's name go to glslang.
function(gr2port_add_shader FILENAME SHADER_NAME)
    set(HEADER_FILE ${GR2PORT_SHADER_DIR}/${SHADER_NAME}.h)
    set(SPIRV_FILE ${GR2PORT_SHADER_DIR}/${SHADER_NAME}.spv)
    set(DEPFILE ${SPIRV_FILE}.d)
    string(TOUPPER ${SHADER_NAME} SYMBOL)
    add_custom_command(
        OUTPUT ${SPIRV_FILE} ${HEADER_FILE}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${GR2PORT_SHADER_DIR}
        COMMAND ${GR2PORT_GLSLANG} --target-env vulkan1.3 ${ARGN}
                -o ${SPIRV_FILE} --depfile ${DEPFILE}
                ${GR2PORT_DIR}/shaders/${FILENAME}
        COMMAND spirv-opt -O ${SPIRV_FILE} -o ${SPIRV_FILE}
        COMMAND ${CMAKE_COMMAND} "-DSPIRV_FILE=${SPIRV_FILE}" "-DHEADER_FILE=${HEADER_FILE}"
                "-DSYMBOL=${SYMBOL}" -P ${GR2PORT_HEADER_SCRIPT}
        DEPENDS ${GR2PORT_GLSLANG} spirv-opt ${GR2PORT_DIR}/shaders/${FILENAME}
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
gr2port_add_shader(particle_active.comp particle_active_comp)
gr2port_add_shader(particle_step.comp particle_step_comp)
gr2port_add_shader(particle_half.frag particle_half_frag)
gr2port_add_shader(antialias_off.comp antialias_off_comp)
gr2port_add_shader(upscale_sharpen.comp upscale_sharpen_comp)
gr2port_add_shader(collision_backup.comp collision_backup_comp)
gr2port_add_shader(exposure.comp exposure_comp)
gr2port_add_shader(composite.frag composite_frag)
gr2port_add_shader(composite_hdr.frag composite_hdr_frag)
gr2port_add_shader(tonemap.frag tonemap_frag)
gr2port_add_shader(antialias.comp antialias_comp)
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
# The programs with a per-draw loop once more, for drivers with
# VK_KHR_shader_maximal_reconvergence.
gr2port_add_shader(character.frag character_reconverging_frag -DMAXIMAL_RECONVERGENCE)
gr2port_add_shader(character_depth.frag character_depth_reconverging_frag -DMAXIMAL_RECONVERGENCE)
gr2port_add_shader(world.frag world_reconverging_frag -DMAXIMAL_RECONVERGENCE)
gr2port_add_shader(world_alpha.frag world_alpha_reconverging_frag -DMAXIMAL_RECONVERGENCE)
# The programs whose loops have limits once more, for the GPU Crash Catcher: they count the limits
# they reach.
gr2port_add_shader(character.frag character_catcher_frag -DCATCHER)
gr2port_add_shader(character.frag character_reconverging_catcher_frag -DMAXIMAL_RECONVERGENCE
                   -DCATCHER)
gr2port_add_shader(character_depth.frag character_depth_catcher_frag -DCATCHER)
gr2port_add_shader(character_depth.frag character_depth_reconverging_catcher_frag
                   -DMAXIMAL_RECONVERGENCE -DCATCHER)
gr2port_add_shader(world.frag world_catcher_frag -DCATCHER)
gr2port_add_shader(world.frag world_reconverging_catcher_frag -DMAXIMAL_RECONVERGENCE -DCATCHER)
gr2port_add_shader(world_alpha.frag world_alpha_catcher_frag -DCATCHER)
gr2port_add_shader(world_alpha.frag world_alpha_reconverging_catcher_frag -DMAXIMAL_RECONVERGENCE
                   -DCATCHER)
gr2port_add_shader(gpu_emitter_spawn.comp gpu_emitter_spawn_catcher_comp -DCATCHER)
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
gr2port_add_shader(character_glass.frag character_glass_frag)
gr2port_add_shader(character_glass.vert character_glass_vert)
gr2port_add_shader(character_glass_skinned.vert character_glass_skinned_vert)
gr2port_add_shader(effect.frag effect_frag)
gr2port_add_shader(effect_skinned.vert effect_skinned_vert)
gr2port_add_shader(effect_unskinned.vert effect_unskinned_vert)
gr2port_add_shader(enemy.frag enemy_frag)
gr2port_add_shader(enemy.vert enemy_vert)
gr2port_add_shader(enemy_boss.frag enemy_boss_frag)
gr2port_add_shader(enemy_boss.vert enemy_boss_vert)
gr2port_add_shader(enemy_boss_skinned.vert enemy_boss_skinned_vert)
gr2port_add_shader(enemy_core.frag enemy_core_frag)
gr2port_add_shader(enemy_core_alpha.frag enemy_core_alpha_frag)
gr2port_add_shader(enemy_core.vert enemy_core_vert)
gr2port_add_shader(enemy_core_skinned.vert enemy_core_skinned_vert)
gr2port_add_shader(enemy_skinned.vert enemy_skinned_vert)
gr2port_add_shader(shadow_trace.frag shadow_trace_frag)
gr2port_add_shader(motion_blur.frag motion_blur_frag)
gr2port_add_shader(motion_blur_compress.comp motion_blur_compress_comp)
gr2port_add_shader(motion_blur.vert motion_blur_vert)
gr2port_add_shader(motion_blur_tiles.vert motion_blur_tiles_vert)
gr2port_add_shader(motion_blur_copy.frag motion_blur_copy_frag)
gr2port_add_shader(motion_blur_tile.comp motion_blur_tile_comp)
gr2port_add_shader(motion_blur_neighbor.comp motion_blur_neighbor_comp)
gr2port_add_shader(water.frag water_frag)
gr2port_add_shader(water.vert water_vert)
gr2port_add_shader(gpu_emitter_spawn.comp gpu_emitter_spawn_comp)
gr2port_add_shader(gpu_object_update.comp gpu_object_update_comp)
gr2port_add_shader(gpu_particle_clear.comp gpu_particle_clear_comp)
gr2port_add_shader(gpu_particle_move.comp gpu_particle_move_comp)
gr2port_add_shader(gpu_sample_mesh.comp gpu_sample_mesh_comp)
gr2port_add_shader(gpu_sample_skinned_mesh.comp gpu_sample_skinned_mesh_comp)
gr2port_add_shader(model.vert model_vert)
gr2port_add_shader(model_blur.frag model_blur_frag)
gr2port_add_shader(model_blur_soft.frag model_blur_soft_frag)
gr2port_add_shader(model_distortion.frag model_distortion_frag)
gr2port_add_shader(model_distortion_soft.frag model_distortion_soft_frag)
gr2port_add_shader(model_fog.frag model_fog_frag)
gr2port_add_shader(model_fog_lit.frag model_fog_lit_frag)
gr2port_add_shader(model_fog_lit_soft.frag model_fog_lit_soft_frag)
gr2port_add_shader(model_fog_soft.frag model_fog_soft_frag)
gr2port_add_shader(model_lit.frag model_lit_frag)
gr2port_add_shader(model_lit_soft.frag model_lit_soft_frag)
gr2port_add_shader(model_plain.frag model_plain_frag)
gr2port_add_shader(model_plain_soft.frag model_plain_soft_frag)
gr2port_add_shader(model_shadow.vert model_shadow_vert)
gr2port_add_shader(sprite_gbuffer.frag sprite_gbuffer_frag)
gr2port_add_shader(sprite_gbuffer_soft.frag sprite_gbuffer_soft_frag)
gr2port_add_shader(model_gbuffer.frag model_gbuffer_frag)
gr2port_add_shader(model_gbuffer_soft.frag model_gbuffer_soft_frag)
gr2port_add_shader(particle_effect.frag particle_effect_frag)
gr2port_add_shader(particle_effect.vert particle_effect_vert)
gr2port_add_shader(particle_effect_shadow.frag particle_effect_shadow_frag)
gr2port_add_shader(particle_effect_shadow.vert particle_effect_shadow_vert)
gr2port_add_shader(particle_gem.frag particle_gem_frag)
gr2port_add_shader(particle_gem.vert particle_gem_vert)
gr2port_add_shader(particle_gem_shadow.vert particle_gem_shadow_vert)
gr2port_add_shader(particle_mesh_shadow.frag particle_mesh_shadow_frag)
gr2port_add_shader(particle_surface.frag particle_surface_frag)
gr2port_add_shader(particle_surface.vert particle_surface_vert)
gr2port_add_shader(particle_surface_shadow.vert particle_surface_shadow_vert)
gr2port_add_shader(particle_surface_color.frag particle_surface_color_frag)
gr2port_add_shader(particle_surface_color.vert particle_surface_color_vert)
gr2port_add_shader(particle_surface_tangent.frag particle_surface_tangent_frag)
gr2port_add_shader(particle_surface_tangent.vert particle_surface_tangent_vert)
gr2port_add_shader(particle_surface_tangent_color.frag particle_surface_tangent_color_frag)
gr2port_add_shader(particle_surface_tangent_color.vert particle_surface_tangent_color_vert)
gr2port_add_shader(sprite.vert sprite_vert)
gr2port_add_shader(sprite_blur.frag sprite_blur_frag)
gr2port_add_shader(sprite_blur_soft.frag sprite_blur_soft_frag)
gr2port_add_shader(sprite_distortion.frag sprite_distortion_frag)
gr2port_add_shader(sprite_distortion_soft.frag sprite_distortion_soft_frag)
gr2port_add_shader(sprite_ribbon.vert sprite_ribbon_vert)
gr2port_add_shader(sprite_ribbon_shadow.vert sprite_ribbon_shadow_vert)
gr2port_add_shader(sprite_shadow.frag sprite_shadow_frag)
gr2port_add_shader(sprite_shadow.vert sprite_shadow_vert)
gr2port_add_shader(sprite_shadow_soft.frag sprite_shadow_soft_frag)
gr2port_add_shader(sprite_fog.frag sprite_fog_frag)
gr2port_add_shader(sprite_fog_lit.frag sprite_fog_lit_frag)
gr2port_add_shader(sprite_fog_lit_soft.frag sprite_fog_lit_soft_frag)
gr2port_add_shader(sprite_fog_soft.frag sprite_fog_soft_frag)
gr2port_add_shader(sprite_lit.frag sprite_lit_frag)
gr2port_add_shader(sprite_lit_soft.frag sprite_lit_soft_frag)
gr2port_add_shader(sprite_plain.frag sprite_plain_frag)
gr2port_add_shader(sprite_plain_soft.frag sprite_plain_soft_frag)
gr2port_add_shader(color_backup.comp color_backup_comp)
gr2port_add_shader(emitter_color.comp emitter_color_comp)
gr2port_add_shader(metaball.frag metaball_frag)
gr2port_add_shader(metaball.vert metaball_vert)
gr2port_add_shader(metaball_skinned.vert metaball_skinned_vert)
gr2port_add_shader(surface_blur.frag surface_blur_frag)
gr2port_add_shader(ui_master.vert ui_master_vert)
gr2port_add_shader(ui_pass.vert ui_pass_vert)
gr2port_add_shader(ui_pass_flat.vert ui_pass_flat_vert)
gr2port_add_shader(ui_skinned.vert ui_skinned_vert)
gr2port_add_shader(ui_skinned_b.vert ui_skinned_b_vert)
gr2port_add_shader(ui_transform.vert ui_transform_vert)
gr2port_add_shader(ui_transform_b.vert ui_transform_b_vert)
gr2port_add_shader(ui_stroke.vert ui_stroke_vert)
gr2port_add_shader(ui_stroke.geom ui_stroke_geom)
gr2port_add_shader(ui_blur.frag ui_blur_frag)
gr2port_add_shader(ui_blur_history.frag ui_blur_history_frag)
gr2port_add_shader(ui_color.frag ui_color_frag)
gr2port_add_shader(ui_color_control.frag ui_color_control_frag)
gr2port_add_shader(ui_photo.frag ui_photo_frag)
gr2port_add_shader(ui_photo.vert ui_photo_vert)
gr2port_add_shader(ui_copy.frag ui_copy_frag)
gr2port_add_shader(ui_frame.frag ui_frame_frag)
gr2port_add_shader(ui_gamma.frag ui_gamma_frag)
gr2port_add_shader(ui_mask.frag ui_mask_frag)
gr2port_add_shader(ui_mask_sprite.frag ui_mask_sprite_frag)
gr2port_add_shader(ui_masked.frag ui_masked_frag)
gr2port_add_shader(ui_movie.frag ui_movie_frag)
gr2port_add_shader(ui_sprite.frag ui_sprite_frag)
gr2port_add_shader(ui_sprite_matrix.frag ui_sprite_matrix_frag)
gr2port_add_shader(ui_stroke.frag ui_stroke_frag)
gr2port_add_shader(ui_threshold.frag ui_threshold_frag)
gr2port_add_shader(ui_present.frag ui_present_frag)
gr2port_add_shader(ui_present_hdr.frag ui_present_hdr_frag)
gr2port_add_shader(ui_text.geom ui_text_geom)
gr2port_add_shader(ui_text_flat.geom ui_text_flat_geom)
gr2port_add_shader(imgui_relay.vert imgui_relay_vert)
gr2port_add_shader(imgui_relay.frag imgui_relay_frag)
gr2port_add_shader(imgui_relay_hdr.frag imgui_relay_hdr_frag)
gr2port_add_shader(skin.comp skin_comp)
gr2port_add_shader(effector.comp effector_comp)
gr2port_add_shader(soft_body.comp soft_body_comp)
gr2port_add_shader(soft_body_write.comp soft_body_write_comp)
add_custom_target(gr2port_shaders DEPENDS ${GR2PORT_SHADER_HEADERS})
target_include_directories(gr2port PRIVATE ${GR2PORT_SHADER_INCLUDE})
add_dependencies(gr2port gr2port_shaders)
