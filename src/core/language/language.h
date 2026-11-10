// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <string>
#include <string_view>

// The text GR2port shows outside the game, in the console language the player picks. Each console
// language but English has a table from the English text to its own, embedded from
// src/resources/languages/<console language>.json. Text a table lacks stays English.
namespace Gr2Port::Language {

/// The console languages in the order the launcher lists them.
std::span<const int> Listed();

/// A console language's name for itself.
std::string Name(int console_language);

/// The language text is shown in: at first the console language in the settings.
int Current();

/// Shows text in a console language from now on.
void Use(int console_language);

/// English text in the current language, with each {N} replaced by values[N]. Without values the
/// text is returned as the table has it, so fmt placeholders stay.
std::string Text(std::string_view english, std::span<const std::string> values = {});

#ifdef _WIN32
/// The same for UTF-16 text.
std::wstring Text(std::wstring_view english, std::span<const std::wstring> values = {});
std::wstring WideName(int console_language);
#endif

} // namespace Gr2Port::Language
