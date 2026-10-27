"""Package byte-exact and decoded X4 ARC material as a REA-readable ZIP.

This creates a supported archive container around the proprietary ARC
extraction. It does not change or reinterpret the original payload bytes.

Usage:
  python tools/diagnostics/package_x4_arc_analysis.py
  python tools/diagnostics/package_x4_arc_analysis.py build/rockman --output build/rockman/x4-arc-analysis.zip --overwrite
"""

import argparse
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile


GROUPS = (
    ("raw", "arc-extracted"),
    ("decoded/player-poses", "sprite-banks"),
    ("decoded/type1", "arc-decoded-all"),
    ("previews", "arc-previews-expanded"),
)

README = """Rockman X4 ARC analysis bundle

raw/ contains byte-exact typed payloads and source offsets/hashes from the ARC extractor.
decoded/player-poses/ contains type-2 word-stream outputs and descriptor provenance.
decoded/type1/ contains parsed type-1 frame/piece records and raw-only payloads
with status reports for two layouts the current parser does not recognize.
previews/ contains grayscale 16x16 tile-major 4bpp previews for ARC types 5, 14, 17, 27, and 35.
The previews are index visualizations, not palette-accurate game renders.
Original ARC hashes and payload source offsets are in raw/manifest.json.
"""


def package(root: Path, output: Path, overwrite: bool) -> tuple[int, int]:
    groups = [(prefix, root / folder) for prefix, folder in GROUPS]
    missing = [str(folder) for _, folder in groups if not folder.is_dir()]
    if missing:
        raise FileNotFoundError("Missing generated analysis folders: " + ", ".join(missing))
    if output.exists() and not overwrite:
        raise FileExistsError(f"Refusing to overwrite existing bundle: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)

    file_count = 0
    with ZipFile(output, "w", ZIP_DEFLATED, compresslevel=6) as bundle:
        bundle.writestr("README.txt", README)
        for prefix, folder in groups:
            for path in sorted(folder.rglob("*")):
                if path.is_file():
                    bundle.write(path, Path(prefix) / path.relative_to(folder))
                    file_count += 1
    return file_count, output.stat().st_size


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", nargs="?", type=Path, default=Path("build/rockman"))
    parser.add_argument("--output", type=Path, default=Path("build/rockman/x4-arc-analysis.zip"))
    parser.add_argument("--overwrite", action="store_true")
    args = parser.parse_args()
    count, size = package(args.root, args.output, args.overwrite)
    print(f"Packaged {count} analysis files ({size} ZIP bytes) at {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
