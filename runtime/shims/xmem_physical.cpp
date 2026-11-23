// xmem.dll's physical pages: XMemAllocatePhysicalPages, XMemFreePhysicalPages, XMemMapPhysicalPages and
// XMemMapPhysicalPagesScatter, and the reservations they are mapped into.
// Adapted from coenflict/Leonida PR #6995, head 67434959a09562164e981a0c40edafa5df509305.
// This local integration adds checked rollback and retains ownership after failed host operations.
//
// The shapes come from the retail executable's call sites (1.0.2.1; four callers of the allocation, three of the free,
// eleven of the map and two of the scatter form). They follow the Win32 AWE functions with the process handle left
// out and an attribute word added:
//   BOOL  XMemAllocatePhysicalPages(ULONG64 attributes, ULONG_PTR* count, ULONG_PTR* page_array)
//           count is in/out, in 64 KiB pages; the callers require TRUE and the count written back unchanged.
//           Measured attribute words: 0x180, 0x280, 0x380 (not decoded; recorded in the startup trace).
//   BOOL  XMemFreePhysicalPages(ULONG_PTR count, ULONG_PTR* page_array)        count by value
//   PVOID XMemMapPhysicalPages(PVOID va, ULONG_PTR count, ULONG_PTR* page_array)
//           the callers test the full 64-bit result for non-null; a null page_array unmaps the range.
//   BOOL  XMemMapPhysicalPagesScatter(PVOID* va_array, ULONG_PTR count, ULONG_PTR* page_array)
//           a null page_array unmaps every listed page; a caller writes 0 entries for pages it leaves unmapped.
// The pages are mapped into ranges reserved with XMemVirtualAlloc(nullptr, size, MEM_RESERVE | MEM_LARGE_PAGES |
// MEM_PHYSICAL, attributes, protect), MEM_LARGE_PAGES | MEM_PHYSICAL being the console's 64 KiB page mode; the
// executable makes two 24 GiB ones at startup and smaller ones per pool. It frees them with VirtualFree.
//
// The page-array entries are frame numbers the title treats as opaque, except that one caller rebuilds an entry as a
// 64 KiB page index times 16, masked to 25 bits. So an entry here is a 4 KiB frame number of a 64 KiB page: (slot + 1)
// * 16, never 0 (0 means "unmapped" in the scatter form), and the store has at most 2^21 - 1 slots (128 GiB).
//
// The host has no user-mode physical pages without SeLockMemoryPrivilege (AWE), and AWE pages cannot be mapped at two
// addresses at once, which the console allows (the two 24 GiB reservations differ only in their attribute word and
// CPU protection). So a page is a 64 KiB slot of a pagefile-backed section (sections of 512 slots), and a mapping is a
// view of that slot: the reservation is a placeholder (VirtualAlloc2 with MEM_RESERVE_PLACEHOLDER), a page is split
// out of it and replaced with a view (MapViewOfFile3 with MEM_REPLACE_PLACEHOLDER), and unmapping puts the
// placeholder back (UnmapViewOfFile2 with MEM_PRESERVE_PLACEHOLDER). One page can then sit at any number of addresses
// and every view sees the same bytes, as on the console. New pages read as zero, a freed page is zeroed before it is
// handed out again, and freeing a page unmaps it everywhere (as FreeUserPhysicalPages does).
//
// Not done: the GPU side. These pages are CPU memory only; the D3D12 layer does not see them yet. #5381's
// PhysicalPageStore is the same mechanism with a device-local half and dirty tracking, and can take this store's
// sections. Committing ordinary memory inside a physical reservation fails (ERROR_INVALID_ADDRESS), as on Win32.
#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "shims.h"
#include "startup_trace.h"
#include "xmem_physical.h"

namespace {

constexpr SIZE_T kPage = 0x10000;
constexpr ULONG_PTR kFramesPerPage = kPage / 0x1000;
constexpr uint32_t kChunkPages = 512;
constexpr uint32_t kMaxSlots = (1u << 21) - 1;

using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*,
                                        ULONG);
using UnmapViewOfFile2Fn = BOOL(WINAPI*)(HANDLE, PVOID, ULONG);

