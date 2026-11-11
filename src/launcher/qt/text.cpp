// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QStringList>

#include "core/language/language.h"
#include "launcher/qt/text.h"

namespace Gr2Port::Launcher {

QString Tr(std::string_view english, const std::vector<std::string>& values) {
    return QString::fromStdString(Language::Text(english, values));
}

QFont DialogFont() {
    QFont font;
    font.setFamilies({QStringLiteral("Tahoma"), QStringLiteral("Noto Sans")});
    font.setPixelSize(11);
    return font;
}

} // namespace Gr2Port::Launcher
