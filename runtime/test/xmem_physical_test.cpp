// xmem.dll's physical pages through the registered shims, as a title calls them: XMemAllocatePhysicalPages,
// XMemFreePhysicalPages, XMemMapPhysicalPages, XMemMapPhysicalPagesScatter, the 64 KiB-page reserve-only
// XMemVirtualAlloc they are mapped into and the title's VirtualFree that releases it. The call shapes are the ones in
// xmem_physical.cpp's header comment; everything here is synthetic.
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <vector>

#include "shims.h"
#include "xmem_physical.h"

namespace {

using XMemVirtualAllocFn = void*(WINAPI*)(void*, SIZE_T, DWORD, ULONGLONG, DWORD);
using AllocatePhysicalFn = BOOL(WINAPI*)(ULONG64, ULONG_PTR*, ULONG_PTR*);
using FreePhysicalFn = BOOL(WINAPI*)(ULONG_PTR, ULONG_PTR*);
using MapPhysicalFn = PVOID(WINAPI*)(PVOID, ULONG_PTR, ULONG_PTR*);
using MapScatterFn = BOOL(WINAPI*)(PVOID*, ULONG_PTR, ULONG_PTR*);
using VirtualFreeFn = BOOL(WINAPI*)(LPVOID, SIZE_T, DWORD);

constexpr SIZE_T kPage = 0x10000;
constexpr DWORD kPhysicalReserve = MEM_RESERVE | 0x20400000;  // MEM_LARGE_PAGES | MEM_PHYSICAL: 64 KiB pages
constexpr DWORD kGraphicsNoAccess = 0x800;
constexpr DWORD kGraphicsReadWrite = 0x2000;

int g_checks = 0;
int g_failures = 0;

bool check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
    return condition;
}

struct Api {
    XMemVirtualAllocFn virtual_alloc;
    AllocatePhysicalFn allocate;
    FreePhysicalFn free_pages;
    MapPhysicalFn map;
    MapScatterFn scatter;
    VirtualFreeFn virtual_free;
};

uint8_t* at(void* base, size_t page) { return static_cast<uint8_t*>(base) + page * kPage; }

MEMORY_BASIC_INFORMATION query(const void* p) {
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(p, &mbi, sizeof(mbi));
    return mbi;
}

bool committed(const void* p) { return query(p).State == MEM_COMMIT; }

void fill(uint8_t* p, uint8_t value) {
    const auto mbi = query(p);
    if (mbi.State != MEM_COMMIT || (mbi.Protect & 0xff) != PAGE_READWRITE) {
        check(false, "test setup needs a writable mapped page");
        return;
    }
    std::memset(p, value, kPage);
}

bool all_equal(const uint8_t* p, uint8_t value) {
    const auto mbi = query(p);
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;
    return std::all_of(p, p + kPage, [value](uint8_t b) { return b == value; });
}

// Faults are injected before a real Windows operation. The rest of each test uses actual pagefile sections and
// mappings; this is not a fake memory implementation. The callback only updates test counters and never re-enters
// the store. The tests run serially in this process.
using HostOperation = rt::xmem::testing::HostOperation;
constexpr size_t kHostOperationCount = static_cast<size_t>(HostOperation::Query) + 1;

struct Failure {
    HostOperation operation;
    unsigned occurrence;
    DWORD error;
    void* address = nullptr;  // null matches every address
};

std::array<unsigned, kHostOperationCount> g_host_calls{};
std::array<Failure, 8> g_injected_failures{};
size_t g_injected_count = 0;

DWORD inject_failure(HostOperation operation, void* address) {
    const auto occurrence = ++g_host_calls[static_cast<size_t>(operation)];
    for (size_t i = 0; i < g_injected_count; ++i) {
        const auto& failure = g_injected_failures[i];
        if (operation == failure.operation && occurrence == failure.occurrence &&
            (!failure.address || address == failure.address)) {
            return failure.error;
        }
    }
    return 0;
}

struct InjectFailures {
    explicit InjectFailures(std::initializer_list<Failure> failures) {
        g_host_calls.fill(0);
        g_injected_count = failures.size();
        std::copy(failures.begin(), failures.end(), g_injected_failures.begin());
        rt::xmem::testing::set_failure_hook(&inject_failure);
    }
    ~InjectFailures() { rt::xmem::testing::set_failure_hook(nullptr); }
    InjectFailures(const InjectFailures&) = delete;
    InjectFailures& operator=(const InjectFailures&) = delete;

    unsigned calls(HostOperation operation) const { return g_host_calls[static_cast<size_t>(operation)]; }
};

// New regression cases use this fixture so a failed assertion can return without leaving test-owned mappings in the
// next case. Free individually: a case may deliberately have freed one of the frames already.
struct TestMemory {
    const Api& x;
    std::vector<ULONG_PTR> frames;
    std::vector<void*> regions;

