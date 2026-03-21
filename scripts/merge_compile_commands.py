#!/usr/bin/env python3

import json
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_INPUTS = (
    REPO_ROOT / "tests/build/compile_commands.json",
    REPO_ROOT / "products/go/build/compile_commands.json",
)
DEFAULT_OUTPUT = REPO_ROOT / "compile_commands.json"


def load_entries(path: Path) -> list[dict]:
    with path.open("r", encoding="utf-8") as file:
        data = json.load(file)

    if not isinstance(data, list):
        raise ValueError(f"{path} does not contain a compile commands array")

    return data


def merge_entries(paths: tuple[Path, ...]) -> list[dict]:
    merged: list[dict] = []
    seen_files: set[str] = set()

    for path in paths:
        if not path.exists():
            continue

        for entry in load_entries(path):
            file_path = entry.get("file")
            if not isinstance(file_path, str) or file_path in seen_files:
                continue

            merged.append(entry)
            seen_files.add(file_path)

    return merged


def main() -> int:
    merged = merge_entries(DEFAULT_INPUTS)
    if not merged:
        missing = "\n".join(str(path) for path in DEFAULT_INPUTS)
        print("No compile_commands inputs found. Expected one or more of:\n"
              f"{missing}",
              file=sys.stderr)
        return 1

    with DEFAULT_OUTPUT.open("w", encoding="utf-8") as file:
        json.dump(merged, file, indent=2)
        file.write("\n")

    print(f"Wrote {len(merged)} entries to {DEFAULT_OUTPUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
