// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

class QStyle;

namespace Gr2Port::Launcher {

/// The dialogs' style: Qt's Fusion, with the check boxes, group boxes and tab widths of the
/// Windows launcher's controls, which the dialogs' fixed places are measured for. The caller
/// owns it.
QStyle* MakeStyle();

} // namespace Gr2Port::Launcher