    explicit TestMemory(const Api& api) : x(api) {}
    ~TestMemory() {
        rt::xmem::testing::set_failure_hook(nullptr);
        for (auto frame : frames) {
            if (frame) x.free_pages(1, &frame);
        }
        for (void* region : regions) {
            if (region) x.virtual_free(region, 0, MEM_RELEASE);
        }
    }

    bool allocate(ULONG_PTR count) {
        frames.assign(count, 0);
        ULONG_PTR actual = count;
        return check(x.allocate(0x280, &actual, frames.data()) == TRUE && actual == count,
                     "regression setup allocates the requested backing pages");
    }

    void* reserve(size_t pages, DWORD protect = PAGE_READWRITE) {
        void* result = x.virtual_alloc(nullptr, pages * kPage, kPhysicalReserve, 0x21, protect);
        check(result != nullptr, "regression setup reserves a physical window");
        if (result) regions.push_back(result);
        return result;
    }

    void forget_region(void* region) {
        std::replace(regions.begin(), regions.end(), region, static_cast<void*>(nullptr));
    }
};

void test_allocate(const Api& x) {
    ULONG_PTR count = 4;
    ULONG_PTR frames[4] = {};
    check(x.allocate(0x280, &count, frames) == TRUE, "XMemAllocatePhysicalPages(0x280, 4) returns TRUE");
    check(count == 4, "the page count comes back unchanged");
    bool shape = true;
    for (const ULONG_PTR f : frames) shape &= f != 0 && f % 16 == 0 && (f & ~0x1FFFFF0ull) == 0;
    check(shape, "every frame number is a nonzero multiple of 16 inside 25 bits");
    std::vector<ULONG_PTR> sorted(frames, frames + 4);
    std::sort(sorted.begin(), sorted.end());
    check(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end(), "the four frame numbers differ");

    ULONG_PTR none = 0;
    check(x.allocate(0x180, &none, nullptr) == TRUE && none == 0, "a count of 0 succeeds with no array");
    check(x.allocate(0x280, nullptr, frames) == FALSE && GetLastError() == ERROR_INVALID_PARAMETER,
          "a null count is ERROR_INVALID_PARAMETER");
    check(x.free_pages(4, frames) == TRUE, "XMemFreePhysicalPages frees them");
    check(x.free_pages(4, frames) == FALSE && GetLastError() == ERROR_INVALID_PARAMETER,
          "freeing them a second time is ERROR_INVALID_PARAMETER");
}

