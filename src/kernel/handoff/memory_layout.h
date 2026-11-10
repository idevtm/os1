// Fixed early physical layout used by BIOS assembly, Limine handoff code, and
// kernel C++ before the page-frame allocator owns memory lifetime.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel_layout.hpp"  // IWYU pragma: export

// These fixed addresses are still part of the early BIOS-era bring-up path.
// Milestone 1 names them explicitly so ownership is clear even before later
// milestones make more of them dynamic.

constexpr uint64_t kPageSize = 0x1000;

// Loader16 and the AP trampoline intentionally reuse the same physical page in
// different phases of boot. The lifetimes do not overlap.
constexpr uint64_t kLoader16LoadAddress = 0x1000;
constexpr uint64_t kApTrampolineAddress = kLoader16LoadAddress;

// The loader publishes the BootInfo block in low memory so the kernel can copy
// it before any allocator or paging policy changes ownership.
constexpr uint64_t kBootInfoAddress = 0x0500;
// BIOS disk packets live in dedicated low-memory scratch so firmware I/O does
// not scribble over executable boot sectors while loading later stages.
constexpr uint64_t kBootDiskPacketAddress = 0x0800;
constexpr uint64_t kLoaderDiskPacketAddress = 0x0820;
constexpr uint64_t kLoaderDiskRangeStateAddress = 0x0840;
constexpr uint64_t kBootMemoryRegionBufferAddress = 0x6000;
constexpr uint64_t kBootModuleInfoBufferAddress = 0x7000;
// The Limine shim mirrors the BIOS handoff layout by copying strings into a
// small low-memory pool before switching to its final identity-mapped CR3.
constexpr uint64_t kBootStringBufferAddress = 0x7200;
constexpr uint64_t kBootStringBufferSizeBytes = 0x0E00;
constexpr size_t kBootMemoryRegionCapacity = 128;

// The temporary real-mode page tables only exist long enough to enter long
// mode. The kernel later replaces them with its own page tables.
// BIOS disk and boot-log scratch in the first page is dead before table construction.
constexpr uint64_t kEarlyLongModePageTablesAddress = 0x9000;

// The BIOS loader expands the kernel ELF from this low-memory image buffer.
// The generated kernel_layout.hpp contract keeps this staging address aligned
// with the BIOS raw-image slot and the initrd staging buffer.

// The AP trampoline communicates through a tiny parameter block in low memory
// because secondary CPUs start before they have per-CPU stacks or rich state.
constexpr uint64_t kApStartupCpuPageAddress = 0x20;
constexpr uint64_t kApStartupRipAddress = 0x28;
constexpr uint64_t kApStartupCr3Address = 0x30;
constexpr uint64_t kApStartupIdtAddress = 0x38;
constexpr uint64_t kApStartupIdtSizeBytes = 6;

// The page-frame bitmap still lives in a fixed early physical window in M1 so
// the kernel can bring up allocation deterministically before later cleanup
// work. It sits immediately above the reserved kernel window so it avoids the
// BIOS kernel/initrd staging buffers while still fitting inside the initial
// BIOS identity map. One bit per page means 512 KiB covers 16 GiB of RAM.
constexpr uint64_t kPageFrameBitmapBaseAddress = 0x1A0000;
static_assert(kKernelReservedPhysicalEnd <= kPageFrameBitmapBaseAddress,
              "kernel reservation overlaps the page-frame bitmap");
constexpr uint64_t kPageFrameBitmapSizeBytes = 0x80000;
constexpr uint64_t kPageFrameBitmapQwordLimit = kPageFrameBitmapSizeBytes / sizeof(uint64_t);

// Early low-memory scratch remains reserved until boot no longer depends on it.
constexpr uint64_t kEarlyReservedPhysicalEnd = 0x20000;

