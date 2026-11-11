# SPDX-FileCopyrightText: Copyright 2026 GR2port
# SPDX-License-Identifier: GPL-2.0-or-later

if (WIN32)
    target_sources(gr2port PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/art.cpp
        ${CMAKE_CURRENT_LIST_DIR}/art.h
        ${CMAKE_CURRENT_LIST_DIR}/avatar.cpp
        ${CMAKE_CURRENT_LIST_DIR}/avatar.h
        ${CMAKE_CURRENT_LIST_DIR}/key_bindings.cpp
        ${CMAKE_CURRENT_LIST_DIR}/key_bindings.h
        ${CMAKE_CURRENT_LIST_DIR}/launcher.cpp
        ${CMAKE_CURRENT_LIST_DIR}/launcher.h
        ${CMAKE_CURRENT_LIST_DIR}/setup_dialog.cpp
        ${CMAKE_CURRENT_LIST_DIR}/setup_dialog.h
        ${CMAKE_CURRENT_LIST_DIR}/trophy_viewer.cpp
        ${CMAKE_CURRENT_LIST_DIR}/trophy_viewer.h)

    cmrc_add_resource_library(launcher-resources
        ALIAS res::launcher
        NAMESPACE launcher
        src/resources/launcher/logo.png
        src/resources/launcher/splash.jpg)
    target_link_libraries(gr2port PRIVATE res::launcher comctl32 gdiplus shlwapi)

    # The game's name, and a Windows program: the launcher is its first window, with no console.
    set_target_properties(gr2port PROPERTIES OUTPUT_NAME "Gravity Rush 2™" WIN32_EXECUTABLE TRUE)
    if (MSVC)
        # The Setup dialog's controls take the themed look of version 6 of the common controls.
        string(CONCAT GR2PORT_COMMON_CONTROLS
            "/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' "
            "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' "
            "language='*'")
        target_link_options(gr2port PRIVATE /ENTRY:mainCRTStartup "${GR2PORT_COMMON_CONTROLS}")
    endif()
elseif (UNIX AND NOT APPLE)
    # The same launcher with Qt's widgets. Its classes need no moc.
    find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets)
    target_sources(gr2port PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/launcher.h
        ${CMAKE_CURRENT_LIST_DIR}/qt/art.cpp
        ${CMAKE_CURRENT_LIST_DIR}/qt/art.h
        ${CMAKE_CURRENT_LIST_DIR}/qt/avatar.cpp
        ${CMAKE_CURRENT_LIST_DIR}/qt/avatar.h
        ${CMAKE_CURRENT_LIST_DIR}/qt/displays.cpp
        ${CMAKE_CURRENT_LIST_DIR}/qt/displays.h
        ${CMAKE_CURRENT_LIST_DIR}/qt/input_config.cpp
        ${CMAKE_CURRENT_LIST_DIR}/qt/input_config.h
        ${CMAKE_CURRENT_LIST_DIR}/qt/key_bindings.cpp
        ${CMAKE_CURRENT_LIST_DIR}/qt/key_bindings.h
        ${CMAKE_CURRENT_LIST_DIR}/qt/launcher.cpp
        ${CMAKE_CURRENT_LIST_DIR}/qt/setup_dialog.cpp
        ${CMAKE_CURRENT_LIST_DIR}/qt/setup_dialog.h
        ${CMAKE_CURRENT_LIST_DIR}/qt/style.cpp
        ${CMAKE_CURRENT_LIST_DIR}/qt/style.h
        ${CMAKE_CURRENT_LIST_DIR}/qt/text.cpp
        ${CMAKE_CURRENT_LIST_DIR}/qt/text.h
        ${CMAKE_CURRENT_LIST_DIR}/qt/trophy_viewer.cpp
        ${CMAKE_CURRENT_LIST_DIR}/qt/trophy_viewer.h
        ${CMAKE_CURRENT_LIST_DIR}/qt/video_card.cpp
        ${CMAKE_CURRENT_LIST_DIR}/qt/video_card.h)

    cmrc_add_resource_library(launcher-resources
        ALIAS res::launcher
        NAMESPACE launcher
        src/resources/launcher/logo.png
        src/resources/launcher/splash.jpg
        src/dist/gravity_rush_2.ico)
    target_link_libraries(gr2port PRIVATE res::launcher Qt6::Widgets)
    target_compile_definitions(gr2port PRIVATE QT_NO_KEYWORDS)
endif()