// Looked up rather than imported, so the runtime still starts where they are missing (before Windows 10 1803, or a
// Wine without placeholders). Physical reservations fail explicitly on such a host.
struct PlaceholderApi {
    VirtualAlloc2Fn alloc2 = nullptr;
    MapViewOfFile3Fn map3 = nullptr;
    UnmapViewOfFile2Fn unmap2 = nullptr;
    bool ok() const { return alloc2 && map3 && unmap2; }
};

const PlaceholderApi& api() {
    static const PlaceholderApi a = [] {
        PlaceholderApi r;
        if (HMODULE k = GetModuleHandleW(L"kernelbase.dll")) {
            r.alloc2 = reinterpret_cast<VirtualAlloc2Fn>(reinterpret_cast<void*>(GetProcAddress(k, "VirtualAlloc2")));
            r.map3 = reinterpret_cast<MapViewOfFile3Fn>(reinterpret_cast<void*>(GetProcAddress(k, "MapViewOfFile3")));
            r.unmap2 =
                reinterpret_cast<UnmapViewOfFile2Fn>(reinterpret_cast<void*>(GetProcAddress(k, "UnmapViewOfFile2")));
        }
        return r;
    }();
    return a;
}

struct Chunk {
    HANDLE section;
    uint8_t* view;  // the store's own view of the whole section, for zeroing a reused page
};

struct Slot {
    bool allocated = false;
    bool needs_zero = false;
    std::vector<uintptr_t> mapped_at;
};

struct Region {
    SIZE_T pages = 0;
    DWORD protect = PAGE_NOACCESS;
    std::vector<DWORD> page_protect; // protection after MEM_REPLACE_PLACEHOLDER, per 64 KiB page
    std::vector<uint32_t> page;     // slot + 1 mapped at each page, 0 for none
    std::vector<uint8_t> isolated;  // the page is a placeholder or view of its own
    std::vector<uint8_t> needs_protect;  // a fallback view exists, but narrowing its protection failed
    bool split = false;            // coalesce the placeholders before releasing the whole window
};

std::mutex g_mutex;
std::vector<Chunk> g_chunks;
std::vector<Slot> g_slots;
std::vector<uint32_t> g_free;            // free slots, the next one at the back
std::map<uintptr_t, Region> g_regions;  // physical reservations by base
std::atomic<rt::xmem::testing::FailureHook> g_failure_hook{nullptr};

using HostOperation = rt::xmem::testing::HostOperation;

bool fail_host(HostOperation operation, void* address) {
    const auto hook = g_failure_hook.load(std::memory_order_acquire);
    const DWORD error = hook ? hook(operation, address) : ERROR_SUCCESS;
    if (error == ERROR_SUCCESS) return false;
    SetLastError(error);
    return true;
}

SIZE_T query_host(void* address, MEMORY_BASIC_INFORMATION& mbi) {
    if (fail_host(HostOperation::Query, address)) return 0;
    return VirtualQuery(address, &mbi, sizeof(mbi));
}

bool free_host(void* address, SIZE_T size, DWORD flags, HostOperation operation) {
    if (fail_host(operation, address)) return false;
    return VirtualFree(address, size, flags) != FALSE;
}

bool unmap_host(void* address) {
    if (fail_host(HostOperation::Unmap, address)) return false;
    return api().unmap2(GetCurrentProcess(), address, MEM_PRESERVE_PLACEHOLDER) != FALSE;
}

bool protect_host(void* address, DWORD protect) {
    if (fail_host(HostOperation::Protect, address)) return false;
    DWORD old = 0;
    return VirtualProtect(address, kPage, protect, &old) != FALSE;
}

void* map_host(HANDLE section, void* address, ULONG64 offset, DWORD protect) {
    if (fail_host(HostOperation::Map, address)) return nullptr;
    return api().map3(section, GetCurrentProcess(), address, offset, kPage, MEM_REPLACE_PLACEHOLDER,
                      protect, nullptr, 0);
}

ULONG_PTR frame_of(uint32_t slot) { return (static_cast<ULONG_PTR>(slot) + 1) * kFramesPerPage; }

bool slot_of(ULONG_PTR frame, uint32_t& slot) {
    if (frame == 0 || frame % kFramesPerPage != 0) return false;
    const ULONG_PTR index = frame / kFramesPerPage - 1;
    if (index >= g_slots.size() || !g_slots[index].allocated) return false;
    slot = static_cast<uint32_t>(index);
    return true;
}