// Persistent RAM boot log for BIOS bring-up on real hardware. The BIOS loader
// saves the previous boot's log from this window onto the boot disk, re-arms the
// header, and publishes the window to the kernel as the "bootlog" boot module.
// Keep these values synchronized with memory_layout.inc.
constexpr uint64_t kBootLogPhysicalAddress = 0x04000000;
constexpr uint64_t kBootLogMagic = 0x31474F4C4231534Full;  // "OS1BLOG1"
constexpr uint32_t kBootLogVersion = 1;
constexpr uint32_t kBootLogHeaderBytes = 64;
// UEFI path: the os1 EFI pre-loader reserves the first free window from this
// list as EfiReservedMemoryType before chain-loading Limine, and saves the
// previous boot's log from whichever window holds one. The Limine shim
// identity-maps up to kBootLogUefiCandidatesEnd so the kernel can attach early.
// The windows deliberately avoid large power-of-two boundaries: the ASUS G74SX
// firmware writes a 0x5A5A5A5A pattern at 16 MiB-aligned addresses on reset,
// which destroyed headers placed there.
constexpr uint64_t kBootLogUefiWindowBytes = 0x100000;
constexpr uint64_t kBootLogUefiCandidates[] = {0x04A35000, 0x05B47000, 0x06C59000, 0x07D6B000};
constexpr uint64_t kBootLogUefiCandidatesEnd = 0x07D6B000 + kBootLogUefiWindowBytes;


constexpr uint64_t kKernelPml4Index = (kKernelVirtualOffset >> 39) & 0x1FFull;
constexpr uint64_t kDirectMapPml4Index = (kDirectMapBase >> 39) & 0x1FFull;
constexpr uint64_t kKernelVirtualBase = kKernelVirtualOffset + kKernelReservedPhysicalStart;
constexpr uint64_t kKernelVirtualEnd = kKernelVirtualOffset + kKernelReservedPhysicalEnd;
constexpr uint64_t kInvalidPhysicalAddress = ~0ull;

[[nodiscard]] constexpr uint64_t phys_to_virt(uint64_t physical_address)
{
	return physical_address + kDirectMapBase;
}

[[nodiscard]] constexpr bool is_kernel_virtual_address(uint64_t virtual_address)
{
	return (virtual_address >= kKernelVirtualBase) && (virtual_address < kKernelVirtualEnd);
}

[[nodiscard]] constexpr bool is_direct_map_virtual_address(uint64_t virtual_address)
{
	return (virtual_address >= kDirectMapBase) && !is_kernel_virtual_address(virtual_address);
}

[[nodiscard]] constexpr uint64_t kernel_virt_to_phys(uint64_t virtual_address)
{
	return is_kernel_virtual_address(virtual_address)
	           ? (virtual_address - kKernelVirtualOffset)
	           : kInvalidPhysicalAddress;
}

[[nodiscard]] constexpr uint64_t direct_virt_to_phys(uint64_t virtual_address)
{
	return is_direct_map_virtual_address(virtual_address)
	           ? (virtual_address - kDirectMapBase)
	           : kInvalidPhysicalAddress;
}

[[nodiscard]] constexpr uint64_t virt_to_phys(uint64_t virtual_address)
{
	if(is_kernel_virtual_address(virtual_address))
	{
		return kernel_virt_to_phys(virtual_address);
	}
	if(is_direct_map_virtual_address(virtual_address))
	{
		return direct_virt_to_phys(virtual_address);
	}
	return kInvalidPhysicalAddress;
}

#if defined(OS1_HOST_TEST)
[[nodiscard]] void* os1_host_physical_pointer(uint64_t physical_address);

template<typename T>
[[nodiscard]] inline T* kernel_physical_pointer(uint64_t physical_address)
{
	return static_cast<T*>(os1_host_physical_pointer(physical_address));
}
#else
extern bool g_kernel_direct_map_ready;

template<typename T>
[[nodiscard]] inline T* kernel_physical_pointer(uint64_t physical_address)
{
	const uint64_t virtual_address =
		g_kernel_direct_map_ready ? phys_to_virt(physical_address) : physical_address;
	return reinterpret_cast<T*>(virtual_address);
}
#endif


// User mappings stay in their own PML4 slot even after the higher-half
// migration; the kernel now reaches physical memory through the direct map and
// keeps only narrow low bootstrap identity exceptions.
constexpr uint64_t kUserPml4Index = 1;
constexpr uint64_t kUserSpaceBase = 0x0000008000000000ull;
constexpr uint64_t kUserImageBase = 0x0000008000400000ull;
constexpr uint64_t kUserStackTop = 0x0000008040000000ull;
constexpr size_t kUserStackPages = 16;
constexpr size_t kKernelThreadStackPages = 4;

constexpr uint16_t kBootTextColumns = 80;
constexpr uint16_t kBootTextRows = 25;
