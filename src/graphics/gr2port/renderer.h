// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The render thread: draws each scene the capture publishes into the renderer's window.
namespace Gr2Port::Renderer {

/// Starts the render thread, which creates the window and the device.
void Start();

} // namespace Gr2Port::Renderer