// Adds sections until `count` slots are free.
bool grow(size_t count) {
    if (count > g_free.size() + kMaxSlots - g_slots.size()) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    const size_t missing = count > g_free.size() ? count - g_free.size() : 0;
    const size_t extra_chunks = (missing + kChunkPages - 1) / kChunkPages;
    const size_t target_slots =
        (std::min)(static_cast<size_t>(kMaxSlots), g_slots.size() + extra_chunks * kChunkPages);
    // Allocate all bookkeeping before acquiring host resources. No vector growth can throw after a section
    // becomes owned by the store. A later host allocation failure may leave additional free, owned chunks.
    g_chunks.reserve((target_slots + kChunkPages - 1) / kChunkPages);
    g_slots.reserve(target_slots);
    g_free.reserve(target_slots);
    while (g_free.size() < count) {
        const uint32_t first = static_cast<uint32_t>(g_slots.size());
        const uint32_t pages = (std::min)(kChunkPages, kMaxSlots - first);
        const uint64_t bytes = static_cast<uint64_t>(pages) * kPage;
        HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT,
                                            static_cast<DWORD>(bytes >> 32), static_cast<DWORD>(bytes), nullptr);
        if (!section) return false;
        void* view = MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
        if (!view) {
            const DWORD error = GetLastError();
            CloseHandle(section);
            SetLastError(error);
            return false;
        }
        g_chunks.push_back({section, static_cast<uint8_t*>(view)});
        g_slots.resize(first + pages);
        for (uint32_t i = pages; i-- > 0;) g_free.push_back(first + i);
    }
    return true;
}

Region* region_of(uintptr_t va, ULONG_PTR count, uintptr_t& base, size_t& index) {
    auto it = g_regions.upper_bound(va);
    if (it == g_regions.begin()) return nullptr;
    --it;
    base = it->first;
    Region& r = it->second;
    if ((va - base) / kPage >= r.pages) return nullptr;
    index = (va - base) / kPage;
    if (count > r.pages - index) return nullptr;
    return &r;
}

uintptr_t page_address(uintptr_t base, size_t index) { return base + index * kPage; }

// Splits page `index` out of the placeholder that holds it, unless it already is a placeholder of its own.
bool isolate(Region& r, uintptr_t base, size_t index) {
    if (r.isolated[index]) return true;
    const uintptr_t a = page_address(base, index);
    MEMORY_BASIC_INFORMATION mbi{};
    if (!query_host(reinterpret_cast<void*>(a), mbi)) return false;
    const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    if (reinterpret_cast<uintptr_t>(mbi.AllocationBase) != a || end != a + kPage) {
        if (!free_host(reinterpret_cast<void*>(a), kPage, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER,
                       HostOperation::Split)) return false;
        r.split = true;
    }
    r.isolated[index] = 1;
    return true;
}

void forget_mapping(uint32_t slot, uintptr_t a) {
    auto& at = g_slots[slot].mapped_at;
    at.erase(std::remove(at.begin(), at.end(), a), at.end());
}

bool unmap_page(Region& r, uintptr_t base, size_t index) {
    if (r.page[index] == 0) return true;
    const uintptr_t a = page_address(base, index);
    if (!unmap_host(reinterpret_cast<void*>(a))) return false;
    forget_mapping(r.page[index] - 1, a);
    r.page[index] = 0;
    r.needs_protect[index] = 0;
    return true;
}

bool map_page(Region& r, uintptr_t base, size_t index, uint32_t slot) {
    void* const a = reinterpret_cast<void*>(page_address(base, index));
    if (r.page[index] == slot + 1) {
        if (!r.needs_protect[index]) return true;
        if (!protect_host(a, r.protect)) return false;
        r.needs_protect[index] = 0;
        return true;
    }
    if (!isolate(r, base, index) || !unmap_page(r, base, index)) return false;
    const Chunk& chunk = g_chunks[slot / kChunkPages];
    const ULONG64 offset = static_cast<ULONG64>(slot % kChunkPages) * kPage;
    const DWORD page_protect = r.page_protect.empty() ? r.protect : r.page_protect[index];
    void* view = map_host(chunk.section, a, offset, page_protect);
    bool narrow_protection = false;
    if (!view && page_protect == PAGE_NOACCESS) {
        // Some hosts refuse NOACCESS when creating a view. Record a successful fallback BEFORE narrowing it:
        // if both protection and cleanup fail, the live view must remain owned and must not allow slot reuse.
        view = map_host(chunk.section, a, offset, PAGE_READWRITE);
        narrow_protection = view != nullptr;
    }
    if (!view) return false;
    // prepare_changes() reserved this capacity before any host mapping was changed.
    g_slots[slot].mapped_at.push_back(page_address(base, index));
    r.page[index] = slot + 1;
    r.needs_protect[index] = narrow_protection;
    if (narrow_protection && !protect_host(view, page_protect)) return false;
    r.needs_protect[index] = 0;
    return true;
}