void test_map(const Api& x) {
    void* rw = x.virtual_alloc(nullptr, 16 * kPage, kPhysicalReserve, 0x21, PAGE_READWRITE);
    void* alias = x.virtual_alloc(nullptr, 16 * kPage, kPhysicalReserve, 0x23, PAGE_READWRITE | kGraphicsReadWrite);
    void* gpu_only = x.virtual_alloc(nullptr, 16 * kPage, kPhysicalReserve, 0x3, PAGE_NOACCESS | kGraphicsNoAccess);
    check(rw && alias && gpu_only, "three 64 KiB-page physical reservations (attributes 0x21, 0x23, 0x3)");
    if (!rw || !alias || !gpu_only) return;
    check(reinterpret_cast<uintptr_t>(rw) % kPage == 0, "a physical reservation is 64 KiB aligned");
    check(!committed(rw), "nothing is accessible in a reservation before a page is mapped");

    ULONG_PTR count = 4;
    ULONG_PTR frames[4] = {};
    check(x.allocate(0x380, &count, frames) == TRUE, "allocate four pages");

    check(x.map(rw, 4, frames) == rw, "XMemMapPhysicalPages returns the address it mapped at");
    bool rw_ok = true;
    for (size_t i = 0; i < 4; ++i) rw_ok &= committed(at(rw, i)) && query(at(rw, i)).Protect == PAGE_READWRITE;
    check(rw_ok, "the four mapped pages are read-write");
    check(all_equal(at(rw, 0), 0) && all_equal(at(rw, 3), 0), "new pages read as zero");
    for (size_t i = 0; i < 4; ++i) fill(at(rw, i), static_cast<uint8_t>(0x10 + i));
    check(!committed(at(rw, 4)), "the page after the mapped range stays unmapped");

    // The same pages at a second address, in another order: both views see one set of bytes.
    ULONG_PTR reversed[4] = {frames[3], frames[2], frames[1], frames[0]};
    check(x.map(at(alias, 8), 4, reversed) == at(alias, 8), "the same pages map at a second address");
    check(all_equal(at(alias, 8), 0x13) && all_equal(at(alias, 11), 0x10), "the second view reads the first's bytes");
    fill(at(alias, 9), 0x77);
    check(all_equal(at(rw, 2), 0x77), "a write through the second view shows in the first");

    check(x.map(gpu_only, 1, frames) == gpu_only, "a page maps into the PAGE_NOACCESS reservation");
    check(committed(gpu_only) && query(gpu_only).Protect == PAGE_NOACCESS,
          "there it keeps the CPU protection, PAGE_NOACCESS (the graphics bit is dropped)");

    // A bad frame number leaves the range as it was.
    ULONG_PTR bad[2] = {frames[0], 17};
    check(x.map(at(rw, 0), 2, bad) == nullptr && GetLastError() == ERROR_INVALID_PARAMETER,
          "a frame number that was never handed out is ERROR_INVALID_PARAMETER");
    check(all_equal(at(rw, 1), 0x11), "and the mapping it would have replaced is untouched");
    check(x.map(at(rw, 14), 4, frames) == nullptr, "a range past the reservation's end fails");
    static uint8_t not_reserved[2 * kPage];
    void* outside = reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(not_reserved) + kPage - 1) & ~(kPage - 1));
    check(x.map(outside, 1, frames) == nullptr, "an address outside every physical reservation fails");
    check(x.map(at(rw, 0) + 0x1000, 1, frames) == nullptr, "an address that is not 64 KiB aligned fails");

    // Remapping a page replaces what was there.
    check(x.map(at(rw, 0), 1, &frames[3]) == at(rw, 0) && all_equal(at(rw, 0), 0x13),
          "mapping another page over a mapped one replaces it");

    check(x.map(at(rw, 0), 4, nullptr) == at(rw, 0), "a null page array unmaps the range");
    check(!committed(at(rw, 0)) && !committed(at(rw, 3)), "the unmapped pages are inaccessible again");
    check(all_equal(at(alias, 11), 0x10), "unmapping one view leaves the other");
    check(x.map(at(rw, 0), 1, &frames[0]) == at(rw, 0) && all_equal(at(rw, 0), 0x10),
          "a page maps again where it was unmapped");

    // Scatter: listed addresses, 0 entries unmap.
    void* vas[3] = {at(rw, 5), at(rw, 12), at(rw, 0)};
    ULONG_PTR scatter_frames[3] = {frames[1], frames[2], 0};
    check(x.scatter(vas, 3, scatter_frames) == TRUE, "XMemMapPhysicalPagesScatter returns TRUE");
    check(all_equal(at(rw, 5), 0x11) && all_equal(at(rw, 12), 0x77), "it maps each page at its own address");
    check(!committed(at(rw, 0)), "a 0 entry unmaps its address");
    check(x.scatter(vas, 2, nullptr) == TRUE && !committed(at(rw, 5)) && !committed(at(rw, 12)),
          "a null page array unmaps every listed address");

    // Freeing a page unmaps it wherever it is, and it comes back zeroed.
    check(x.free_pages(4, frames) == TRUE, "free the four pages while they are mapped");
    check(!committed(at(alias, 8)) && !committed(at(alias, 11)) && !committed(gpu_only),
          "freeing unmaps the pages in every reservation");
    ULONG_PTR again_count = 4;
    ULONG_PTR again[4] = {};
    check(x.allocate(0x280, &again_count, again) == TRUE, "allocate four pages again");
    check(x.map(rw, 4, again) == rw, "map them");
    bool zero = true;
    for (size_t i = 0; i < 4; ++i) zero &= all_equal(at(rw, i), 0);
    check(zero, "reused pages read as zero");

    ULONG_PTR twice[2] = {again[0], again[0]};
    check(x.free_pages(2, twice) == FALSE && GetLastError() == ERROR_INVALID_PARAMETER,
          "freeing one frame number twice in one call is refused");
    check(committed(rw), "and frees nothing");

    check(x.virtual_free(at(rw, 1), 0, MEM_RELEASE) == FALSE, "VirtualFree inside a physical reservation fails");
    check(x.virtual_free(rw, 0, MEM_RELEASE) == TRUE, "VirtualFree(base, 0, MEM_RELEASE) releases it");
    check(query(rw).State == MEM_FREE && query(at(rw, 7)).State == MEM_FREE && query(at(rw, 15)).State == MEM_FREE,
          "the whole range is free afterwards, views and split placeholders included");
    check(x.map(rw, 1, again) == nullptr, "nothing maps into a released reservation");
    check(x.free_pages(4, again) == TRUE, "the pages that were mapped there can still be freed");
    check(x.virtual_free(alias, 0, MEM_RELEASE) == TRUE && x.virtual_free(gpu_only, 0, MEM_RELEASE) == TRUE,
          "release the other two");

    // Plain memory still goes to the host.
    void* plain = VirtualAlloc(nullptr, kPage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    check(plain && x.virtual_free(plain, 0, MEM_RELEASE) == TRUE && query(plain).State == MEM_FREE,
          "VirtualFree of ordinary memory is the host's");
}

