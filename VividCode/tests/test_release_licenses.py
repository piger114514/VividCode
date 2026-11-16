"""Validate the checked-in license bundle; optionally audit a staged runtime."""
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def validate(root):
    licenses = root / "licenses"
    entries = json.loads((licenses / "manifest.json").read_text("utf-8-sig"))["files"]
    expected = {entry["path"] for entry in entries}
    actual = {p.relative_to(licenses).as_posix() for p in licenses.rglob("*")
              if p.is_file() and p != licenses / "manifest.json"}
    assert expected == actual, f"Unlisted/missing license files: {expected ^ actual}"
    for entry in entries:
        assert hashlib.sha256((licenses / entry["path"]).read_bytes()).hexdigest() == entry["sha256"], entry["path"]
    for group in ("npm", "nuget"):
        for package in json.loads((licenses / group / "packages.json").read_text("utf-8")):
            folder = licenses / package["directory"]
            assert folder.is_dir(), package["name"]
            assert any("license" in p.name.lower() for p in folder.iterdir()), package["name"]
    assert (root / "THIRD_PARTY_NOTICES.txt").is_file()
    if (root / "vividcode-viewer.exe").exists():
        assert (root / "LICENSE.txt").is_file()
        assert not (root / "dxcompiler.dll").exists()
        assert not (root / "pi-renderer/node_modules/mathjax-full/es5").exists()
        assert (root / "sqldrivers/qsqlite.dll").is_file()
        assert (root / "platforms/qwindows.dll").is_file()
        assert not list((root / "pi-renderer").rglob("*.map"))
    print(f"PASS: {len(entries)} verified license files in {root}")


if __name__ == "__main__":
    validate(ROOT)
    for argument in sys.argv[1:]:
        validate(Path(argument))