struct Change {
    Region* region;
    uintptr_t base;
    size_t index;
    uint32_t before;  // slot + 1, or zero
    uint32_t after;
};

// Allocate reverse-mapping capacity for both the operation and a rollback before touching the host. In
// particular, push_back after a successful MapViewOfFile3 must never throw and leave an untracked live view.
void prepare_changes(const std::vector<Change>& changes) {
    std::map<uint32_t, size_t> extra;
    for (const Change& c : changes) {
        if (c.before) ++extra[c.before - 1];
        if (c.after) ++extra[c.after - 1];
    }
    for (const auto& [slot, count] : extra) {
        auto& aliases = g_slots[slot].mapped_at;
        if (count > aliases.max_size() - aliases.size()) throw std::length_error("physical page aliases");
        aliases.reserve(aliases.size() + count);
    }
}

// Rollback itself can fail at the OS boundary. In that case each successful host operation has still updated
// both directions of the mapping records, and every referenced frame remains allocated. Report the incomplete
// rollback rather than forgetting a view or claiming an atomic operation.
bool rollback_changes(const std::vector<Change>& changes, size_t count) {
    bool ok = true;
    while (count != 0) {
        const Change& c = changes[--count];
        if (c.before) {
            if (!map_page(*c.region, c.base, c.index, c.before - 1)) ok = false;
        } else if (!unmap_page(*c.region, c.base, c.index)) {
            ok = false;
        }
    }
    return ok;
}

bool fail_and_rollback(const std::vector<Change>& changes, size_t count, DWORD original_error) {
    if (!rollback_changes(changes, count)) {
        std::fprintf(stderr, "[rt] xmem: host mapping rollback incomplete; surviving mappings remain tracked\n");
        SetLastError(ERROR_GEN_FAILURE);
    } else {
        SetLastError(original_error);
    }
    return false;
}

bool apply_changes(const std::vector<Change>& changes) {
    prepare_changes(changes);
    // Splitting a still-empty placeholder does not alter its backing or protection. Do it before unmapping
    // any old view, so a split failure cannot destroy a mapping.
    for (const Change& c : changes) {
        if (c.after && !isolate(*c.region, c.base, c.index)) return false;
    }
    for (size_t i = 0; i < changes.size(); ++i) {
        const Change& c = changes[i];
        const bool ok = c.after ? map_page(*c.region, c.base, c.index, c.after - 1)
                               : unmap_page(*c.region, c.base, c.index);
        if (!ok) return fail_and_rollback(changes, i + 1, GetLastError());
    }
    return true;
}

// The first allocation for each distinct attribute word, to the startup trace (gta6rt --trace).
void note_allocation(uint64_t attributes, ULONG_PTR count, const void* caller) {
    if (!rt::startup_trace::enabled()) return;
    static std::mutex mutex;
    static std::set<uint64_t> seen;
    {
        const std::lock_guard lock(mutex);
        if (!seen.insert(attributes).second) return;
    }
    rt::startup_trace::event(
        "xmem_physical",
        "\"api\":\"XMemAllocatePhysicalPages\",\"attributes\":" +
            rt::startup_trace::quote(rt::startup_trace::hex(attributes)) +
            ",\"pages\":" + std::to_string(static_cast<uint64_t>(count)) + ",\"caller\":" +
            rt::startup_trace::quote(rt::startup_trace::where(reinterpret_cast<uint64_t>(caller))));
}

