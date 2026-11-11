# SPDX-FileCopyrightText: Copyright 2026 GR2port
# SPDX-License-Identifier: GPL-2.0-or-later

target_sources(gr2port PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/game_check.cpp
    ${CMAKE_CURRENT_LIST_DIR}/game_check.h
    ${CMAKE_CURRENT_LIST_DIR}/game_patches.cpp
    ${CMAKE_CURRENT_LIST_DIR}/game_patches.h
    ${CMAKE_CURRENT_LIST_DIR}/trophy_list.cpp
    ${CMAKE_CURRENT_LIST_DIR}/trophy_list.h)

# The Gravity Rush 2 patches, with their on and off flags, are part of the executable.
cmrc_add_resource_library(gr2port-patches
    ALIAS res::gr2port_patches
    NAMESPACE gr2port_patches
    src/resources/patches/GravityDaze2-Orbis.xml)
target_link_libraries(gr2port PRIVATE res::gr2port_patches)

# GR2Launcher's GR2 packs: button prompts and input presets for each pad type, and the Halloween
# photo items. The executable writes them to the user folder before the game starts.
file(GLOB_RECURSE GR2PORT_PROMPT_MODS RELATIVE ${CMAKE_SOURCE_DIR} CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/resources/prompt_mods/GR2/*)
cmrc_add_resource_library(gr2port-prompt-mods
    ALIAS res::gr2port_prompt_mods
    NAMESPACE gr2port_prompt_mods
    ${GR2PORT_PROMPT_MODS})
target_link_libraries(gr2port PRIVATE res::gr2port_prompt_mods)