// The startup reservations are 24 GiB; the page tables of a reservation that size must not cost more than a few MB,
// and a page at its far end maps like any other.
void test_large_reservation(const Api& x) {
    constexpr SIZE_T size = 24ull << 30;
    void* big = x.virtual_alloc(nullptr, size, kPhysicalReserve, 0x21, PAGE_READWRITE);
    check(big != nullptr, "a 24 GiB physical reservation");
    if (!big) return;
    ULONG_PTR count = 1;
    ULONG_PTR frame = 0;
    check(x.allocate(0x280, &count, &frame) == TRUE, "allocate one page");
    uint8_t* last = static_cast<uint8_t*>(big) + size - kPage;
    if (!check(x.map(last, 1, &frame) == last, "it maps at the last page of the reservation")) {
        x.free_pages(1, &frame);
        x.virtual_free(big, 0, MEM_RELEASE);
        return;
    }
    last[kPage - 1] = 0x5a;
    check(last[kPage - 1] == 0x5a, "and is writable there");
    check(x.free_pages(1, &frame) == TRUE && x.virtual_free(big, 0, MEM_RELEASE) == TRUE, "free and release");
}

// Many pages at once, as the startup pools ask for (a few hundred MB each).
void test_bulk(const Api& x) {
    constexpr ULONG_PTR pages = 2048;  // 128 MiB, four sections
    void* region = x.virtual_alloc(nullptr, pages * kPage, kPhysicalReserve, 0x23, PAGE_READWRITE | kGraphicsReadWrite);
    std::vector<ULONG_PTR> frames(pages);
    ULONG_PTR count = pages;
    const bool allocated = region && x.allocate(0x280, &count, frames.data()) == TRUE && count == pages;
    if (!check(allocated, "allocate 2048 pages")) {
        if (region) x.virtual_free(region, 0, MEM_RELEASE);
        return;
    }
    if (!check(x.map(region, pages, frames.data()) == region, "map 128 MiB in one call")) {
        x.free_pages(pages, frames.data());
        x.virtual_free(region, 0, MEM_RELEASE);
        return;
    }
    bool ok = true;
    for (ULONG_PTR i = 0; i < pages; ++i) at(region, i)[0] = static_cast<uint8_t>(i);
    for (ULONG_PTR i = 0; i < pages; ++i) ok &= at(region, i)[0] == static_cast<uint8_t>(i);
    check(ok, "every page holds its own byte");
    check(x.free_pages(pages, frames.data()) == TRUE && x.virtual_free(region, 0, MEM_RELEASE) == TRUE,
          "free and release");
}

void test_input_boundaries(const Api& x) {
    TestMemory memory(x);
    if (!memory.allocate(3)) return;
    void* region = memory.reserve(4);
    if (!region) return;

    ULONG_PTR too_many = ULONG_PTR{1} << 21;
    ULONG_PTR untouched = 0x12345678;
    check(x.allocate(0x280, &too_many, &untouched) == FALSE && too_many == 0 &&
              GetLastError() == ERROR_NOT_ENOUGH_MEMORY && untouched == 0x12345678,
          "an allocation beyond the 25-bit frame space fails without writing its output array");
    ULONG_PTR one = 1;
    check(x.allocate(0x280, &one, nullptr) == FALSE && GetLastError() == ERROR_INVALID_PARAMETER,
          "a nonempty physical allocation requires an output array");
    check(x.free_pages(0, nullptr) == TRUE && x.scatter(nullptr, 0, nullptr) == TRUE,
          "empty free and scatter calls are no-ops");
    // A UINT32 declaration would truncate this to zero and incorrectly accept it as the empty call above.
    check(x.scatter(nullptr, ULONG_PTR{1} << 32, nullptr) == FALSE && GetLastError() == ERROR_INVALID_PARAMETER,
          "scatter preserves its pointer-width count instead of truncating the high 32 bits");
    check(x.map(region, (std::numeric_limits<ULONG_PTR>::max)(), memory.frames.data()) == nullptr,
          "a contiguous count exceeding the window is rejected before indexing the frame array");

    ULONG_PTR repeated[] = {memory.frames[0], memory.frames[0]};
    if (!check(x.map(region, 2, repeated) == region, "one physical frame may occur twice in a mapping batch")) return;
    fill(at(region, 0), 0x57);
    check(all_equal(at(region, 1), 0x57), "duplicate frame entries at distinct addresses are real aliases");

    ULONG_PTR invalid_free[] = {memory.frames[0], 17};
    check(x.free_pages(2, invalid_free) == FALSE && GetLastError() == ERROR_INVALID_PARAMETER,
          "a bad later free entry rejects the entire input before releasing the first frame");
    check(all_equal(at(region, 0), 0x57) && all_equal(at(region, 1), 0x57),
          "both aliases survive invalid free input");
    check(x.free_pages(2, repeated) == FALSE && GetLastError() == ERROR_INVALID_PARAMETER,
          "duplicate entries are invalid in a free batch");

    void* duplicate_targets[] = {region, region};
    ULONG_PTR replacements[] = {memory.frames[1], memory.frames[2]};
    check(x.scatter(duplicate_targets, 2, replacements) == FALSE && GetLastError() == ERROR_INVALID_PARAMETER,
          "a scatter batch cannot describe the same destination twice");
    check(all_equal(at(region, 0), 0x57), "duplicate scatter targets leave the original mapping intact");

    void* unaligned_targets[] = {region, at(region, 2) + 0x1000};
    check(x.scatter(unaligned_targets, 2, replacements) == FALSE && GetLastError() == ERROR_INVALID_PARAMETER,
          "every scatter address is validated before the first mapping changes");
    void* valid_targets[] = {region, at(region, 2)};
    ULONG_PTR invalid_frames[] = {memory.frames[1], ULONG_PTR{1} << 25};
    check(x.scatter(valid_targets, 2, invalid_frames) == FALSE && GetLastError() == ERROR_INVALID_PARAMETER,
          "every scatter frame is validated, including the upper frame bound");
    check(all_equal(at(region, 0), 0x57) && !committed(at(region, 2)),
          "invalid scatter input leaves both existing and empty destinations unchanged");

    check(x.virtual_free(region, kPage, MEM_RELEASE) == FALSE && GetLastError() == ERROR_INVALID_PARAMETER,
          "whole-region release requires a zero size");
    check(x.virtual_free(region, kPage, MEM_DECOMMIT) == FALSE,
          "ordinary decommit cannot invalidate physical-store bookkeeping");
    check(all_equal(at(region, 1), 0x57), "invalid release and decommit preserve the aliases");
}