BOOL WINAPI XMemAllocatePhysicalPages(ULONG64 attributes, ULONG_PTR* count, ULONG_PTR* page_array) {
    if (!count || (*count != 0 && !page_array)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const ULONG_PTR wanted = *count;
    if (wanted != 0 && !api().ok()) {
        *count = 0;
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    try {
        note_allocation(attributes, wanted, _ReturnAddress());
        const std::lock_guard lock(g_mutex);
        if (wanted > kMaxSlots || !grow(static_cast<size_t>(wanted))) {
            *count = 0;
            if (wanted > kMaxSlots) SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return FALSE;
        }
        for (ULONG_PTR i = 0; i < wanted; ++i) {
            const uint32_t slot = g_free.back();
            g_free.pop_back();
            Slot& s = g_slots[slot];
            if (s.needs_zero) {
                std::memset(g_chunks[slot / kChunkPages].view + static_cast<size_t>(slot % kChunkPages) * kPage, 0,
                            kPage);
                s.needs_zero = false;
            }
            s.allocated = true;
            page_array[i] = frame_of(slot);
        }
        return TRUE;
    } catch (const std::bad_alloc&) {
        *count = 0;
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    } catch (const std::length_error&) {
        *count = 0;
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
}

BOOL WINAPI XMemFreePhysicalPages(ULONG_PTR count, ULONG_PTR* page_array) {
    if (count != 0 && !page_array) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    try {
        const std::lock_guard lock(g_mutex);
        if (count > g_slots.size()) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        std::vector<uint32_t> slots(static_cast<size_t>(count));
        for (ULONG_PTR i = 0; i < count; ++i) {
            if (!slot_of(page_array[i], slots[i])) {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
        }
        std::vector<uint32_t> sorted = slots;
        std::sort(sorted.begin(), sorted.end());
        if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        g_free.reserve(g_free.size() + slots.size());
        std::vector<Change> changes;
        for (const uint32_t slot : slots) {
            for (const uintptr_t a : g_slots[slot].mapped_at) {
                uintptr_t base = 0;
                size_t index = 0;
                Region* r = region_of(a, 1, base, index);
                if (!r || r->page[index] != slot + 1) {
                    SetLastError(ERROR_GEN_FAILURE);
                    return FALSE;
                }
                changes.push_back({r, base, index, slot + 1, 0});
            }
        }
        if (!apply_changes(changes)) return FALSE;
        // Do not recycle any frame until every alias of every requested frame was successfully unmapped.
        for (const uint32_t slot : slots) {
            Slot& s = g_slots[slot];
            s.allocated = false;
            s.needs_zero = true;
            g_free.push_back(slot);
        }
        return TRUE;
    } catch (const std::bad_alloc&) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    } catch (const std::length_error&) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
}

// Checks every entry before anything changes, so a bad frame number leaves the mappings as they were.
bool resolve_frames(ULONG_PTR count, const ULONG_PTR* page_array, std::vector<uint32_t>& slots) {
    slots.assign(static_cast<size_t>(count), 0);
    if (!page_array) return true;
    for (ULONG_PTR i = 0; i < count; ++i) {
        if (page_array[i] == 0) continue;
        uint32_t slot = 0;
        if (!slot_of(page_array[i], slot)) return false;
        slots[i] = slot + 1;
    }
    return true;
}

PVOID WINAPI XMemMapPhysicalPages(PVOID va, ULONG_PTR count, ULONG_PTR* page_array) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(va);
    if (!api().ok()) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return nullptr;
    }
    if (!va || a % kPage != 0 || count == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    try {
        const std::lock_guard lock(g_mutex);
        uintptr_t base = 0;
        size_t first = 0;
        Region* r = region_of(a, count, base, first);
        std::vector<uint32_t> slots;
        if (!r || !resolve_frames(count, page_array, slots)) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return nullptr;
        }
        std::vector<Change> changes;
        changes.reserve(static_cast<size_t>(count));
        for (ULONG_PTR i = 0; i < count; ++i)
            changes.push_back({r, base, first + i, r->page[first + i], slots[i]});
        return apply_changes(changes) ? va : nullptr;
    } catch (const std::bad_alloc&) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    } catch (const std::length_error&) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
}

