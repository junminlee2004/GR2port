// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <string_view>

#include "common/types.h"

namespace Xbyak {
class CodeGenerator;
}

// More sliders on two pages of the System Menu. The Camera Configuration page takes two after the
// game's Camera Sensitivity: the camera sensitivity of the Special Attack's flight and of Panther
// Mode, which the game's own leaves alone. The Sound page takes one before the game's Music Volume:
// Low Energy Alert, the volume of the alert that sounds while Kat's gravity energy is low. A page's
// slider places show its own sliders and the added ones as a list, as many at a time as the page
// has places, with a scroll bar beside them, and Up and Down move through them. Each added slider
// is one more of the game's own slider rows, made with the game's recipe; its value acts at once
// and goes to config.json when the page closes. GraphicsMenu, which holds the System Menu's hooks,
// calls the functions below.
namespace Gr2Port::PageSliders {

using DrawFunction = void PS4_SYSV_ABI (*)(u64 handle, u64 context);

/// The start of the keys of the sliders' captions, before the English caption, and the key of
/// the record they take their style from: the game's Camera Sensitivity.
inline constexpr std::string_view KeyPrefix = "gr2port_gfx_slider_";
inline constexpr char ModelKey[] = "ui_system_menu_1_5_2";

/// Hooks the pages' frames.
bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity);

/// As the System Menu makes its rows: makes the sliders for system.
void MakeRows(u64 system);

/// Whether the sliders loaded, or there are none to wait for.
bool RowsLoaded();

/// Once the System Menu placed its rows: hangs the sliders, hidden, on their pages.
void Hang(u64 system);

/// Before the System Menu's rows take a slide: as a page slides in, its list shows its top.
void BeforeSlide(u64 system, u32 mode);

/// After the System Menu's rows took a slide: the sliders of the page on screen take it too.
void Slide(u64 system, u32 mode);

/// After the game drew the clip the System Menu's own rows draw after: draws the sliders of the
/// page on screen.
void Draw(u64 system, u64 context, DrawFunction draw);

/// Before the System Menu ends its rows: ends the sliders.
void End(u64 system);

} // namespace Gr2Port::PageSliders
