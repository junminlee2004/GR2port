// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <QFont>
#include <QString>

namespace Gr2Port::Launcher {

/// English text in the language in use, with each {N} replaced by values[N].
QString Tr(std::string_view english, const std::vector<std::string>& values = {});

/// The dialogs' typeface, 11 pixels tall at 96 DPI as the Windows launcher's 8 point Tahoma.
QFont DialogFont();

} // namespace Gr2Port::Launcher
