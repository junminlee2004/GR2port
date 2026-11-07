// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "mutex.h"

#include "common/assert.h"

namespace Libraries::Kernel {

TimedMutex::TimedMutex() {
#ifdef _WIN64
    unlocked = CreateEvent(nullptr, false, false, nullptr);
    ASSERT(unlocked);
#endif
}

TimedMutex::~TimedMutex() {
#ifdef _WIN64
    CloseHandle(unlocked);
#endif
}

void TimedMutex::lock() {
#ifdef _WIN64
    u32 expected = Free;
    if (state.compare_exchange_strong(expected, Held, std::memory_order_acquire,
                                      std::memory_order_relaxed)) {
        return;
    }
    while (state.exchange(Waited, std::memory_order_acquire) != Free) {
        WaitForSingleObjectEx(unlocked, INFINITE, true);
    }
#else
    mtx.lock();
#endif
}

bool TimedMutex::try_lock() {
#ifdef _WIN64
    u32 expected = Free;
    return state.load(std::memory_order_relaxed) == Free &&
           state.compare_exchange_strong(expected, Held, std::memory_order_acquire,
                                         std::memory_order_relaxed);
#else
    return mtx.try_lock();
#endif
}

void TimedMutex::unlock() {
#ifdef _WIN64
    if (state.exchange(Free, std::memory_order_release) == Waited) {
        SetEvent(unlocked);
    }
#else
    mtx.unlock();
#endif
}

} // namespace Libraries::Kernel
