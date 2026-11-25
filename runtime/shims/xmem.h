#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "../src/gpu_label_memory.h"  // leonida::label::Target: the console's value writes (list slots 71 and 72)

namespace rt::xmem {

void geometry_aliases(std::uintptr_t va, std::vector<std::uintptr_t>& addresses);
bool commit_physical_range(const void* address, std::size_t size);
// A lifetime-pinned alias of a contiguous range of the title's writable mapped physical pages.
std::shared_ptr<std::uint8_t> pin_mapped_data(const void* address, std::size_t size);
bool commit_physical_pages(const std::uint64_t* page_array, std::size_t count);

// Copy a validated DirectStorage staging buffer through the page table. The page list has
// one frame number per 64 KiB page. All pages are resolved before a write starts.
bool transfer_physical(std::span<const std::uint64_t> pages, std::uint16_t offset,
                       std::span<std::uint8_t> staging, bool write);

// Console queue slot 31, CopyPageMappingsBatchX (d3d12_xs_device.cpp): maps at `destination` the physical pages that
// are mapped at `source` now, `pages` 64 KiB pages one after another, or the one page at `source` every time with
// `repeat_first`. A source page with nothing mapped leaves its destination page unmapped; a null `source` unmaps them
// all. False, with the last error set, for an unaligned address or a destination outside a physical-page window.
// Set by xmem.cpp when it is linked: the command-object proxy is also compiled into tests without it.
using CopyPageMappingsFn = bool (*)(void* destination, const void* source, std::uint64_t pages, bool repeat_first);
inline CopyPageMappingsFn copy_page_mappings_fn = nullptr;

// Writes `size` bytes at `address` in the title's memory, through the host pages xmem manages. False when the
// range is not mapped, or not writable where it is.
bool write_title_bytes(const void* address, const void* bytes, std::size_t size);

// The host write into the title's memory that the console's value writes (list slots 71 and 72) need. The shim
// owns the implementation and sets this pointer when it is linked into the process. The D3D12 layer calls the write
// through the pointer, so the D3D12 library does not have to link this shim - and with it the export table and the
// vectored exception handler - into every consumer of that library.
using TitleWriteFn = bool (*)(const void* address, const void* bytes, std::size_t size);
inline TitleWriteFn title_write_fn = nullptr;

// The write through the registered pointer. False when the shim is not linked into the process: the console's value
// writes stay dropped, the way they were before the shim answered them.
inline bool call_title_write(const void* address, const void* bytes, std::size_t size) {
    if (!title_write_fn) return false;
    return title_write_fn(address, bytes, size);
}

// The label target of an address in the pages the title maps: those pages are the title's memory too, and the
// console's value writes name them (runtime/src/vulkan_gpu_labels.h). The shim owns the page tables, so it answers
// the question. False when the address is not in one of the title's mapped pages.
using LabelTargetFn = bool (*)(std::uintptr_t address, unsigned bytes, leonida::label::Target& out);
inline LabelTargetFn label_target_fn = nullptr;

}
