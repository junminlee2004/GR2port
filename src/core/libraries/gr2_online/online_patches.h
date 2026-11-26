// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Code patches to the executable of Gravity Rush 2 that the served online content needs: without
// them a served challenge or notice ends in a double free, and the player's own name is missing
// from two screens.
namespace Libraries::Gr2Online::Patches {

/// Installs every patch that is not installed yet. Cheap once all are in. Called from the HTTP
/// library, which the game initialises before it asks the server for anything.
void Apply();

/// Recompiled mode, before the recompiled C loads for the executable at base: keeps the functions
/// that hold the code patches with the runtime, which sees Apply's writes. A function that a
/// patch makes return at once keeps its recompiled C; Apply hooks its entry.
void ExcludeSites(u64 base);

} // namespace Libraries::Gr2Online::Patches
