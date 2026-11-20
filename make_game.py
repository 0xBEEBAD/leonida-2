"""Build the PC port from your own copy of the game.

    python -I make_game.py --game "D:\\path\\to\\your\\package" --out "D:\\Games\\Leonida"
    python -I make_game.py --game "..." --check                 # only check the copy, build nothing
    python -I make_game.py --game "..." --out "..." --stages shaders,assets

The game folder is your own Xbox Series package, version 1.0.2.1, with its files already readable on disk
(see game/xbox-1.0.2.1.json for the file list). This script never decrypts, downloads or works around anything:
it reads plain files and writes everything it makes under --out. Never share what it makes.

Stages, in order (each skips work that is already done, so a stopped build continues where it left off):
    check     file names and sizes against game/<version>.json (add --full-check for SHA-256, slow)
    shaders   carve shaders.rpf, unpack the native RDNA2 code, disassemble, translate to SPIR-V
    assets    models (OBJ), texture dictionaries and texture pools (DDS), audio banks (AWC)
    runtime   build the loader (needs the game's executable to run, not to build)
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(ROOT, "tools")
VERSIONS = os.path.join(ROOT, "game")
STAGES = ["check", "shaders", "assets", "runtime"]
# Archives scanned for models and texture dictionaries, small to large.
MODEL_ARCHIVES = [
    "common.rpf", "textures.rpf", "data.rpf",
    "pack/story000_restofworld/story000_restofworld.rpf", "pack/shared_minigame000/shared_minigame000.rpf",
    "pack/core_restofworld/core_restofworld.rpf", "pack/core_restofworld001/core_restofworld001.rpf",
    "packs.rpf", "pack/story000/story000.rpf", "levels.rpf",
]


def log(msg: str) -> None:
    print(f"{time.strftime('%H:%M:%S')}  {msg}", flush=True)


def fail(msg: str) -> "None":
    print(f"\nERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def below_normal() -> None:
    """Keep the PC usable while a long build runs."""
    if os.name == "nt":
        import ctypes
        k32 = ctypes.windll.kernel32
        k32.SetPriorityClass(k32.GetCurrentProcess(), 0x00004000)


def run(args: list, **kw) -> None:
    log("  " + " ".join(os.path.basename(str(a)) if i == 0 else str(a) for i, a in enumerate(args)))
    r = subprocess.run([str(a) for a in args], **kw)
    if r.returncode != 0:
        fail(f"{os.path.basename(str(args[0]))} exited with {r.returncode}")


def py(tool: str, *args) -> None:
    run([sys.executable, "-I", os.path.join(TOOLS, tool), *args])


def need(exe: str, hint: str) -> str:
    path = shutil.which(exe)
    if not path:
        fail(f"'{exe}' was not found on PATH. {hint}")
    return path


# ---------------------------------------------------------------- check
def load_version(game: str) -> dict:
    for name in sorted(os.listdir(VERSIONS)):
        if not name.endswith(".json"):
            continue
        with open(os.path.join(VERSIONS, name), encoding="utf-8") as f:
            v = json.load(f)
        probe = "MicrosoftGame.Config"
        p = os.path.join(game, probe)
        if probe in v["files"] and os.path.isfile(p) and os.path.getsize(p) == v["files"][probe]["size"]:
            return v
    fail("This doesn't look like a supported version of the game. Supported: "
         + ", ".join(n[:-5] for n in os.listdir(VERSIONS) if n.endswith(".json")))


def check(game: str, full: bool) -> dict:
    if not os.path.isdir(game):
        fail(f"Game folder not found: {game}")
    v = load_version(game)
    log(f"check: {v['title']} {v['version']}, {len(v['files'])} files")
    bad = 0
    for rel, info in v["files"].items():
        p = os.path.join(game, rel)
        if not os.path.isfile(p):
            print(f"  missing  {rel}"); bad += 1
        elif os.path.getsize(p) != info["size"]:
            print(f"  wrong size  {rel}"); bad += 1
    if bad:
        fail(f"{bad} file(s) missing or different. Use a complete, unmodified copy of version {v['version']}.")
    if full:
        py("manifest.py", "verify", game, "--manifest", os.path.join(VERSIONS, f"xbox-{v['version']}.json"))
    exe = os.path.join(game, v["expected_exe"])
    log(f"check: all files present{' and hashes match' if full else ''}. "
        + ("Executable found." if os.path.isfile(exe) else
           f"{v['expected_exe']} is not in the folder: the runtime stage can be built but not run."))
    return v


# ---------------------------------------------------------------- shaders
def shaders(game: str, work: str, jobs: int) -> None:
    src = os.path.join(game, "shaders.rpf")
    sh = os.path.join(work, "shaders")
    dxbc = os.path.join(sh, "dxbc")
    if not os.path.isfile(os.path.join(dxbc, "index.json")):
        log("shaders: carving shaders.rpf")
        py("shader_carve.py", src, dxbc)
    if not os.path.isdir(os.path.join(dxbc, "fssm")):
        log("shaders: unpacking native code")
        py("fssm_unpack.py", dxbc)
    disasm = os.path.join(sh, "disasm")
    if not os.path.isdir(disasm) or len(os.listdir(disasm)) < 1000:
        need(os.environ.get("LLVM_MC", "llvm-mc"),
             "Install LLVM 21 or newer with the AMDGPU target, or set LLVM_MC to llvm-mc's full path.")
        log("shaders: disassembling (gfx1013)")
        py("shader_disasm.py", os.path.join(dxbc, "fssm"), "--out", disasm, "--mcpu", "gfx1013", "--jobs", jobs)
    spirv = os.path.join(sh, "spirv")
    if not os.path.isdir(spirv) or len(os.listdir(spirv)) < 1000:
        need("spirv-as", "Install the Vulkan SDK (it includes spirv-as and spirv-val) and reopen the terminal.")
        need("spirv-val", "Install the Vulkan SDK and reopen the terminal.")
        log("shaders: translating RDNA2 to SPIR-V (the longest step)")
        py("rdna2spirv.py", disasm, "--out", spirv)
    log("shaders: done")


# ---------------------------------------------------------------- assets
def rpf9dump(work: str) -> str:
    exe = os.path.join(work, "tools", "rpf9dump", "rpf9dump.exe" if os.name == "nt" else "rpf9dump")
    if not os.path.isfile(exe):
        core = os.path.join(ROOT, "third_party", "rpf9-explorer", "Core")
        if not os.path.isdir(core):
            fail("third_party/rpf9-explorer is empty. Run: git submodule update --init")
        need("dotnet", "Install the .NET 8 SDK.")
        log("assets: building rpf9dump")
        run(["dotnet", "build", os.path.join(TOOLS, "rpf9dump", "rpf9dump.csproj"), "-c", "Release",
             "-o", os.path.dirname(exe), "--nologo", "-v", "q"])
    return exe


def assets(game: str, work: str) -> None:
    exe = rpf9dump(work)
    out = os.path.join(work, "assets")
    for rel in MODEL_ARCHIVES:
        src = os.path.join(game, rel)
        name = os.path.splitext(os.path.basename(rel))[0]
        for mode in ("models", "textures"):
            if os.path.isfile(os.path.join(out, name, mode, "manifest.json")):
                continue
            log(f"assets: {mode} from {rel}")
            run([exe, mode, src, out])
    pools = []
    for root, _, files in os.walk(os.path.join(game, "data", "ttdb")):
        pools += [os.path.join(root, f) for f in files if f.lower().endswith(".ttpf")]
    for p in sorted(pools, key=os.path.getsize):
        name = os.path.splitext(os.path.basename(p))[0]
        if os.path.isfile(os.path.join(out, "ttpf", name, "manifest.json")):
            continue
        log(f"assets: texture pool {os.path.relpath(p, game)}")
        run([exe, "ttpf", p, os.path.join(out, "ttpf")])
    if not os.path.isfile(os.path.join(out, "audio", "audio_manifest.json")):
        log("assets: audio banks")
        run([exe, "audio", os.path.join(game, "audio.rpf"), out])
    log("assets: done")


# ---------------------------------------------------------------- runtime
def runtime(game: str, out: str, v: dict) -> None:
    if os.name != "nt":
        fail("The runtime builds on Windows only for now.")
    log("runtime: building")
    run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
         os.path.join(ROOT, "scripts", "build_runtime.ps1")])
    built = os.path.join(ROOT, "runtime", "build", "gta6rt.exe")
    os.makedirs(out, exist_ok=True)
    shutil.copy2(built, os.path.join(out, "gta6rt.exe"))
    with open(os.path.join(out, "game_path.txt"), "w", encoding="utf-8") as f:
        f.write(os.path.abspath(game))
    if not os.path.isfile(os.path.join(game, v["expected_exe"])):
        log(f"runtime: built, but {v['expected_exe']} is missing from your copy, so there is nothing to run yet.")
    else:
        log(f"runtime: done. Start {os.path.join(out, 'gta6rt.exe')}")


def main() -> int:
    ap = argparse.ArgumentParser(description="Build the PC port from your own copy of the game.")
    ap.add_argument("--game", required=True, help="your package folder")
    ap.add_argument("--out", help="folder for the finished port (and its work files unless --work is given)")
    ap.add_argument("--work", help="folder for intermediate files (hundreds of GB; default <out>/work)")
    ap.add_argument("--stages", default=",".join(STAGES), help="comma list from: " + ",".join(STAGES))
    ap.add_argument("--check", action="store_true", help="only check the game files")
    ap.add_argument("--full-check", action="store_true", help="also compare SHA-256 of every file (slow)")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    a = ap.parse_args()
    below_normal()
    game = os.path.abspath(a.game)
    v = check(game, a.full_check)
    if a.check:
        return 0
    if not a.out:
        fail("--out is required to build.")
    out = os.path.abspath(a.out)
    work = os.path.abspath(a.work or os.path.join(out, "work"))
    if os.path.commonpath([out, game]) == game or os.path.commonpath([work, game]) == game:
        fail("--out and --work must be outside the game folder.")
    stages = [s.strip() for s in a.stages.split(",") if s.strip()]
    for s in stages:
        if s not in STAGES:
            fail(f"unknown stage '{s}'")
    os.makedirs(work, exist_ok=True)
    t0 = time.time()
    if "shaders" in stages:
        shaders(game, work, a.jobs)
    if "assets" in stages:
        assets(game, work)
    if "runtime" in stages:
        runtime(game, out, v)
    log(f"finished in {(time.time() - t0) / 60:.0f} min")
    return 0


if __name__ == "__main__":
    sys.exit(main())
