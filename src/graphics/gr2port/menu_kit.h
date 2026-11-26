// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "common/types.h"
#include "core/recompiled/recompiled.h"

// What GR2port's rows in the game's menus share: the game's code and data by GoldHEN address, the
// pad input its menus read, its clips, text records, sounds and dialog.
namespace Gr2Port::MenuKit {

// The pause menu, which holds the input its pages read.
constexpr VAddr PauseMenu = 0x1eee178;
constexpr u64 PauseMenuInput = 0xd0;
// The bit numbers of the buttons that decide and cancel.
constexpr VAddr DecideButton = 0x1c81b70;
constexpr VAddr CancelButton = 0x1c81b74;

// A menu's input: the buttons pressed this frame, the ones held, the ones that repeat while held,
// and whether the menu reads them.
constexpr u64 InputPressed = 0x08;
constexpr u64 InputHeld = 0x0c;
constexpr u64 InputRepeated = 0x80;
constexpr u64 InputEnabled = 0x18c;
// The d-pad's and the left stick's bits together.
constexpr u32 OptionsButton = 0x4000;
constexpr u32 UpButtons = 0x800400;
constexpr u32 DownButtons = 0x1001000;
constexpr u32 LeftButtons = 0x2002000;
constexpr u32 RightButtons = 0x4000800;

// A clip handle: the clip, its tracks and the texts attached to its nodes.
constexpr u64 HandleClip = 0x00;
constexpr u64 HandleFirstTrack = 0x10;
constexpr u64 HandleLastTrack = 0x18;
constexpr u64 HandleFirstText = 0x30;
constexpr u64 HandleLastText = 0x38;

// A clip: the node it hangs from, its resource, its own nodes and the state of each node's
// animation.
constexpr u64 ClipParent = 0xb0;
constexpr u64 ClipResource = 0xa0;
constexpr u64 ClipNodes = 0x100;
constexpr u64 ClipNodeStates = 0x138;

constexpr u32 NoNode = 0x8cde0001;

// One track of a clip handle: the index of its animation that plays, its node and its animations.
struct Track {
    u32 current;
    u32 node;
    u8 reversed;
    u8 unused[15];
    const u32* first;
    const u32* last;
    u64 capacity;
};
static_assert(sizeof(Track) == 0x30);

// An attached text: its node, its number among the node's texts, then the text.
struct AttachedText {
    u32 node;
    u32 index;
    u64 text;
};
static_assert(sizeof(AttachedText) == 0x10);

// A clip's node. A clip that hangs from a node takes the node's world matrix, four rows of four
// floats with the translation last, and, in a draw that asks for it, the node's opacity.
struct alignas(32) Node {
    u8 head[0x80];
    std::array<float, 16> world;
    u8 pad[8];
    float opacity;
    u8 tail[0x14];
};
static_assert(sizeof(Node) == 0xe0);

/// Keeps the base the game's executable loaded at; Game takes addresses from it.
void SetBase(VAddr module_base);

/// The address the game's GoldHEN address has in this process.
VAddr Game(VAddr address);

template <typename T>
T Read(VAddr address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

template <typename T>
void Write(VAddr address, T value) {
    std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(T));
}

/// A function of the game, which a call runs through Core::Recompiled::Call.
template <typename Function>
struct GameFunction {
    Function function;

    template <typename... Args>
    auto operator()(Args... args) const {
        return Core::Recompiled::Call(function, args...);
    }
};

/// The game's function at a GoldHEN address.
template <typename Function>
GameFunction<Function> Call(VAddr address) {
    return {reinterpret_cast<Function>(Game(address))};
}

/// The record of text in the style of the game's record model.
std::vector<u8> MakeRecord(u64 model, std::string_view text);

/// English text in the language the game shows, which is not always the console's: a game that
/// lacks the console's language shows English or another regional form.
std::string Shown(std::string_view english);

/// The pad bit of a button whose bit number the game keeps at number.
u32 ButtonBit(VAddr number);

void Sound(u32 sound);

/// Opens the game's dialog with the text of key. Kind 1 asks Yes or No, with choice first.
void OpenDialog(const char* key, s32 kind, s32 choice);

/// The open dialog's answer: -1 while it is open, then the number of the choice.
s32 Answer();

/// Starts an animation of a clip handle's track.
void PlayTrack(u64 handle, size_t track_index, size_t animation);

/// Whether a clip handle's track plays its animation, as the game's own pages test it.
bool Playing(u64 handle, size_t track_index, size_t animation);

/// Index of the node with the name hash in a clip's resource, or NoNode.
u32 FindNode(u64 clip, u32 hash);

} // namespace Gr2Port::MenuKit
