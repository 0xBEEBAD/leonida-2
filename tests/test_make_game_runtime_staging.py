"""make_game.py's runtime stage stages its files before replacing the old install (RT-249).

A copy that fails part way (disk full, a file locked by a running gta6rt.exe) used to leave the output folder half
replaced: a new gta6rt.exe next to an old gta6rt_tls_host.dll, or a new runtime with the old game_path.txt. Now every
file is written to a staging folder inside --out first and only then moved into place. Synthetic folders only; no
game files.

    python -I tests/test_make_game_runtime_staging.py
"""
import os
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
import make_game


def read(path):
    with open(path, "rb") as f:
        return f.read()


class RuntimeStagingTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.build = os.path.join(self.tmp, "build")
        self.game = os.path.join(self.tmp, "game")
        self.out = os.path.join(self.tmp, "out")
        for d in (self.build, self.game, self.out):
            os.makedirs(d)
        self.artifacts = []
        for name in make_game.RUNTIME_FILES:
            path = os.path.join(self.build, name)
            with open(path, "wb") as f:
                f.write(b"new " + name.encode())
            self.artifacts.append(path)
        # the previous install
        self.old = {name: b"old " + name.encode() for name in (*make_game.RUNTIME_FILES, "game_path.txt", "gta6rt.sh")}
        for name, data in self.old.items():
            with open(os.path.join(self.out, name), "wb") as f:
                f.write(data)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def entries(self):
        return sorted(os.listdir(self.out))

    def test_success_replaces_everything_and_leaves_no_staging(self):
        make_game.install_runtime(self.game, self.out, self.artifacts)
        for name in make_game.RUNTIME_FILES:
            self.assertEqual(read(os.path.join(self.out, name)), b"new " + name.encode())
        self.assertEqual(read(os.path.join(self.out, "game_path.txt")).decode(), os.path.abspath(self.game))
        self.assertEqual(self.entries(), sorted(self.old))
        if os.name != "nt":
            self.assertEqual(read(os.path.join(self.out, "gta6rt.sh")).decode(), make_game.WINE_LAUNCHER)
            self.assertTrue(os.access(os.path.join(self.out, "gta6rt.sh"), os.X_OK))

    def test_failed_copy_leaves_the_old_install(self):
        real = shutil.copy2
        calls = []

        def copy2(src, dst, *a, **k):
            calls.append(src)
            if len(calls) == 2:  # the second runtime file: the first is already copied (into the staging folder)
                raise OSError(28, "No space left on device")
            return real(src, dst, *a, **k)

        with patch.object(make_game.shutil, "copy2", copy2):
            with self.assertRaises(OSError):
                make_game.install_runtime(self.game, self.out, self.artifacts)
        self.assertEqual(len(calls), 2)
        for name, data in self.old.items():
            self.assertEqual(read(os.path.join(self.out, name)), data, name)
        self.assertEqual(self.entries(), sorted(self.old))  # no staging folder left behind

    def test_failed_game_path_write_leaves_the_old_install(self):
        real = open

        def failing_open(path, mode="r", *a, **k):
            if os.path.basename(str(path)) == "game_path.txt" and "w" in mode:
                raise OSError(5, "Input/output error")
            return real(path, mode, *a, **k)

        with patch("builtins.open", failing_open):
            with self.assertRaises(OSError):
                make_game.install_runtime(self.game, self.out, self.artifacts)
        for name, data in self.old.items():
            self.assertEqual(read(os.path.join(self.out, name)), data, name)
        self.assertEqual(self.entries(), sorted(self.old))

    def test_runtime_build_in_the_output_folder_is_left_in_place(self):
        # --runtime-build pointing at --out: the runtime files are already there and are not copied onto themselves
        artifacts = [os.path.join(self.out, name) for name in make_game.RUNTIME_FILES]
        with patch.object(make_game.shutil, "copy2", side_effect=AssertionError("copied onto itself")):
            make_game.install_runtime(self.game, self.out, artifacts)
        for name in make_game.RUNTIME_FILES:
            self.assertEqual(read(os.path.join(self.out, name)), self.old[name])
        self.assertEqual(read(os.path.join(self.out, "game_path.txt")).decode(), os.path.abspath(self.game))
        self.assertEqual(self.entries(), sorted(self.old))

    def test_missing_output_folder_is_created(self):
        out = os.path.join(self.tmp, "fresh", "out")
        make_game.install_runtime(self.game, out, self.artifacts)
        names = sorted(os.listdir(out))
        want = sorted([*make_game.RUNTIME_FILES, "game_path.txt"] + (["gta6rt.sh"] if os.name != "nt" else []))
        self.assertEqual(names, want)


if __name__ == "__main__":
    unittest.main()