BOOL WINAPI XMemMapPhysicalPagesScatter(PVOID* va_array, ULONG_PTR count, ULONG_PTR* page_array) {
    if (!api().ok()) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    if (count != 0 && !va_array) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    try {
        const std::lock_guard lock(g_mutex);
        std::vector<uint32_t> slots;
        if (!resolve_frames(count, page_array, slots)) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        std::vector<Change> changes;
        std::vector<uintptr_t> addresses;
        changes.reserve(static_cast<size_t>(count));
        addresses.reserve(static_cast<size_t>(count));
        for (ULONG_PTR i = 0; i < count; ++i) {
            const uintptr_t a = reinterpret_cast<uintptr_t>(va_array[i]);
            uintptr_t base = 0;
            size_t index = 0;
            Region* r = a % kPage == 0 ? region_of(a, 1, base, index) : nullptr;
            if (!r) {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            changes.push_back({r, base, index, r->page[index], slots[i]});
            addresses.push_back(a);
        }
        std::sort(addresses.begin(), addresses.end());
        if (std::adjacent_find(addresses.begin(), addresses.end()) != addresses.end()) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        return apply_changes(changes) ? TRUE : FALSE;
    } catch (const std::bad_alloc&) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    } catch (const std::length_error&) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
}

void* reserve_physical(SIZE_T size, DWORD protect, DWORD allocation_flags) {
    if (!api().ok()) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) {
            std::fprintf(stderr,
                         "[rt] xmem: no VirtualAlloc2/MapViewOfFile3 placeholders on this host; physical-page "
                         "reservations are unsupported\n");
        }
        SetLastError(ERROR_NOT_SUPPORTED);
        return nullptr;
    }
    if (size == 0 || size > (std::numeric_limits<SIZE_T>::max)() - (kPage - 1) ||
        (allocation_flags & ~MEM_TOP_DOWN) != 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    size = (size + kPage - 1) & ~(kPage - 1);
    try {
        const std::lock_guard lock(g_mutex);
        Region r;
        r.pages = size / kPage;
        r.protect = protect;
        r.page_protect.assign(r.pages, protect);
        r.page.assign(r.pages, 0);
        r.isolated.assign(r.pages, 0);
        r.needs_protect.assign(r.pages, 0);
        // Prepare the map node before obtaining host address space. Inserting a node handle afterwards does
        // not allocate, so a bookkeeping allocation failure cannot leak an untracked reservation.
        std::map<uintptr_t, Region> prepared;
        prepared.emplace(0, std::move(r));
        auto node = prepared.extract(prepared.begin());
        if (fail_host(HostOperation::Reserve, nullptr)) return nullptr;
        void* p = api().alloc2(GetCurrentProcess(), nullptr, size,
                               MEM_RESERVE | MEM_RESERVE_PLACEHOLDER | allocation_flags,
                               PAGE_NOACCESS, nullptr, 0);
        if (!p) return nullptr;
        node.key() = reinterpret_cast<uintptr_t>(p);
        g_regions.insert(std::move(node));
        return p;
    } catch (const std::bad_alloc&) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    } catch (const std::length_error&) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
}

// A reserve-only XMemVirtualAlloc with MEM_REPLACE_PLACEHOLDER leaves
// the existing placeholder in place; it updates each page's requested CPU
// protection. The pagefile view is installed later by XMemMapPhysicalPages.
void* replace_placeholder_window(void* address, SIZE_T size, DWORD protect) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(address);
    if (!address || size == 0 || (a & (kPage - 1)) != 0 ||
        (size & (kPage - 1)) != 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    try {
        const std::lock_guard lock(g_mutex);
        uintptr_t base = 0;
        size_t first = 0;
        Region* r = region_of(a, size / kPage, base, first);
        if (!r) {
            SetLastError(ERROR_INVALID_ADDRESS);
            return nullptr;
        }
        // Do not reconfigure a region after pages have been mapped into it.
        // The title performs these reservations before XMemMapPhysicalPages.
        for (SIZE_T i = 0; i < size / kPage; ++i) {
            if (r->page[first + i] != 0) {
                SetLastError(ERROR_BUSY);
                return nullptr;
            }
        }
        for (SIZE_T i = 0; i < size / kPage; ++i)
            r->page_protect[first + i] = protect;
        return address;
    } catch (const std::bad_alloc&) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    } catch (const std::length_error&) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
}

