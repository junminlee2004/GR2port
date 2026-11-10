# SPDX-FileCopyrightText: Copyright 2026 GR2port
# SPDX-License-Identifier: GPL-2.0-or-later

target_sources(shadps4 PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/language.cpp
    ${CMAKE_CURRENT_LIST_DIR}/language.h)

# A table for each console language but English, from the English text to its own.
file(GLOB GR2PORT_LANGUAGES RELATIVE ${CMAKE_SOURCE_DIR} CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/resources/languages/*.json)
cmrc_add_resource_library(gr2port-languages
    ALIAS res::gr2port_languages
    NAMESPACE gr2port_languages
    ${GR2PORT_LANGUAGES})
target_link_libraries(shadps4 PRIVATE res::gr2port_languages)
