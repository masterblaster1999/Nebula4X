import tempfile
import unittest
import zipfile
from pathlib import Path


import make_release_zip


class MakeReleaseZipTests(unittest.TestCase):
    def test_excludes_common_artifacts_and_self(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)

            # Included files.
            (root / "README.md").write_text("hello\n", encoding="utf-8")
            (root / "src").mkdir()
            (root / "src" / "main.cpp").write_text("int main(){}\n", encoding="utf-8")

            # Excluded top-level dirs.
            (root / "out" / "core").mkdir(parents=True)
            (root / "out" / "core" / "obj.o").write_bytes(b"\0")
            (root / "build").mkdir()
            (root / "build" / "CMakeCache.txt").write_text("x", encoding="utf-8")

            # Excluded anywhere dirs.
            (root / ".idea").mkdir()
            (root / ".idea" / "workspace.xml").write_text("x", encoding="utf-8")
            (root / "nested" / "__pycache__").mkdir(parents=True)
            (root / "nested" / "__pycache__" / "a.pyc").write_bytes(b"\0")

            # Excluded files.
            (root / "debug.log").write_text("x", encoding="utf-8")
            (root / "archive.zip").write_bytes(b"\0")
            (root / ".DS_Store").write_bytes(b"\0")
            (root / "Thumbs.db").write_bytes(b"\0")

            out_zip = root / "release.zip"
            # Ensure self-exclusion works even if the output already exists.
            out_zip.write_bytes(b"old")

            n = make_release_zip.create_source_zip(root, out_zip)
            self.assertGreaterEqual(n, 2)

            with zipfile.ZipFile(out_zip, "r") as zf:
                names = set(zf.namelist())

            self.assertIn("README.md", names)
            self.assertIn("src/main.cpp", names)

            # Ensure excluded entries are not present.
            for bad in [
                "out/core/obj.o",
                "build/CMakeCache.txt",
                ".idea/workspace.xml",
                "nested/__pycache__/a.pyc",
                "debug.log",
                "archive.zip",
                ".DS_Store",
                "Thumbs.db",
                "release.zip",
            ]:
                self.assertNotIn(bad, names)

    def test_cli_relative_output_is_under_root(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "a.txt").write_text("x", encoding="utf-8")

            # Simulate the CLI behavior for relative outputs.
            out_rel = Path("nebula4x_source.zip")
            out_abs = (root / out_rel).resolve()

            make_release_zip.create_source_zip(root, out_abs)

            self.assertTrue(out_abs.exists())
            with zipfile.ZipFile(out_abs, "r") as zf:
                self.assertIn("a.txt", zf.namelist())


if __name__ == "__main__":
    unittest.main()
