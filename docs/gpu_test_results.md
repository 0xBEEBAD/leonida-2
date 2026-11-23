# GPU test results

This page keeps a local record of GPU test runs. The project's public tracker is
[#166](https://github.com/ItsNotRexed/Leonida/issues/166) (one comment per GPU and driver);
post counts and pass/fail lines there, never game shaders, logs or private paths.

| GPU | Driver, OS | Commit | build.py | check.py | run_gpu_tests.py | Failed | Entry |
|---|---|---|---|---|---|---|---|
| AMD Radeon RX 9070 GRE (RDNA4, GFX1201), subgroup 32..64 | RADV, Mesa 26.3.1-arch3.5, Vulkan 1.4.363, CachyOS | `05961f0` | 264/264 translated and valid | 203/206 pass, 3 skipped | 218/235 passed, 16 skipped (native and emulated) | `ms_wave_info_s3`, flaky (see below) | [local note](../gpu-test-results/RX9070GRE.txt) |
| AMD Radeon RX 9070 XT (RDNA4, GFX1201), subgroup 32..64 | AMD 26.11.2 (LLPC), Vulkan 1.4.359, Windows 11 26300.9550 | `5d74c27` | not run | not run | 224/241 passed, 16 skipped, 1 failed (native); 225/241 passed, 0 failed (`LEONIDA_W64_EMULATE=1`) | `ms_wave_info_s3`, every native run, flaky emulated (see below) | [local note](../gpu-test-results/RX9070XT.txt) |
| AMD Radeon RX 7900 GRE (RDNA3, gfx1100), subgroup 32..64 | AMD proprietary 26.11.2 (LLPC), Vulkan 1.4.359, Windows 11 x64 | `5d74c27` | 247/247 translated and valid | run via run_gpu_tests.py | 225/241 passed, 16 skipped | none | [#166](https://github.com/ItsNotRexed/Leonida/issues/166#issuecomment-7115041436) |
| Intel Iris Xe Graphics (Xe-LP / Gen12.2, Alder Lake-P, PCI `8086:46a6`), subgroup 8..32 | Intel proprietary 32.0.101.7088, Vulkan 1.4.323, Windows 11 26H2 (26200) | `5d74c27` / `a60e5ea` | 270/270 translated and valid | 160/211 native, 191/211 emulated (`5d74c27`) | 171/241 native, 201/241 emulated (`5d74c27`); 174/244 native, 204/244 emulated (`a60e5ea`); 213/241 all fallbacks (`5d74c27`) | none natively; `ms_wave_info_s3` under `MESH_COMPUTE=1` (fixed by #9826) | this page |
| NVIDIA GeForce RTX 3070 Ti Laptop GPU (Ampere GA104, PCI `10de:24e0`), subgroup 32..32 | NVIDIA 616.64 (32.0.16.1664), Vulkan 1.4.351, Windows 11 26H2 (26200) | `5d74c27` / `a60e5ea` | 270/270 translated and valid | 176/211 native, 207/211 emulated (`5d74c27`) | 192/241 native, 223/241 emulated (`5d74c27`); 226/244 emulated (`a60e5ea`) | `ms_wave_info_s3` (known SHADER-766, fixed by #9826) | this page |

## AMD Radeon RX 9070 GRE — 2026-11-23

```
GPU:        AMD Radeon RX 9070 GRE (RDNA4 / GFX1201, Navi 48, PCI 1002:7550), subgroup 32..64
Driver:     RADV (Mesa 26.3.1-arch3.5), Vulkan 1.4.363
Subgroup:   32..64 (minSubgroupSize 32, maxSubgroupSize 64; default 64)
OS:         CachyOS, kernel 7.3.5-2-cachyos
Commit:     05961f0eeb24486ce25ddc7dfb453ca87db2be5f (main, 2026-11-23)
Tools:      llvm-mc 23.1.5 (native), SPIRV-Tools v2026.4, CMake 4.4.5 + Ninja 1.13.2, GCC 16.2.1, Python 3.14.9
build.py:   264 shaders: 264 translated, 264 pass spirv-val (6 mesh fixtures not built: ms_clip, ms_f16_pack, ms_tri, ms_v5_workgroup, ms_w64x_ballot, ms_wave_info_s3)
check.py:   203 of 206 pass, 3 skipped (the three mesh fixtures built only by run_gpu_tests.py)
run_gpu_tests.py: 218/235 passed, 16 skipped, 1 failed (native and LEONIDA_W64_EMULATE=1 alike)
Failed:     ms_wave_info_s3: at (0,0) got (-1,-1,-1,-1), want (0.0078,0.0078,0.5,1.0). Flaky on this RADV GPU: 1 pass and 2 fails in three isolated --filter reruns; same signature as the known round-44 mesh regression (#6172, fixed by #5741/#5855). Not a new failure.
Skipped:    16 skips are the documented "run by d3d12_*_test / w64_graphics.py" cases.
```

Setup: `cmake -S runtime -B runtime/build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DLEONIDA_VULKAN_HEADERS_FETCH=ON`;
implicit Vulkan layers disabled (`VK_LOADER_LAYERS_DISABLE='~implicit~'`) because MangoHud/Steam layers are installed.
Inside `run_gpu_tests.py`, 241/241 shaders translated and passed `spirv-val` in both the native and emulated runs.
Every pixel test other than the flaky mesh fixture was exact (4096/4096).

AI-assisted (Hermes Agent): built `main` at `05961f0`, ran `build.py`, `check.py` and
`run_gpu_tests.py` (native and emulated) on this GPU, and verified the isolated reruns of the one failure.

## AMD Radeon RX 9070 XT — 2026-11-23

```
GPU:        AMD Radeon RX 9070 XT (RDNA4 / GFX1201, Navi 48, PCI 1002:7550), subgroup 32..64
Driver:     AMD proprietary 26.11.2 (LLPC), Vulkan 1.4.359
Subgroup:   32..64 (minSubgroupSize 32, maxSubgroupSize 64; default 64, subgroupSizeControl)
OS:         Windows 11, 10.0.26300.9550
Commit:     5d74c272363f2703265d44e6aaab1a1df8ddfaf3 (main, 2026-11-23)
Tools:      llvm-mc from ROCm/AOMP 18.0-12 (LLVM 19.0.0git), SPIRV-Tools v2026.4,
            CMake 4.4.5, Visual Studio 17 2022 generator, MSVC 19.44.35230.0, Python 3.11.9
run_gpu_tests.py: 247 shaders: 247 translated, 247 pass spirv-val
            native:                 224/241 passed, 16 skipped, 1 failed
            LEONIDA_W64_EMULATE=1:  225/241 passed, 16 skipped, 0 failed
Failed:     ms_wave_info_s3: at (0,0) got (-1,-1,-1,-1), want (0.0078,0.0078,0.5,1.0).
            Native: 3 of 3 isolated --filter reruns failed. Emulated: the full-suite
            run passed it, then 1 of 3 isolated reruns passed, so the emulated path
            is flaky here while native fails every time. Same signature as #6172 /
            #5273 and as the RTX 4060 record (#8752), where it failed 3 of 3.
Skipped:    16 skips are the documented "run by d3d12_*_test / w64_graphics.py" cases.
```

Setup: `cmake -S runtime -B runtime/build` (default Visual Studio 17 2022 generator) and
`cmake --build runtime/build --config RelWithDebInfo`. The suite ran on the AMD LLPC driver with
the project's own `vk_harness`, `vk_compute_test` and `vk_store_test` binaries; no implicit layer
beyond the Vulkan SDK's. Every other pixel and compute case was exact.

The one failure is worth a note for #6172: on this machine the native 64-wide path fails
`ms_wave_info_s3` on every run, while `LEONIDA_W64_EMULATE=1` passes it most of the time, which is
the shape a lane-set-dependent race takes rather than a driver-specific miscompile. It also
reproduces with an AMD proprietary driver, so the symptom is not tied to RADV or to Mesa's shader
cache.

`build.py` and `check.py` (the D3D12-side rows in the table) were not run here; the counts above
come from `run_gpu_tests.py`, which builds and translates the same fixtures.

AI-assisted (Cline): built `main` at `5d74c27` with MSVC, ran `run_gpu_tests.py` native and under
`LEONIDA_W64_EMULATE=1`, and reran the failure in isolation three times each way.

## AMD Radeon RX 7900 GRE — 2026-11-23

```
GPU:        AMD Radeon RX 7900 GRE (RDNA3 / gfx1100, PCI 1002:744C), subgroup 32..64
Driver:     AMD proprietary 26.11.2 (LLPC), Vulkan 1.4.359
Subgroup:   32..64 (minSubgroupSize 32, maxSubgroupSize 64; default 64)
OS:         Windows 11 x64
Commit:     5d74c2723 (main, 2026-11-23)
Tools:      llvm-mc 23.1.5 (MSYS2 UCRT64), Vulkan SDK 1.4.363 / SPIRV-Tools v2026.4, MSVC 14.51 (VS 2026 Community), Python 3.12.1
build.py:   247 shaders: 247 translated, 247 pass spirv-val
            wave64 VS/PS/GS on 32-wide subgroups: tier A: 1, tier C: 1
check.py:   not run separately (run_gpu_tests.py below)
run_gpu_tests.py: 225/241 passed, 16 skipped
Failed:     none
Skipped:    16 skips are the documented "run by d3d12_*_test / w64_graphics.py" cases.
```

Setup: `scripts\build_runtime.ps1` from a plain PowerShell. Without Ninja on `PATH` it took the cl.exe fallback, which
builds every Vulkan program the GPU tests use. `llvm-mc` came from the MSYS2 package `mingw-w64-ucrt-x86_64-llvm`
(23.1.5, AMDGPU target included) with `C:\msys64\ucrt64\bin` on `PATH`; on an old MSYS2 install, run `pacman -Syu`
until it reports nothing to do before installing it. Loader warnings from installed Medal/OBS Vulkan layers did not
affect the run. This is the first RDNA3 entry: wave64 and mesh shader tests ran natively (all `ms_*` pass).

AI-assisted (Claude): helped set up the tools and write this entry; the build and test run were done on this GPU.

## Intel Iris Xe Graphics (Alder Lake-P) — 2026-11-24

```
GPU:        Intel(R) Iris(R) Xe Graphics (Xe-LP / Gen12.2, Core i7-12700H, PCI 8086:46a6)
Driver:     Intel proprietary Windows 32.0.101.7088, Vulkan 1.4.323
Subgroup:   8..32 (default 32, maxPushConstantsSize 256, per_vertex_inputs: false)
OS:         Windows 11 Home 26H2 (build 26200)
Commits:    5d74c2723, a60e5eab5, 2d53a8055 (main, 2026-11-24)
Tools:      MSYS2 clang64 LLVM 23.1.5 (llvm-mc with AMDGPU target), SPIRV-Tools v2026.4,
            Clang 23.1.5, CMake 4.5.1, Ninja 1.13.2, CPython 3.11.9
build.py:   270 shaders: 270 translated, 270 pass spirv-val (native and LEONIDA_W64_EMULATE=1, 5d74c2723)
check.py:   160 of 211 pass, 51 skipped natively; 191 of 211 pass, 20 skipped with LEONIDA_W64_EMULATE=1 (5d74c2723)
run_gpu_tests.py:
            5d74c2723 native:        171/241 passed, 70 skipped, 0 failed
            5d74c2723 W64_EMULATE=1: 201/241 passed, 40 skipped, 0 failed
            5d74c2723 all fallbacks (W64_EMULATE + BARY_FALLBACK + TEXOFF_FALLBACK + MESH_COMPUTE):
                                     213/241 passed, 27 skipped, 1 failed (ms_wave_info_s3)
            a60e5eab5 native:        174/244 passed, 70 skipped, 0 failed
            a60e5eab5 W64_EMULATE=1: 204/244 passed, 40 skipped, 0 failed
FMA:        cs_fma_fused — glsl: 3/7 f32 fused / 4/7 split (f16 exact); khr: SKIP (no VK_KHR_shader_fma);
            exact: 8/8 fused exact
PC_TAIL_UBO: LEONIDA_PC_TAIL_UBO=1 passed on cs_user_sgprs, cs_heap_init, ps_tex (6/12, 6 feature skips),
            ps_ops* (8/8), vs_clip* (3/3)
ctest:      72/72 portable C++ tests passed on 2d53a8055 (70 passed, 2 skipped, 0 failed, excluding CPU-only xdxr_build_check)
Failed:     none natively or under W64_EMULATE=1. Under LEONIDA_MESH_COMPUTE=1, ms_wave_info_s3 failed with
            (-1,-1,-1,-1) (known SHADER-766 %ms_counts single-writer guard; passes 6/6 mesh fixtures with #9826).
Skipped:    native skips on 5d74c2723 (run_gpu_tests.py): 30 wave64 compute, 6 mesh (no VK_EXT_mesh_shader),
            4 shaderFloat64, 4 BaryCoordKHR, 3 VK_KHR_maintenance8 texoff, 3 shaderResourceMinLod,
            3 narrow/non-64 fragment subgroup, 1 computed I/J without PerVertexKHR (passes with #9855),
            and 16 harness-external d3d12_* / w64_graphics cases.
```

Setup: built portable Vulkan test binaries with `-DLEONIDA_BUILD_WINDOWS_RUNTIME=OFF` using MSYS2 `clang64`
(`clang++ 23.1.5`, `cmake 4.5.1`, `ninja 1.13.2`, `llvm-mc 23.1.5`, `spirv-tools v2026.4`, `vulkan-headers`) and
selected the iGPU with `LEONIDA_VK_DEVICE=Intel`.

## NVIDIA GeForce RTX 3070 Ti Laptop GPU — 2026-11-24

```
GPU:        NVIDIA GeForce RTX 3070 Ti Laptop GPU (Ampere GA104, PCI 10de:24e0)
Driver:     NVIDIA proprietary Windows 616.64 (32.0.16.1664), Vulkan 1.4.351
Subgroup:   32..32 (maxPushConstantsSize 256, per_vertex_inputs: true, VK_EXT_mesh_shader, VK_KHR_shader_fma)
OS:         Windows 11 Home 26H2 (build 26200)
Commits:    5d74c2723, a60e5eab5, 2d53a8055 (main, 2026-11-24)
Tools:      MSYS2 clang64 LLVM 23.1.5, SPIRV-Tools v2026.4, Clang 23.1.5, CMake 4.5.1, Ninja 1.13.2, CPython 3.11.9
build.py:   270 shaders: 270 translated, 270 pass spirv-val (native and LEONIDA_W64_EMULATE=1, 5d74c2723)
check.py:   176 of 211 pass, 35 skipped natively; 207 of 211 pass, 4 skipped with LEONIDA_W64_EMULATE=1 (5d74c2723)
run_gpu_tests.py:
            5d74c2723 native:        192/241 passed, 48 skipped, 1 failed (ms_wave_info_s3)
            5d74c2723 W64_EMULATE=1: 223/241 passed, 17 skipped, 1 failed (ms_wave_info_s3)
            a60e5eab5 W64_EMULATE=1: 226/244 passed, 17 skipped, 1 failed (ms_wave_info_s3)
FMA:        cs_fma_fused — glsl: 8/8 fused exact; khr: 8/8 fused exact; exact: 8/8 fused exact
ctest:      73/73 portable C++ tests passed on 2d53a8055 (71 passed, 2 skipped, 0 failed, including xdxr_build_check)
Failed:     ms_wave_info_s3: at (0,0) got (-1.0000,-1.0000,-1.0000,-1.0000), want (0.0078,0.0078,0.5,1.0)
            (known SHADER-766 %ms_counts single-writer guard; passes 6/6 mesh fixtures with #9826).
Skipped:    W64_EMULATE=1 skips are the 16 harness-external d3d12_* / w64_graphics cases plus ps_w64x_readlane
            (subgroup 32..32 cannot require 64-wide fragment subgroup).
```

AI-assisted: built the portable Vulkan test tools locally on Windows 11 with MSYS2 `clang64` and ran all synthetic shader and C++ `ctest` suites on both GPUs (no game files).