// Unmaps every page of a physical reservation and releases its placeholders. Called with g_mutex held.
bool release_region(std::map<uintptr_t, Region>::iterator it) {
    const uintptr_t base = it->first;
    Region& r = it->second;
    std::vector<Change> changes;
    for (size_t i = 0; i < r.pages; ++i) {
        if (r.page[i]) changes.push_back({&r, base, i, r.page[i], 0});
    }
    if (!apply_changes(changes)) return false;
    // Releasing fragments one by one can leave holes in a window that remains in our map. Instead, coalesce
    // all empty placeholders, then release once. Any failure retains the complete reservation record, and
    // the previous views are restored where the host allows it.
    if (r.split) {
        if (!free_host(reinterpret_cast<void*>(base), r.pages * kPage,
                       MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS, HostOperation::Coalesce))
            return fail_and_rollback(changes, changes.size(), GetLastError());
        std::fill(r.isolated.begin(), r.isolated.end(), 0);
        r.split = false;
    }
    if (!free_host(reinterpret_cast<void*>(base), 0, MEM_RELEASE, HostOperation::Release))
        return fail_and_rollback(changes, changes.size(), GetLastError());
    g_regions.erase(it);
    return true;
}

// kernel32!VirtualFree as a title calls it: a physical reservation is a set of placeholders and views, which the
// host's VirtualFree(base, 0, MEM_RELEASE) would release only the first of. Everything else goes to the host.
BOOL WINAPI TitleVirtualFree(LPVOID address, SIZE_T size, DWORD free_type) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(address);
    try {
        const std::lock_guard lock(g_mutex);
        auto it = g_regions.upper_bound(a);
        if (it != g_regions.begin()) {
            --it;
            if (a - it->first < it->second.pages * kPage) {
                if (a == it->first && size == 0 && free_type == MEM_RELEASE) {
                    return release_region(it) ? TRUE : FALSE;
                }
                if (free_type == (MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) && size != 0 &&
                    (a & (kPage - 1)) == 0 && (size & (kPage - 1)) == 0 &&
                    size <= it->second.pages * kPage - (a - it->first)) {
                    // The title splits its 185 MiB placeholder into five
                    // smaller windows. The region remains owned and tracked.
                    const size_t first = (a - it->first) / kPage;
                    bool empty = true;
                    for (size_t i = 0; i < size / kPage; ++i)
                        empty &= it->second.page[first + i] == 0;
                    if (!empty) {
                        SetLastError(ERROR_BUSY);
                        return FALSE;
                    }
                    if (!free_host(address, size, free_type, HostOperation::Split)) return FALSE;
                    it->second.split = true;
                    return TRUE;
                }
                // Decommitting or releasing any other part of a mapped physical window is invalid.
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
        }
    } catch (const std::bad_alloc&) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    } catch (const std::length_error&) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    return VirtualFree(address, size, free_type);
}

const bool g_registered = (rt::xmem::set_physical_reserve(&reserve_physical),
                           rt::xmem::set_placeholder_replace(&replace_placeholder_window), true);

}  // namespace

namespace rt::xmem::testing {
void set_failure_hook(FailureHook hook) { g_failure_hook.store(hook, std::memory_order_release); }
}  // namespace rt::xmem::testing

#define TITLE_VIRTUAL_FREE(dll) {dll, "VirtualFree", reinterpret_cast<void*>(&TitleVirtualFree)}

SHIM_TABLE(xmem_physical,
    {"xmem.dll", "XMemAllocatePhysicalPages", reinterpret_cast<void*>(&XMemAllocatePhysicalPages)},
    {"xmem.dll", "XMemFreePhysicalPages", reinterpret_cast<void*>(&XMemFreePhysicalPages)},
    {"xmem.dll", "XMemMapPhysicalPages", reinterpret_cast<void*>(&XMemMapPhysicalPages)},
    {"xmem.dll", "XMemMapPhysicalPagesScatter", reinterpret_cast<void*>(&XMemMapPhysicalPagesScatter)},
    TITLE_VIRTUAL_FREE("kernel32.dll"),
    TITLE_VIRTUAL_FREE("kernelbase.dll"),
    TITLE_VIRTUAL_FREE("api-ms-win-core-memory-l1-1-0.dll"),
    TITLE_VIRTUAL_FREE("api-ms-win-core-memory-l1-1-1.dll"),
    TITLE_VIRTUAL_FREE("api-ms-win-core-memory-l1-1-2.dll"),
    TITLE_VIRTUAL_FREE("api-ms-win-core-memory-l1-1-3.dll"),
    TITLE_VIRTUAL_FREE("api-ms-win-core-memory-l1-1-4.dll"),
    TITLE_VIRTUAL_FREE("api-ms-win-core-memory-l1-1-5.dll"),
    TITLE_VIRTUAL_FREE("api-ms-win-core-memory-l1-1-6.dll"),
);