void test_reserve_and_split_failures(const Api& x) {
    {
        InjectFailures failure{{HostOperation::Reserve, 1, ERROR_NOT_ENOUGH_MEMORY}};
        check(x.virtual_alloc(nullptr, 4 * kPage, kPhysicalReserve, 0x21, PAGE_READWRITE) == nullptr &&
                  GetLastError() == ERROR_NOT_ENOUGH_MEMORY,
              "a failed host placeholder reservation is reported as failure");
        check(failure.calls(HostOperation::Reserve) == 1, "the reserve failure was actually injected");
    }

    TestMemory memory(x);
    if (!memory.allocate(1)) return;
    void* region = memory.reserve(4);
    if (!region) return;
    {
        InjectFailures failure{{HostOperation::Split, 1, ERROR_BUSY}};
        check(x.map(at(region, 1), 1, memory.frames.data()) == nullptr && GetLastError() == ERROR_BUSY,
              "a failed placeholder split preserves the host error");
        check(failure.calls(HostOperation::Split) == 1, "the split failure was actually injected");
    }
    check(query(at(region, 1)).State == MEM_RESERVE, "failed splitting leaves the destination reserved");
    check(x.map(at(region, 1), 1, memory.frames.data()) == at(region, 1) && all_equal(at(region, 1), 0),
          "the same frame and reservation remain usable after the split failure");
}

void test_batch_rollback(const Api& x) {
    TestMemory memory(x);
    if (!memory.allocate(4)) return;
    void* region = memory.reserve(8);
    void* backing_alias = memory.reserve(4);
    if (!region || !backing_alias) return;
    if (!check(x.map(backing_alias, 4, memory.frames.data()) == backing_alias,
               "prepare independent aliases for the rollback test")) return;
    for (size_t i = 0; i < 4; ++i) fill(at(backing_alias, i), static_cast<uint8_t>(0x31 + i));
    if (!check(x.map(region, 2, memory.frames.data()) == region, "prepare two original mappings")) return;

    {
        InjectFailures failure{{HostOperation::Map, 2, ERROR_NOT_ENOUGH_MEMORY}};
        check(x.map(region, 2, memory.frames.data() + 2) == nullptr && GetLastError() == ERROR_NOT_ENOUGH_MEMORY,
              "a second-page mapping failure reports the first host error after rollback");
        check(failure.calls(HostOperation::Map) >= 2, "the later map failure was actually injected");
    }
    check(all_equal(at(region, 0), 0x31) && all_equal(at(region, 1), 0x32),
          "contiguous rollback restores both old mappings and their contents");
    check(all_equal(at(backing_alias, 2), 0x33) && all_equal(at(backing_alias, 3), 0x34),
          "failed remapping does not modify either replacement page's backing");

    if (!check(x.map(at(region, 5), 1, memory.frames.data() + 1) == at(region, 5),
               "prepare a distant original mapping for scatter")) return;
    void* targets[] = {region, at(region, 5)};
    {
        InjectFailures failure{{HostOperation::Map, 2, ERROR_NOT_ENOUGH_MEMORY}};
        check(x.scatter(targets, 2, memory.frames.data() + 2) == FALSE &&
                  GetLastError() == ERROR_NOT_ENOUGH_MEMORY,
              "a later scatter map failure is reported after rollback");
    }
    check(all_equal(at(region, 0), 0x31) && all_equal(at(region, 5), 0x32),
          "scatter rollback restores both old mappings");
    {
        InjectFailures failure{{HostOperation::Unmap, 2, ERROR_BUSY}};
        check(x.scatter(targets, 2, nullptr) == FALSE && GetLastError() == ERROR_BUSY,
              "a later scatter unmap failure is reported after rollback");
    }
    check(all_equal(at(region, 0), 0x31) && all_equal(at(region, 5), 0x32),
          "failed scatter unmapping also restores every old mapping");
    check(x.scatter(targets, 2, memory.frames.data() + 2) == TRUE && all_equal(at(region, 0), 0x33) &&
              all_equal(at(region, 5), 0x34),
          "the scatter batch succeeds when retried without the injected fault");
}

