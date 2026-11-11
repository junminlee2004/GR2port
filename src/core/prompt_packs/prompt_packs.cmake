# SPDX-FileCopyrightText: Copyright 2026 GR2port
# SPDX-License-Identifier: GPL-2.0-or-later

target_sources(gr2port PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/gnf.cpp
    ${CMAKE_CURRENT_LIST_DIR}/gnf.h
    ${CMAKE_CURRENT_LIST_DIR}/keyboard_bindings.cpp
    ${CMAKE_CURRENT_LIST_DIR}/keyboard_prompts.cpp
    ${CMAKE_CURRENT_LIST_DIR}/keyboard_prompts.h
    ${CMAKE_CURRENT_LIST_DIR}/keyboard_symbols.h
    ${CMAKE_CURRENT_LIST_DIR}/pad_prompts.cpp
    ${CMAKE_CURRENT_LIST_DIR}/pad_prompts.h
    ${CMAKE_CURRENT_LIST_DIR}/prompt_packs.cpp
    ${CMAKE_CURRENT_LIST_DIR}/prompt_packs.h)
