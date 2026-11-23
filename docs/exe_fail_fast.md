# Where the retail executable stops itself on an import's result (RT-5)

A title treats some results as impossible. It tests the value an import returned and, when it is the failing one,
branches to `int3`: MSVC's `__debugbreak`, and the marker it places after a call that does not return. A shim that
returns that value at such a site ends the boot there, with no message. This page lists those sites for
`game_scarlett_master.exe`, per import: how many there are and which value takes them. It shows which results each shim
must get right before a run reaches them.

Nothing was run. `tools/import_fail_fast.py` reads the file: it finds each import's call sites (through its thunk with
`tools/import_call_sites.py`, and as `call [rip+slot]` for the `api-ms-win-*` imports). It then decodes the instructions
after each call up to the test of the result and follows both sides of the branch for a few instructions. The method
and its limits are in the tool's docstring. No code or data of the executable is quoted here, only names, counts and
conditions. Measured on SHA-256 `e95020b2fa4a721db3df59fdecf75621730ac559d8835252f6c3b658d5a08993`. The bindings below
are those of `main` `829e5ede`.

```powershell
python -I tools/import_fail_fast.py game_scarlett_master.exe
python -I tools/import_fail_fast.py game_scarlett_master.exe --dll xmem.dll --all --json
```

Result: 515 of 587 imports called directly, 27 with a fatal result check, 78 fatal sites. An independent pass with Capstone found the
same sites, apart from two `XMemVirtualAlloc` sites that the tool finds and that pass missed. At one, the result is copied
to another register before the test. At the other, the failing side reaches `int3` through a `jmp`.

## The sites, by how `main` binds the import

"Stops when" is the result that takes the fatal side. It is written with the register at the width the test reads:
`eax < 0` is a failed `HRESULT`, `rax == -1` is `INVALID_HANDLE_VALUE`.

### Shims: results to keep

| Import | Calls | Fatal | Stops when | Note |
|---|---:|---:|---|---|
| `xmem.dll!XMemVirtualAlloc` | 23 | 14 | `rax == 0` | Since #6908 the shim takes the attribute word and drops the `PAGE_GRAPHICS_*` bits. It still refuses the 2 MiB page mode (`allocation_type` `0x20003000`, `ERROR_NOT_SUPPORTED`), which 4 of these 14 sites pass; the other 10 reach `VirtualAlloc`. The 6 calls it refuses for `0x4000` or `0x40000` (`docs/xmem_exe_calls.md`) are not among the 14 |
| `XMemGetWorkingSetStatistics` | 3 | 2 | `eax == 0` | The shim returns `FALSE` only for a null output or a failed `GetProcessMemoryInfo`; both sites pass a stack buffer |
| `XMemTransferMemory` | 1 | 1 | `eax == 0` | The shim returns `TRUE` |
| `CreateFileW` | 13 | 1 | `rax == -1` | One open that has no failure path: the file must exist where `runtime/shims/file_system.cpp` maps it |
| `GetModuleHandleW` | 3 | 1 | `rax == 0` | Two calls in a row, for `api-ms-win-core-synch-l1-2-0.dll` and then `kernel32.dll`; the title stops when both give `NULL` |
| `_configure_narrow_argv` | 1 | 1 | `eax != 0` | The CRT's start-up; the shim returns 0 |
| `d3d12_xs.dll!D3D12ConfigureVirtualMemoryX` | 1 | 1 | `eax < 0` | The shim (#6792) returns `S_OK` |

### No binding at `main`: what the default return does

An import with no shim, host function or bundled module returns 0 under `LEONIDA_UNIMPLEMENTED=log` (the default), and
under `fail` too unless `runtime/shims/registry.cpp` lists it as returning an `HRESULT`, which none of these is.

| Import | Calls | Fatal | Stops when | 0 stops it? |
|---|---:|---:|---|---|
| `xmem.dll!XMemMapPhysicalPages` | 12 | 3 | `rax == 0` | yes |
| `XMemAllocatePhysicalPages` | 4 | 2 | `eax != 1` (1), `eax == 0` (1) | yes, both |
| `XMemMapPhysicalPagesScatter` | 2 | 2 | `eax == 0` | yes |
| `XMemIsToolsMemAvailable` | 1 | 1 | `eax == 0` | yes, where reached: the function holding the site has 6 callers, and each calls it only after one helper returns true (the first after an allocation failed) |
| `AcpHal.dll!ApuCreateHeap` | 1 | 1 | `eax < 0` | no: 0 is `S_OK`, with the output unset |
| `GameInput.dll!#1` | 1 | 1 | `eax < 0` | no. The executable imports `GameInput.dll` by ordinal 1 only, and the shim table names `GameInputCreate`. The site zeroes an out pointer before the call and calls the object's `AddRef` after it, so ordinal 1 is `GameInputCreate(IGameInput**)`, which #3410 binds |

The four xmem rows are the ones a return of 0 stops: 8 sites.

### Host and CRT: not failures a shim causes

| Import | Calls | Fatal | Stops when | What it is |
|---|---:|---:|---|---|
| `MSVCP140.dll!_Mtx_lock` | 21 | 19 | `eax != 0` | `std::mutex::lock`'s error path |
| `VirtualFree` | 23 | 12 | `eax == 0` | Whatever `XMemVirtualAlloc` returns must be memory the host `VirtualFree` releases, or `VirtualFree` needs a shim |
| `IsProcessorFeaturePresent` | 3 | 3 | `eax != 0` | `PF_FASTFAIL_AVAILABLE` (`0x17`) before `int 29h`: the CRT's abort path, after the decision to stop |
| `GetLogicalProcessorInformation` | 2 | 2 | `eax != 0` (1), `eax == 0` (1) | The size query must fail, then the real call must succeed |
| `GetLastError` | 36 | 1 | `eax != 0x7a` | Between those two: the size query's error must be `ERROR_INSUFFICIENT_BUFFER` |
| `strcmp` | 464 | 2 | `eax == 0` | The title's own string checks |
| `_stricmp` | 160 | 1 | `eax != 0` | as above |
| `strrchr` | 32 | 1 | `rax == 0` | as above |
| `MSVCP140.dll!_Getcat` (`codecvt<char>`, `ctype<wchar_t>`) | 2 | 2 | `rax == -1` | Locale facets; `msvcp140.dll` binds to the host copy |
| `ReadFile` | 2 | 1 | `eax == 0` | Host |
| `CreateEventW` | 1 | 1 | `rax == 0` | Host |
| `CreateSemaphoreExW` | 2 | 1 | `rax == 0` | Host |
| `WS2_32.dll!#115` (`WSAStartup`) | 1 | 1 | `eax != 0` | Host; #6943's offline Winsock shim serves ordinal 115 and must return 0 here |

## What this does not show

A site found here is code that can stop the title, not proof that the boot reaches it: nothing ran. Only
straight-line code after each call is read. A result stored and tested later, or a failure path that logs before it
stops, is not seen, so each count is a lower bound. #6915 records the stops one run reached, in order.
