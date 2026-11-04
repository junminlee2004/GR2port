# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

target_sources(shadps4 PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/gr2_pt.cpp
    ${CMAKE_CURRENT_LIST_DIR}/gr2_pt.h
    ${CMAKE_CURRENT_LIST_DIR}/hooks.cpp
    ${CMAKE_CURRENT_LIST_DIR}/hooks.h
    ${CMAKE_CURRENT_LIST_DIR}/marker.cpp
    ${CMAKE_CURRENT_LIST_DIR}/marker.h
    ${CMAKE_CURRENT_LIST_DIR}/pass_timing.cpp
    ${CMAKE_CURRENT_LIST_DIR}/pass_timing.h
    ${CMAKE_CURRENT_LIST_DIR}/shadow_pass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/shadow_pass.h)
