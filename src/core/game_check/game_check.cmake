# SPDX-FileCopyrightText: Copyright 2026 GR2port
# SPDX-License-Identifier: GPL-2.0-or-later

target_sources(shadps4 PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/game_check.cpp
    ${CMAKE_CURRENT_LIST_DIR}/game_check.h
    ${CMAKE_CURRENT_LIST_DIR}/game_patches.cpp
    ${CMAKE_CURRENT_LIST_DIR}/game_patches.h)

# The Gravity Rush 2 patches, with their on and off flags, are part of the executable.
cmrc_add_resource_library(gr2port-patches
    ALIAS res::gr2port_patches
    NAMESPACE gr2port_patches
    src/resources/patches/GravityDaze2-Orbis.xml)
target_link_libraries(shadps4 PRIVATE res::gr2port_patches)
