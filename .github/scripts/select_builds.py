#!/usr/bin/env python3

"""Select firmware products and host tests from a Git change set."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Iterable, Sequence


ZERO_SHA = "0" * 40

DOCUMENTATION_CONFIG_PATHS = {
    ".markdownlint-cli2.jsonc",
    ".markdownlint.json",
}

NON_BUILD_PATHS = {
    ".clang-format",
    ".editorconfig",
    ".gitignore",
    ".github/workflows/pre-commit.yml",
    ".pre-commit-config.yaml",
    "scripts/merge_compile_commands.py",
}

HOST_TEST_ONLY_PATHS = {
    ".github/scripts/test_select_builds.py",
    ".github/workflows/host-tests.yml",
}

ALL_BUILD_PATHS = {
    ".github/scripts/select_builds.py",
    ".github/workflows/firmware-build.yml",
    ".gitmodules",
}


@dataclass(frozen=True)
class BuildSelection:
    products: tuple[str, ...]
    host_tests_required: bool

    @property
    def firmware_required(self) -> bool:
        return bool(self.products)


def discover_products(repository_root: Path) -> tuple[str, ...]:
    products_root = repository_root / "products"
    products = (
        path.name
        for path in products_root.iterdir()
        if path.is_dir() and (path / "CMakeLists.txt").is_file()
    )
    return tuple(sorted(products))


def is_documentation(path: PurePosixPath) -> bool:
    path_string = path.as_posix()
    if path_string in DOCUMENTATION_CONFIG_PATHS:
        return True
    if path.name == "LICENSE" or path.name.startswith("LICENSE."):
        return True

    parts = path.parts
    if not parts:
        return False
    if parts[0] == "docs":
        return True
    if len(parts) == 1:
        return path.suffix.lower() == ".md"
    if parts[0] == ".github":
        return path.suffix.lower() == ".md"
    if parts[0] == "tests":
        return path.suffix.lower() == ".md"

    if parts[0] == "components":
        if len(parts) <= 3:
            return path.suffix.lower() == ".md"
        return path.name in {"CHANGELOG.md", "README.md"}

    if parts[0] == "products":
        if len(parts) <= 3:
            return path.suffix.lower() == ".md"
        if parts[2] in {"docs", "specs"}:
            return True
        return "tests" in parts[2:] and path.name == "README.md"

    return False


def is_test_path(path: PurePosixPath) -> bool:
    if path.parts and path.parts[0] == "tests":
        return True
    if len(path.parts) < 3 or path.parts[0] not in {"components", "products"}:
        return False
    return "tests" in path.parts[2:]


def classify_paths(
    changed_paths: Iterable[str], all_products: Sequence[str]
) -> BuildSelection:
    known_products = set(all_products)
    selected_products: set[str] = set()
    host_tests_required = False

    for path_string in changed_paths:
        path = PurePosixPath(path_string)
        normalized_path = path.as_posix()

        if is_documentation(path) or normalized_path in NON_BUILD_PATHS:
            continue

        if is_test_path(path) or normalized_path in HOST_TEST_ONLY_PATHS:
            host_tests_required = True
            continue

        if normalized_path in ALL_BUILD_PATHS:
            selected_products.update(known_products)
            host_tests_required = True
            continue

        if path.parts and path.parts[0] == "components":
            selected_products.update(known_products)
            host_tests_required = True
            continue

        if len(path.parts) >= 2 and path.parts[0] == "products":
            product = path.parts[1]
            if product in known_products:
                selected_products.add(product)
            else:
                selected_products.update(known_products)
            host_tests_required = True
            continue

        # Unknown files fail safe by selecting every known product.
        selected_products.update(known_products)
        host_tests_required = True

    return BuildSelection(
        products=tuple(sorted(selected_products)),
        host_tests_required=host_tests_required,
    )


def commit_exists(repository_root: Path, revision: str) -> bool:
    result = subprocess.run(
        ["git", "cat-file", "-e", f"{revision}^{{commit}}"],
        cwd=repository_root,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return result.returncode == 0


def read_changed_paths(
    repository_root: Path,
    base: str,
    head: str,
    use_merge_base: bool = False,
) -> list[str]:
    if base == ZERO_SHA or not commit_exists(repository_root, base):
        raise ValueError(f"base commit is unavailable: {base}")
    if not commit_exists(repository_root, head):
        raise ValueError(f"head commit is unavailable: {head}")

    comparison = f"{base}...{head}" if use_merge_base else f"{base}..{head}"
    result = subprocess.run(
        [
            "git",
            "diff",
            "--name-only",
            "--no-renames",
            "-z",
            comparison,
        ],
        cwd=repository_root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        error = result.stderr.decode(errors="replace").strip()
        raise RuntimeError(f"git diff failed: {error}")

    return [
        os.fsdecode(path)
        for path in result.stdout.split(b"\0")
        if path
    ]


def write_github_outputs(output_path: Path, selection: BuildSelection) -> None:
    products = json.dumps(selection.products, separators=(",", ":"))
    with output_path.open("a", encoding="utf-8") as output:
        output.write(f"products={products}\n")
        output.write(
            f"firmware_required={str(selection.firmware_required).lower()}\n"
        )
        output.write(
            f"host_tests_required={str(selection.host_tests_required).lower()}\n"
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True, help="Base Git revision")
    parser.add_argument("--head", required=True, help="Head Git revision")
    parser.add_argument(
        "--merge-base",
        action="store_true",
        help="Compare the merge base with the head revision",
    )
    parser.add_argument(
        "--github-output",
        type=Path,
        default=None,
        help="GitHub Actions output file",
    )
    parser.add_argument(
        "--repository-root",
        type=Path,
        default=Path.cwd(),
        help="Repository root",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    repository_root = args.repository_root.resolve()
    products = discover_products(repository_root)
    if not products:
        print("No firmware products were discovered", file=sys.stderr)
        return 1

    try:
        changed_paths = read_changed_paths(
            repository_root,
            args.base,
            args.head,
            use_merge_base=args.merge_base,
        )
        selection = classify_paths(changed_paths, products)
    except (RuntimeError, ValueError) as error:
        print(f"{error}; selecting all builds", file=sys.stderr)
        changed_paths = []
        selection = BuildSelection(products=products, host_tests_required=True)

    print("Changed paths:")
    for path in changed_paths:
        print(f"  {path}")
    print(f"Firmware products: {', '.join(selection.products) or 'none'}")
    print(f"Host tests required: {selection.host_tests_required}")

    if args.github_output is not None:
        write_github_outputs(args.github_output, selection)

    return 0


if __name__ == "__main__":
    sys.exit(main())
