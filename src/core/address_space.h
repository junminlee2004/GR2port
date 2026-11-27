// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <boost/icl/separate_interval_set.hpp>
#include "common/arch.h"
#include "common/enum.h"
#include "common/multi_level_page_table.h"
#include "common/types.h"

namespace Core {

/// How far above the PS4's own addresses the guest's address space lies. A native arm64 process
/// on macOS maps nothing below 4 GiB, and the shared region holds 0x180000000-0x7000000000, so
/// the PS4's layout sits 512 GiB higher there. Guest memory stays identity-mapped: an address of
/// the PS4 plus this offset is the guest's address and the host's.
#if defined(__APPLE__) && defined(ARCH_ARM64)
inline constexpr VAddr LayoutOffset = 0x8000000000ULL;
#else
inline constexpr VAddr LayoutOffset = 0;
#endif

enum class MemoryPermission : u32 {
    None = 0,
    Read = 1 << 0,
    Write = 1 << 1,
    ReadWrite = Read | Write,
    Execute = 1 << 2,
    ReadWriteExecute = Read | Write | Execute,
};
DECLARE_ENUM_FLAG_OPERATORS(MemoryPermission)

/**
 * Represents the user virtual address space backed by a dmem memory block
 */
class AddressSpace {
public:
    explicit AddressSpace();
    ~AddressSpace();

    struct Traits {
        using Entry = u8*;
        static constexpr size_t ADDRESS_SPACE_BITS = 40;
        static constexpr size_t L1_BITS = 16;
        static constexpr size_t PAGE_BITS = 14;
        static constexpr bool NULL_CHECK = true;
    };
    using BackingPageTable = Common::MultiLevelPageTable<Traits>;

    [[nodiscard]] u8* BackingBase() const noexcept {
        return backing_base;
    }
    [[nodiscard]] const BackingPageTable& BackingPages() const noexcept {
        return backing_pages;
    }

    [[nodiscard]] VAddr SystemManagedVirtualBase() noexcept {
        return reinterpret_cast<VAddr>(system_managed_base);
    }
    [[nodiscard]] const u8* SystemManagedVirtualBase() const noexcept {
        return system_managed_base;
    }
    [[nodiscard]] u64 SystemManagedVirtualSize() const noexcept {
        return system_managed_size;
    }

    [[nodiscard]] VAddr SystemReservedVirtualBase() noexcept {
        return reinterpret_cast<VAddr>(system_reserved_base);
    }
    [[nodiscard]] const u8* SystemReservedVirtualBase() const noexcept {
        return system_reserved_base;
    }
    [[nodiscard]] u64 SystemReservedVirtualSize() const noexcept {
        return system_reserved_size;
    }

    [[nodiscard]] VAddr UserVirtualBase() noexcept {
        return reinterpret_cast<VAddr>(user_base);
    }
    [[nodiscard]] const u8* UserVirtualBase() const noexcept {
        return user_base;
    }
    [[nodiscard]] u64 UserVirtualSize() const noexcept {
        return user_size;
    }

    /**
     * @brief Maps memory to the specified virtual address.
     * @param virtual_addr The base address to place the mapping.
     *        If zero is provided an address in system managed area is picked.
     * @param size The size of the area to map.
     * @param phys_addr The offset of the backing file handle to map.
     *                  The same backing region may be aliased into different virtual regions.
     *                  If zero is provided the mapping is considered as private.
     * @return A pointer to the mapped memory.
     */
    void* Map(VAddr virtual_addr, u64 size, PAddr phys_addr = -1, bool exec = false);

    /// Memory maps a specified file descriptor.
    void* MapFile(VAddr virtual_addr, u64 size, u64 offset, u32 prot, uintptr_t fd);

    /// Unmaps specified virtual memory area and marks it free in the address space.
    VAddr Unmap(VAddr virtual_addr, u64* size);

    /// Protects requested region.
    void Protect(VAddr virtual_addr, u64 size, MemoryPermission perms);

    // Returns an interval set containing all usable regions.
    boost::icl::interval_set<VAddr> GetUsableRegions();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    u8* backing_base{};
    u8* system_managed_base{};
    u64 system_managed_size{};
    u8* system_reserved_base{};
    u64 system_reserved_size{};
    u8* user_base{};
    u64 user_size{};
    BackingPageTable backing_pages;
};

/// Observes protection changes for the stream mirror epoch machinery. The
/// observer runs before the permission change takes effect; tracker_origin
/// marks calls issued by the page manager's write-watch maintenance.
using ProtectObserver = void (*)(void* user, VAddr virtual_addr, u64 size, bool write_granted,
                                 bool tracker_origin);
void SetProtectObserver(ProtectObserver observer, void* user);
void NotifyProtectObserver(VAddr virtual_addr, u64 size, bool write_granted);

/// Marks protection calls issued by the page tracker while in scope.
struct TrackerProtectScope {
    TrackerProtectScope();
    ~TrackerProtectScope();
};

} // namespace Core
