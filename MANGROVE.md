# mangrove

TreXito's own copy of [Leonida](https://github.com/ItsNotRexed/Leonida) (GPL-3.0). Only TreXito works here.

## What `main` is

Upstream `main` at `5d74c2723`, plus, in merge order:

1. The `ladder-frame` boot stack (ASTROWwwW): the #7154 boot-path PRs merged together, build fixes for clang-cl.
2. Boot-path PRs on top: #7290 (fence slot 12), #8591, #8408, #8869 (slot 81 GetFrameEventsX), newer heads of
   #4313 / #6446 / #7201 / #7403 / #8273, #7584 (xmem: one view per run of pages), #7218 (slot 133 buffer on the
   title's pages), #8304's two commits (slot 70 finds slot 133 buffers).
3. Cherry-picks: slot 25 layouts, UAV flag stripping, slot 61, slot 55 derived PSOs, #7052's mock checks.
4. Slot 67 translator work: #8448 (null/exec/vcc_hi compare destinations), #8526 (bit replication into EXEC),
   #3976 (ds_append / ds_consume) with the always-barrier fix for `--wave64-emulate`.
5. Own fixes: xmem split guard only for split views; a struct-return argument on the caller's stack is the return
   buffer, not the device (slots 25 / 26 / 56 / 108); the shared CommittedResource3 mock.
6. All of TreXito's open upstream PRs: #4025, #4717, #4795, #4908, #5012, #5029, #5168, #5231, #5310, #5479,
   #5561, #6207, #6443, #7010, #7864, #8408 (and #7290 from step 2).

Left out: #2231 (conflicts with #8448 in the lane-mask analysis, adds 46 library programs), #7235.

## Branches

- `main`: the above.
- `pr/<number>-<name>`: each of TreXito's open upstream PRs at its head, to push back upstream from.
- `upstream-main`: the upstream `main` this copy is based on.

## Building it with your own copy of the game (Windows)

The prerequisites are the ones in [README.md](README.md#how-to-build): package version 1.0.2.1, Python 3.12+,
.NET 8 SDK, Vulkan SDK, LLVM with the AMDGPU target (`LLVM_MC`), VS 2022 C++ with Windows SDK 10.0.26100+.

1. `git clone --recurse-submodules truenas_admin@192.168.0.250:repos/mangrove.git`. The submodule
   `third_party/rpf9-explorer` comes from its GitHub URL in `.gitmodules`.
2. `python -I tools/pc_checker.py --cli` checks whether the PC is strong enough.
3. `python -I make_game.py --check --game "D:\path\to\package"` checks the copy and builds nothing.
4. To run the port, about 30 GB:
   `python -I make_game.py --game "D:\path\to\package" --out "D:\Games\Leonida" --work "E:\leonida-work" --stages check,shaders,runtime`.
   Add `assets` (about 370 GB) only for the asset tools.
5. Or double-click `Build.bat`, which finds the package by itself and builds everything (about 400 GB).

What is tested and what is not: the C++ runtime is built with clang-cl and its tests run under Wine on the NAS;
the Python tests run on Linux. An MSVC build (`build_runtime.ps1` with `cl.exe`) and a real `make_game.py` run
with the game files have not been done on this copy yet.

## Keeping up with upstream

GitHub disabled the upstream repository (moved to `ProjectLeonida/Leonida`, git returns 403 "disabled") on
2026-11-23. Its last `main` is `upstream-main` here (`5d74c2723`, 2026-11-23 21:01 UTC, full history).

The open upstream PRs still exist in their authors' forks. A snapshot of every open PR head (4,731 PRs) is in
`truenas_admin@192.168.0.250:repos/leonida-own.git` under `refs/pr/<number>`:

```bash
git remote add claude truenas_admin@192.168.0.250:repos/leonida-own.git
git fetch claude 'refs/pr/7320:refs/remotes/pr/7320'     # one PR
git fetch claude 'refs/pr/*:refs/remotes/pr/*'           # all of them
```

`claude/main` is Claude's working copy (mangrove's base plus fixes); `claude/for-mangrove` is this `main` with those
fixes merged, a fast-forward from here: `git fetch claude && git merge --ff-only claude/for-mangrove`.

## Testing

- Python: `python3 -I tests/<file>.py` (spirv-as / spirv-val on PATH un-skip the translator tests that need them).
- C++ under Wine on the NAS: `git bundle create own.bundle <base>..main`, copy it to `~/leonida/w/own.bundle`,
  put `/w/own.bundle refs/heads/main` in `~/leonida/w/xref.txt`, leave `xtests.txt` empty for a full run, then
  `midclt call cronjob.run 1370 true`. Results: `~/leonida/w/out/xjob.log` (`XJOB_DONE`) and `out/ctest.log`.
  The known Wine baseline is 10 failures: file_system, dstorage, run_startup_cmdline, d3d12_clear,
  d3d12_view_slices, d3d12_depth_resolve, d3d12_residency, d3d12_heap_properties, dxgi_swapchain,
  d3d12_clear_formats.

## Related project

[xcl](https://github.com/ivall/xcl) (Rust, Xbox Series compatibility layer, RDNA2 to DXIL) works on the same
problem. It is GPL-2.0-only, which cannot be combined with GPL-3.0 code: read it for facts (slot meanings, ABI
quirks), write the code here yourself.
