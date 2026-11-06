// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Gr2Port {

/// Windows, from the emulator's vectored exception handler, with its EXCEPTION_POINTERS: logs an
/// exception nothing handles, with its registers and the module and offset of each stack frame.
void ReportFault(void* exception_pointers);

} // namespace Gr2Port
