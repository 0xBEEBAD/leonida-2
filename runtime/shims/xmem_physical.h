// The seam between XMemVirtualAlloc and the CPU physical-page store.
// Some test targets link xmem.cpp without the store; the store installs this hook at startup.
#pragma once
#include <windows.h>

namespace rt::xmem {

// Reserves a placeholder window. `protect` is the CPU protection applied to mapped pages;
// allocation_flags is zero or MEM_TOP_DOWN. Returns null with last error set on failure.
using PhysicalReserveFn = void* (*)(SIZE_T size, DWORD protect, DWORD allocation_flags);
void set_physical_reserve(PhysicalReserveFn fn);
PhysicalReserveFn physical_reserve();

// The guest's MEM_REPLACE_PLACEHOLDER reserve-only form changes only the
// protection of the already-reserved 64 KiB pages. No CPU mapping is created
// until XMemMapPhysicalPages supplies a physical frame.
using PlaceholderReplaceFn = void* (*)(void* address, SIZE_T size, DWORD protect);
void set_placeholder_replace(PlaceholderReplaceFn fn);
PlaceholderReplaceFn placeholder_replace();

namespace testing {

// A narrow failure seam: a hook returns zero to call the real host API, or a Win32 error to fail
// before the host operation changes anything. It must not reenter the page store. Tests install it
// only while their own calls are active and restore nullptr afterwards.
enum class HostOperation { Reserve, Split, Map, Protect, Unmap, Coalesce, Release, Query };
using FailureHook = DWORD (*)(HostOperation operation, void* address);
void set_failure_hook(FailureHook hook);

}  // namespace testing
}  // namespace rt::xmem
