// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::Gr2Photo {

/// Photo mode and album of Gravity Rush 2. Registers nothing for any other title.
void RegisterLib(Core::Loader::SymbolsResolver* sym);

} // namespace Libraries::Gr2Photo
