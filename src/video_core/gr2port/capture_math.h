// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstring>

#include "common/types.h"

// What the capture's parts share: reads of guest memory and the game's matrices.
namespace Gr2Port::Capture {

template <typename T>
T Load(VAddr address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

using Matrix = std::array<float, 16>;

// a * b for the game's row-major matrices.
inline Matrix Multiply(const Matrix& a, const Matrix& b) {
    Matrix result{};
    for (u32 row = 0; row < 4; row++) {
        for (u32 column = 0; column < 4; column++) {
            for (u32 i = 0; i < 4; i++) {
                result[row * 4 + column] += a[row * 4 + i] * b[i * 4 + column];
            }
        }
    }
    return result;
}

// A's inverse.
inline Matrix Invert(const Matrix& a) {
    const float s0 = a[0] * a[5] - a[4] * a[1];
    const float s1 = a[0] * a[6] - a[4] * a[2];
    const float s2 = a[0] * a[7] - a[4] * a[3];
    const float s3 = a[1] * a[6] - a[5] * a[2];
    const float s4 = a[1] * a[7] - a[5] * a[3];
    const float s5 = a[2] * a[7] - a[6] * a[3];
    const float c5 = a[10] * a[15] - a[14] * a[11];
    const float c4 = a[9] * a[15] - a[13] * a[11];
    const float c3 = a[9] * a[14] - a[13] * a[10];
    const float c2 = a[8] * a[15] - a[12] * a[11];
    const float c1 = a[8] * a[14] - a[12] * a[10];
    const float c0 = a[8] * a[13] - a[12] * a[9];
    const float scale = 1.0f / (s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0);
    return {
        (a[5] * c5 - a[6] * c4 + a[7] * c3) * scale,
        (-a[1] * c5 + a[2] * c4 - a[3] * c3) * scale,
        (a[13] * s5 - a[14] * s4 + a[15] * s3) * scale,
        (-a[9] * s5 + a[10] * s4 - a[11] * s3) * scale,
        (-a[4] * c5 + a[6] * c2 - a[7] * c1) * scale,
        (a[0] * c5 - a[2] * c2 + a[3] * c1) * scale,
        (-a[12] * s5 + a[14] * s2 - a[15] * s1) * scale,
        (a[8] * s5 - a[10] * s2 + a[11] * s1) * scale,
        (a[4] * c4 - a[5] * c2 + a[7] * c0) * scale,
        (-a[0] * c4 + a[1] * c2 - a[3] * c0) * scale,
        (a[12] * s4 - a[13] * s2 + a[15] * s0) * scale,
        (-a[8] * s4 + a[9] * s2 - a[11] * s0) * scale,
        (-a[4] * c3 + a[5] * c1 - a[6] * c0) * scale,
        (a[0] * c3 - a[1] * c1 + a[2] * c0) * scale,
        (-a[12] * s3 + a[13] * s1 - a[14] * s0) * scale,
        (a[8] * s3 - a[9] * s1 + a[10] * s0) * scale,
    };
}

} // namespace Gr2Port::Capture
