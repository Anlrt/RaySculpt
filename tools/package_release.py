"""Package the Windows desktop editor with its native optics engine."""
import argparse
from hashlib import sha256
from pathlib import Path
import re
from zipfile import ZIP_DEFLATED, ZipFile

ROOT = Path(__file__).resolve().parents[1]


def package(output):
    version = (ROOT / "VERSION").read_text(encoding="ascii").strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        raise ValueError("Invalid release version")
    binary = ROOT / "build/Release/caustic_cli.exe"
    if not binary.is_file():
        raise FileNotFoundError(binary)
    files = [ROOT / name for name in ("Caustic CAD.cmd", "README.md", "CHANGELOG.md",
                                      "THIRD_PARTY_NOTICES.md", "VERSION", "requirements.txt")]
    files.extend(p for p in (ROOT / "tools").iterdir() if p.suffix in (".py", ".json") and p.name != "package_release.py")
    files.extend(p for p in (ROOT / "examples").iterdir() if p.is_file() and p.suffix != ".cpp")
    files.extend(p for p in (ROOT / "docs").iterdir() if p.is_file())
    output.mkdir(parents=True, exist_ok=True)
    archive = output / f"RaySculpt-{version}-Windows-x64.zip"
    with ZipFile(archive, "w", compression=ZIP_DEFLATED) as target:
        for path in sorted([*files, binary]):
            target.write(path, path.relative_to(ROOT).as_posix())
    with ZipFile(archive) as target:
        if target.testzip() is not None:
            raise OSError("Release ZIP failed its integrity check")
    checksum = sha256(archive.read_bytes()).hexdigest()
    archive.with_suffix(".zip.sha256").write_text(f"{checksum}  {archive.name}\n", encoding="ascii")
    print(archive)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=ROOT / "build/releases")
    package(parser.parse_args().output)