void test_free_failure(const Api& x) {
    TestMemory memory(x);
    if (!memory.allocate(2)) return;
    void* region = memory.reserve(8);
    if (!region) return;
    ULONG_PTR original[] = {memory.frames[0], memory.frames[1]};
    ULONG_PTR aliases[] = {original[0], original[0], original[1]};
    if (!check(x.map(region, 3, aliases) == region, "prepare aliased pages for a failed free")) return;
    fill(at(region, 0), 0x6a);
    fill(at(region, 2), 0x6b);
    {
        InjectFailures failure{{HostOperation::Unmap, 2, ERROR_BUSY}};
        check(x.free_pages(2, original) == FALSE && GetLastError() == ERROR_BUSY,
              "free reports a failed alias unmap without retiring either physical frame");
    }
    check(all_equal(at(region, 0), 0x6a) && all_equal(at(region, 1), 0x6a) && all_equal(at(region, 2), 0x6b),
          "a failed free rolls all previous alias removals back");

    ULONG_PTR count = 1;
    ULONG_PTR fresh = 0;
    if (!check(x.allocate(0x280, &count, &fresh) == TRUE && count == 1, "allocate after the failed free")) return;
    memory.frames.push_back(fresh);
    check(fresh != original[0] && fresh != original[1], "frames from a failed free are never reused");
    check(x.map(at(region, 4), 2, original) == at(region, 4) && all_equal(at(region, 4), 0x6a) &&
              all_equal(at(region, 5), 0x6b),
          "both original frames remain allocated and keep their data");
    const bool freed = check(x.free_pages(2, original) == TRUE,
                            "free succeeds on retry after the host fault is removed");
    check(!committed(at(region, 0)) && !committed(at(region, 1)) && !committed(at(region, 2)) &&
              !committed(at(region, 4)) && !committed(at(region, 5)),
          "retrying the free removes every original and newly added alias");
    if (freed) memory.frames[0] = memory.frames[1] = 0;
}

void test_release_failure(const Api& x, HostOperation operation, const char* description) {
    TestMemory memory(x);
    if (!memory.allocate(2)) return;
    void* region = memory.reserve(8);
    void* other = memory.reserve(2);
    if (!region || !other) return;
    if (!check(x.map(other, 2, memory.frames.data()) == other &&
                   x.map(region, 1, memory.frames.data()) == region &&
                   x.map(at(region, 7), 1, memory.frames.data() + 1) == at(region, 7),
               "prepare split placeholders and surviving aliases for release failure")) return;
    fill(at(other, 0), 0x71);
    fill(at(other, 1), 0x72);
    {
        InjectFailures failure{{operation, 1, ERROR_BUSY}};
        check(x.virtual_free(region, 0, MEM_RELEASE) == FALSE && GetLastError() == ERROR_BUSY, description);
        check(failure.calls(operation) >= 1, "the requested release-path failure was actually injected");
    }
    check(query(region).State != MEM_FREE && query(at(region, 7)).State != MEM_FREE,
          "failed release keeps ownership of the complete reservation");
    check(x.map(at(region, 3), 1, memory.frames.data()) == at(region, 3) && all_equal(at(region, 3), 0x71),
          "retained reservation metadata supports mapping after a failed release");
    const bool released = check(x.virtual_free(region, 0, MEM_RELEASE) == TRUE,
                                "whole-region release succeeds when retried");
    if (released) memory.forget_region(region);
    check(query(region).State == MEM_FREE && query(at(region, 3)).State == MEM_FREE &&
              query(at(region, 7)).State == MEM_FREE,
          "retry releases the entire split or coalesced reservation");
    check(all_equal(at(other, 0), 0x71) && all_equal(at(other, 1), 0x72),
          "releasing virtual addresses preserves the physical backing and other aliases");
}

