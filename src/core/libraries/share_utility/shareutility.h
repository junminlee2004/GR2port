// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::ShareUtility {

void RegisterLib(Core::Loader::SymbolsResolver* sym);

} // namespace Libraries::ShareUtility
