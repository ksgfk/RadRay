"""Explicit, offline dependency build/install stage. See docs/guide/build-test.md."""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def fail(message):
    sys.exit(f"error: {message}")


def run(command, cwd, log):
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("w", encoding="utf-8") as output:
        output.write(subprocess.list2cmdline([str(x) for x in command]) + "\n")
        output.flush()
        result = subprocess.run(command, cwd=cwd, stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        print(log.read_text(encoding="utf-8", errors="replace")[-12000:])
        fail(f"command failed ({result.returncode}); log: {log}")


def triplets():
    return ("windows-x64-msvc", "windows-x64-clangcl", "macos-arm64-appleclang")


def toolchain_arguments(triplet):
    if triplet == "windows-x64-msvc":
        args = ["-G", "Ninja", "-DCMAKE_C_COMPILER=cl", "-DCMAKE_CXX_COMPILER=cl"]
    elif triplet == "windows-x64-clangcl":
        args = ["-G", "Visual Studio 18 2026", "-A", "x64", "-T", "ClangCL"]
    elif triplet == "macos-arm64-appleclang":
        return ["-G", "Ninja", "-DCMAKE_C_COMPILER=/usr/bin/clang",
                "-DCMAKE_CXX_COMPILER=/usr/bin/clang++", "-DCMAKE_OSX_ARCHITECTURES=arm64"]
    else:
        fail(f"unsupported triplet: {triplet}")
    return [*args, "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"]


def load_catalog(root, triplet):
    entries = json.loads((root / "cmake/DependencyCatalog.json").read_text(encoding="utf-8"))
    platform = triplet.split("-", 1)[0]
    return {entry["name"]: entry for entry in entries
            if platform in entry.get("platforms", [platform])}


def closure(names, catalog):
    ordered = []

    def visit(name, ancestors):
        if name not in catalog:
            fail(f"unknown or unsupported dependency: {name}")
        if name in ancestors:
            fail(f"dependency cycle: {ancestors + [name]}")
        if name not in ordered:
            for dependency in catalog[name]["requires"]:
                visit(dependency, ancestors + [name])
            ordered.append(name)

    for name in names:
        visit(name, [])
    return ordered


def build_directory(root, name, triplet, config):
    return root / "build_dependencies" / name / triplet / config / "build"


def install_prefix(root, name, triplet, config):
    return root / "SDKs" / name / triplet / config


def validate_output_path(path, directory):
    work = directory.absolute()
    resolved = path.resolve()
    if resolved == work or not resolved.is_relative_to(work) or resolved != path.absolute():
        fail(f"refusing output outside its dependency directory or through a link: {path}")


def build_packages(root, catalog, selected, triplet, config, parallel, clean):
    # Check every selected source and output before cleaning or building anything.
    for name in selected:
        source = root / "third_party" / catalog[name]["source"]
        if not source.is_dir():
            fail(f"missing source: {source}. Run: python tools/fetch_third_party.py restore --only {name}")
        validate_output_path(build_directory(root, name, triplet, config), root / "build_dependencies" / name)
        validate_output_path(install_prefix(root, name, triplet, config), root / "SDKs" / name)
    common = toolchain_arguments(triplet)
    for name in selected:
        entry = catalog[name]
        build = build_directory(root, name, triplet, config)
        prefix = install_prefix(root, name, triplet, config)
        if clean:
            for path in (build, prefix):
                if path.exists():
                    shutil.rmtree(path)
        print(f"Building {name}: {triplet}/{config}", flush=True)
        prefixes = [install_prefix(root, dependency, triplet, config)
                    for dependency in closure(entry["requires"], catalog)]
        options = {key: ("ON" if triplet.startswith("windows-") else "OFF")
                   if value == "${WIN32}" else value for key, value in entry["options"].items()}
        run(["cmake", "-S", str(root / "cmake/dependencies"), "-B", str(build), *common,
             f"-DCMAKE_BUILD_TYPE={config}", f"-DCMAKE_CONFIGURATION_TYPES={config}",
             "-DCMAKE_CXX_STANDARD=20", "-DCMAKE_INSTALL_LIBDIR=lib",
             f"-DCMAKE_INSTALL_PREFIX={prefix.as_posix()}",
             f"-DCMAKE_PREFIX_PATH={';'.join(path.as_posix() for path in prefixes)}",
             f"-DRADRAY_DEPENDENCY_TRIPLET={triplet}", f"-DRADRAY_DEPENDENCY_NAME={name}",
             f"-DRADRAY_DEPENDENCY_SOURCE={(root / 'third_party' / entry['source']).as_posix()}",
             *[f"-D{key}={value}" for key, value in options.items()]], root, build / "configure.log")
        run(["cmake", "--build", str(build), "--config", config, "--parallel", str(parallel)],
            root, build / "build.log")
        run(["cmake", "--install", str(build), "--config", config], root, build / "install.log")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["build", "list"])
    parser.add_argument("--triplet", required=True, choices=triplets())
    parser.add_argument("--config", required=True, choices=["Debug", "Release"])
    parser.add_argument("--parallel", type=int, default=8)
    parser.add_argument("--only", action="append", default=[])
    parser.add_argument("--clean", action="store_true", help="Remove selected build/install directories before building")
    args = parser.parse_args()
    if args.parallel < 1:
        parser.error("--parallel must be positive")
    if args.command == "list" and args.clean:
        parser.error("--clean requires build")
    root = Path(__file__).resolve().parents[1]
    catalog = load_catalog(root, args.triplet)
    selected = closure([name for item in args.only for name in item.split(",")] if args.only else catalog, catalog)
    if args.command == "list":
        for name in selected:
            print(f"{name} (requires: {', '.join(catalog[name]['requires']) or 'none'})")
            print(f"  build: {build_directory(root, name, args.triplet, args.config)}")
            print(f"  install: {install_prefix(root, name, args.triplet, args.config)}")
        return
    if (args.triplet.startswith("windows-") and os.name != "nt") or (
            args.triplet.startswith("macos-") and sys.platform != "darwin"):
        fail(f"{args.triplet} must be built on its native platform")
    build_packages(root, catalog, selected, args.triplet, args.config, args.parallel, args.clean)
    preset = {"windows-x64-msvc": f"win-x64-{args.config.lower()}",
              "windows-x64-clangcl": f"win-x64-{args.config.lower()}-clangcl",
              "macos-arm64-appleclang": f"macos-arm64-{args.config.lower()}"}[args.triplet]
    print(f"Installed {len(selected)} dependencies for {args.triplet}/{args.config}.")
    print(f"Configure RadRay explicitly after preparing all required packages and SDKs: cmake --preset {preset}")


if __name__ == "__main__":
    main()