void test_rollback_failure(const Api& x) {
    TestMemory memory(x);
    if (!memory.allocate(2)) return;
    void* region = memory.reserve(4);
    void* other = memory.reserve(2);
    if (!region || !other) return;
    if (!check(x.map(other, 2, memory.frames.data()) == other &&
                   x.map(region, 1, memory.frames.data()) == region,
               "prepare backing for a host failure during rollback")) return;
    fill(at(other, 0), 0x81);
    fill(at(other, 1), 0x82);
    {
        InjectFailures failure{{HostOperation::Map, 1, ERROR_NOT_ENOUGH_MEMORY},
                               {HostOperation::Map, 2, ERROR_ACCESS_DENIED}};
        check(x.map(region, 1, memory.frames.data() + 1) == nullptr && GetLastError() == ERROR_GEN_FAILURE,
              "a second host failure during rollback is reported distinctly");
        check(failure.calls(HostOperation::Map) >= 2, "both the original map and its rollback were attempted");
    }
    check(all_equal(at(other, 0), 0x81) && all_equal(at(other, 1), 0x82),
          "failed rollback does not release or overwrite either physical backing page");
    check(x.map(region, 2, memory.frames.data()) == region && all_equal(at(region, 0), 0x81) &&
              all_equal(at(region, 1), 0x82),
          "truthful bookkeeping permits recovery after rollback itself failed");
    const bool freed = x.free_pages(2, memory.frames.data()) == TRUE;
    check(freed && !committed(region) && !committed(at(region, 1)) && !committed(other) && !committed(at(other, 1)),
          "all aliases, including recovered mappings, are still found when backing pages are freed");
    if (freed) memory.frames.clear();
}

void test_noaccess_protection_failure(const Api& x) {
    TestMemory memory(x);
    if (!memory.allocate(1)) return;
    void* region = memory.reserve(4, PAGE_NOACCESS);
    void* alias = memory.reserve(1);
    if (!region || !alias) return;
    if (!check(x.map(alias, 1, memory.frames.data()) == alias, "prepare readable backing for NOACCESS failure")) return;
    fill(at(alias, 0), 0x93);
    {
        // Force the documented fallback even on a host that can map PAGE_NOACCESS directly.
        InjectFailures failure{{HostOperation::Map, 1, ERROR_INVALID_PARAMETER},
                               {HostOperation::Protect, 1, ERROR_ACCESS_DENIED}};
        check(x.map(at(region, 1), 1, memory.frames.data()) == nullptr && GetLastError() == ERROR_ACCESS_DENIED,
              "a failed NOACCESS protection change is a mapping failure");
        check(failure.calls(HostOperation::Protect) == 1, "the NOACCESS fallback protection failure was exercised");
    }
    check(query(at(region, 1)).State == MEM_RESERVE,
          "failed NOACCESS fallback removes the temporary writable view");
    check(all_equal(at(alias, 0), 0x93), "protection failure preserves the shared physical backing");
    check(x.map(at(region, 1), 1, memory.frames.data()) == at(region, 1) &&
              query(at(region, 1)).Protect == PAGE_NOACCESS,
          "a retry establishes the requested NOACCESS mapping without weakening protection");

    // If cleanup also fails, the temporary writable alias still exists. It must remain owned, and retrying the
    // same frame must repair its protection rather than return success merely because the frame number matches.
    {
        InjectFailures failure{{HostOperation::Map, 1, ERROR_INVALID_PARAMETER},
                               {HostOperation::Protect, 1, ERROR_ACCESS_DENIED},
                               {HostOperation::Unmap, 1, ERROR_BUSY}};
        check(x.map(at(region, 2), 1, memory.frames.data()) == nullptr && GetLastError() == ERROR_GEN_FAILURE,
              "failed NOACCESS protection and failed cleanup report incomplete rollback");
        check(failure.calls(HostOperation::Protect) == 1 && failure.calls(HostOperation::Unmap) == 1,
              "both protection and cleanup failures were actually injected");
    }
    check(committed(at(region, 2)) && query(at(region, 2)).Protect == PAGE_READWRITE &&
              all_equal(at(region, 2), 0x93),
          "the surviving temporary alias still refers to the allocated backing page");
    ULONG_PTR count = 1;
    ULONG_PTR fresh = 0;
    if (!check(x.allocate(0x280, &count, &fresh) == TRUE && count == 1,
               "allocate another frame after incomplete protection cleanup")) return;
    memory.frames.push_back(fresh);
    check(fresh != memory.frames[0], "the live frame from incomplete cleanup has not been recycled");
    check(x.map(at(region, 2), 1, memory.frames.data()) == at(region, 2) &&
              query(at(region, 2)).Protect == PAGE_NOACCESS,
          "retrying the same frame repairs the outstanding NOACCESS protection");
    const bool freed = check(x.free_pages(1, memory.frames.data()) == TRUE,
                            "freeing the recovered frame succeeds");
    check(!committed(alias) && !committed(at(region, 1)) && !committed(at(region, 2)),
          "free finds every alias, including the view retained after failed cleanup");
    if (freed) memory.frames[0] = 0;
}

