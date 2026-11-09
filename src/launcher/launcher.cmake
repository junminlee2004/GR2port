# SPDX-FileCopyrightText: Copyright 2026 GR2port
# SPDX-License-Identifier: GPL-2.0-or-later

if (WIN32)
    target_sources(shadps4 PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/art.cpp
        ${CMAKE_CURRENT_LIST_DIR}/art.h
        ${CMAKE_CURRENT_LIST_DIR}/avatar.cpp
        ${CMAKE_CURRENT_LIST_DIR}/avatar.h
        ${CMAKE_CURRENT_LIST_DIR}/key_bindings.cpp
        ${CMAKE_CURRENT_LIST_DIR}/key_bindings.h
        ${CMAKE_CURRENT_LIST_DIR}/launcher.cpp
        ${CMAKE_CURRENT_LIST_DIR}/launcher.h
        ${CMAKE_CURRENT_LIST_DIR}/setup_dialog.cpp
        ${CMAKE_CURRENT_LIST_DIR}/setup_dialog.h)

    cmrc_add_resource_library(launcher-resources
        ALIAS res::launcher
        NAMESPACE launcher
        src/resources/launcher/logo.png
        src/resources/launcher/splash.jpg)
    target_link_libraries(shadps4 PRIVATE res::launcher comctl32 gdiplus shlwapi)

    # The game's name, and a Windows program: the launcher is its first window, with no console.
    set_target_properties(shadps4 PROPERTIES OUTPUT_NAME "Gravity Rush 2™" WIN32_EXECUTABLE TRUE)
    if (MSVC)
        # The Setup dialog's controls take the themed look of version 6 of the common controls.
        string(CONCAT GR2PORT_COMMON_CONTROLS
            "/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' "
            "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' "
            "language='*'")
        target_link_options(shadps4 PRIVATE /ENTRY:mainCRTStartup "${GR2PORT_COMMON_CONTROLS}")
    endif()
endif()
