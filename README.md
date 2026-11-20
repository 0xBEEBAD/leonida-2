<h1 align="center">
  <br>
  <b>Leonida</b>
  <br>
</h1>

<h4 align="center">Grand Theft Auto VI (Xbox Series) running natively on Windows, built on your PC from your own copy.
</h4>

<p align="center">
  <a href="#project-status">Status</a> |
  <a href="#how-to-build">How to build</a> |
  <a href="#how-it-works">How it works</a> |
  <a href="#want-to-help">Want to help?</a> |
  <a href="#credits">Credits</a> |
  <a href="#license">License</a>
</p>

Leonida is a static recompilation project. The Xbox Series CPU is x86-64 like a PC, so the game's code is loaded
and run natively, and the console's system software is reimplemented. The console's precompiled RDNA2 GPU programs
are translated to SPIR-V for Vulkan. The game's models, textures and audio are read from your package. All of this
happens **on your PC, from your own copy of the game**.

> [!IMPORTANT]
> This repository **does not contain the game, keys or an executable**, and it does not explain how to get them. You
> need your own copy, with its files already readable on disk. Nothing here decrypts or works around any protection.
> Everything the build creates (the generated shaders, extracted assets, the runtime folder and your saves) comes
> from your game and stays on your PC. Never share those files.

## Project status

**Early. The game does not boot yet.** Everything that can be done without the game's executable is in place, so
work on booting can start as soon as one is available to a developer.

| What | Status |
|---|---|
| Version check of your copy (Xbox Series 1.0.2.1) | ✅ Works |
| Shaders: carve, unpack native RDNA2 code, disassemble | ✅ All 26,503 |
| Shaders: RDNA2 → SPIR-V translation | ⚠️ ~97% of vertex/pixel shaders pass `spirv-val`. Compute, mesh and geometry shaders not yet translated |
| Shader translator checked on a real GPU | ⚠️ Vertex fetch, texture sampling, exports and ALU ops checked on one GPU |
| Models (OBJ with real names, UVs, material references) | ✅ Exported. Not all layouts are decoded yet |
| Texture dictionaries and world texture pools (DDS) | ✅ Exported. Some pool entries (BCPack) are skipped |
| Audio banks (AWC: Opus, XMA2) | ✅ Exported whole. Decoding to WAV is not done yet |
| PE loader and import shims | ⚠️ Skeleton. Untested against the real executable |
| Xbox D3D12 → Vulkan | ❌ Not started |
| Booting, story mode, anything playable | ❌ No |

## How to build

You need:

- **Your own copy** of the Xbox Series package, version 1.0.2.1, as plain files in a folder.
- **Windows 10/11 x64**, about **400 GB** free for intermediate files.
- [Python 3.11+](https://www.python.org/), the [.NET 8 SDK](https://dotnet.microsoft.com/download/dotnet/8.0)
  and the [Vulkan SDK](https://vulkan.lunarg.com/) (for `spirv-as` / `spirv-val`).
- [LLVM 21+](https://github.com/llvm/llvm-project/releases) with `llvm-mc` on PATH (or set `LLVM_MC`).
- Visual Studio 2022 with **Desktop development with C++**, for the runtime stage.

Clone with the submodule:

```powershell
git clone --recursive https://github.com/<you>/Leonida
cd Leonida
```

Check your copy (reads names and sizes only, takes a second):

```powershell
python -I make_game.py --game "D:\path\to\your\package" --check
```

Build:

```powershell
python -I make_game.py --game "D:\path\to\your\package" --out "D:\Games\Leonida"
```

Stages run in order: `check`, `shaders`, `assets`, `runtime`. Pick some with `--stages shaders,assets`. Put
intermediate files on another drive with `--work`. A stopped build continues where it left off. The shader
translation is the longest step: several hours on one core for now.

## How it works

| Piece | What it does | Where |
|---|---|---|
| Version check | Every package file's name, size and SHA-256 for each supported version | `game/`, `make_game.py` |
| Shader pipeline | Carves DXBC containers from `shaders.rpf` without its encrypted table of contents. Unpacks the `XBCH` → `FSSM` native RDNA2 program, disassembles it with LLVM (`gfx1013`) and translates it to SPIR-V | `tools/shader_carve.py`, `fssm_unpack.py`, `shader_disasm.py`, `rdna2spirv.py` |
| Asset export | Finds validated resources inside RPF9 archives and the loose `.ttpf` texture pools | `tools/rpf9dump` (on rpf9-explorer's Core) |
| Runtime | Maps the PE image, resolves imports to shims (GDK, XMem, `d3d12_xs`) | `runtime/` |
| Research tools | PE/RTTI inspection, raw carving, texture tiling probes, GPU test harnesses | `tools/`, `runtime/test/` |

The translator's runtime contract (user SGPRs, push constants, memory model, bindless images, vertex fetch,
system values) and all the format notes are in [docs/design.md](docs/design.md).

## Want to help?

The biggest open pieces:

- **Compute, mesh and geometry shaders**: their per-stage system-value register layout.
- **Xbox D3D12 → Vulkan**: the `d3d12_xs` command list and descriptor heap model.
- **Audio decoding**: Opus/XMA2 streams in AWC banks to PCM.
- **Model layouts** the decoder rejects today.

See [CONTRIBUTING.md](CONTRIBUTING.md). Never attach game files, extracted assets or generated shaders to issues or
pull requests.

## Credits

- [rpf9-explorer](https://github.com/breadbbbb/rpf9-explorer) by breadbbbb: the RPF9 model, texture-pool and AWC
  decoders used by `tools/rpf9dump` (git submodule).
- [Mesa](https://gitlab.freedesktop.org/mesa/mesa) (MIT): GFX10 resource-descriptor layout (`tools/gfx10_formats.json`),
  and the ACO/RADV rules used as reference for 16-bit results and shader system registers.
- [LLVM](https://llvm.org/): AMDGPU disassembler.
- [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools): assembly and validation.

Grand Theft Auto and all related names are trademarks of their owners. This project is not affiliated with or
endorsed by them.

## License

GPLv3 (or any later version). See [LICENSE](LICENSE). Third-party code keeps its own license.