// The title's first descriptor pool: 185 MiB MEM_RESERVE_PLACEHOLDER, then
// VirtualFree(PRESERVE_PLACEHOLDER) splits and XMemVirtualAlloc replaces a
// physical-page window. All operations use the *registered shims*, not direct
// mocks, and the backing page must be actually readable/writable.
void test_descriptor_placeholder_chain(const Api& x) {
    constexpr SIZE_T full = 185ull * 1024 * 1024;
    constexpr DWORD placeholder_flags = MEM_RESERVE | MEM_RESERVE_PLACEHOLDER;
    constexpr DWORD replace_flags = MEM_RESERVE | MEM_REPLACE_PLACEHOLDER | 0x20400000;
    // The real request reserves address space only: it must not commit 185 MiB.
    void* large = x.virtual_alloc(nullptr, full, placeholder_flags, 0x7, PAGE_NOACCESS);
    if (!check(large != nullptr, "185 MiB Xbox descriptor placeholder is reserved")) return;
    check(query(large).State == MEM_RESERVE, "large placeholder does not commit RAM");
    check(reinterpret_cast<uintptr_t>(large) % kPage == 0, "large placeholder is 64 KiB aligned");
    check(x.virtual_free(large, 0, MEM_RELEASE) == TRUE,
          "the 185 MiB placeholder can be released through the mapped title shim");

    TestMemory memory(x);
    void* region = x.virtual_alloc(nullptr, 8 * kPage, placeholder_flags, 0x7, PAGE_NOACCESS);
    if (!check(region != nullptr, "eight-page descriptor placeholder reserved")) return;
    memory.regions.push_back(region);
    void* target = at(region, 4);
    if (!check(x.virtual_free(target, kPage, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) == TRUE,
               "title VirtualFree splits the 64 KiB middle placeholder")) return;
    check(query(target).State == MEM_RESERVE, "split placeholder remains reserved");
    check(x.virtual_alloc(target, kPage, replace_flags, 0x27, PAGE_READWRITE | kGraphicsReadWrite) == target,
          "MEM_REPLACE_PLACEHOLDER retains and reconfigures the existing window");
    if (!memory.allocate(1)) return;
    if (!check(x.map(target, 1, memory.frames.data()) == target,
               "physical page maps into the split descriptor window")) return;
    check(query(target).State == MEM_COMMIT && query(target).Protect == PAGE_READWRITE,
          "the replacement window uses its read/write protection");
    if (committed(target)) {
        at(region, 4)[0] = 0xAC;
        check(at(region, 4)[0] == 0xAC, "mapped descriptor backing is writable");
    }
    check(x.map(target, 1, nullptr) == target, "mapped page can be unmapped into placeholder");
    check(x.virtual_free(region, 0, MEM_RELEASE) == TRUE,
          "split descriptor reservation coalesces and releases as one region");
    memory.forget_region(region);
}

}  // namespace

int main() {
    std::printf("xmem_physical_test: xmem.dll physical pages\n");
    Api x{};
    x.virtual_alloc = reinterpret_cast<XMemVirtualAllocFn>(rt::shims::find("xmem.dll", "XMemVirtualAlloc"));
    x.allocate = reinterpret_cast<AllocatePhysicalFn>(rt::shims::find("xmem.dll", "XMemAllocatePhysicalPages"));
    x.free_pages = reinterpret_cast<FreePhysicalFn>(rt::shims::find("xmem.dll", "XMemFreePhysicalPages"));
    x.map = reinterpret_cast<MapPhysicalFn>(rt::shims::find("xmem.dll", "XMemMapPhysicalPages"));
    x.scatter = reinterpret_cast<MapScatterFn>(rt::shims::find("xmem.dll", "XMemMapPhysicalPagesScatter"));
    x.virtual_free = reinterpret_cast<VirtualFreeFn>(
        rt::shims::find("api-ms-win-core-memory-l1-1-0.dll", "VirtualFree"));
    check(x.virtual_alloc && x.allocate && x.free_pages && x.map && x.scatter && x.virtual_free,
          "the registry serves the four physical-page exports, XMemVirtualAlloc and the title's VirtualFree");
    check(rt::shims::find("kernel32.dll", "VirtualFree") == reinterpret_cast<void*>(x.virtual_free),
          "kernel32.dll!VirtualFree is the same shim");
    if (x.virtual_alloc && x.allocate && x.free_pages && x.map && x.scatter && x.virtual_free) {
        test_allocate(x);
        test_map(x);
        test_large_reservation(x);
        test_bulk(x);
        test_input_boundaries(x);
        test_reserve_and_split_failures(x);
        test_batch_rollback(x);
        test_free_failure(x);
        test_release_failure(x, HostOperation::Unmap, "failed unmapping prevents whole-region release");
        test_release_failure(x, HostOperation::Coalesce, "failed coalescing preserves the physical reservation");
        test_release_failure(x, HostOperation::Release, "failed final release preserves the physical reservation");
        test_rollback_failure(x);
        test_noaccess_protection_failure(x);
        test_descriptor_placeholder_chain(x);
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
