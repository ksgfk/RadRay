#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True


class ScriptError(Exception):
    pass


def info(message: str) -> None:
    print(f"[INFO] {message}")


def warn(message: str) -> None:
    print(f"[WARN] {message}")


def error(message: str) -> None:
    print(f"[ERR ] {message}", file=sys.stderr)


def get_repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def resolve_existing_path(path: Path, description: str) -> Path:
    try:
        return path.resolve(strict=True)
    except FileNotFoundError as exc:
        raise ScriptError(f"{description} does not exist: {path}") from exc


def resolve_build_dir(path: Path) -> Path:
    build_dir = resolve_existing_path(path, "BuildDir")
    if not build_dir.is_dir():
        raise ScriptError(f"BuildDir is not a directory: {build_dir}")
    return build_dir


def resolve_output_path(path: Path) -> Path:
    output = path if path.is_absolute() else Path.cwd() / path
    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    return output


def run(args: list[str], env: dict[str, str]) -> None:
    try:
        result = subprocess.run(args, env=env)
    except FileNotFoundError as exc:
        raise ScriptError(f"Command not found: {args[0]}") from exc
    if result.returncode != 0:
        raise ScriptError(f"Command failed with exit code {result.returncode}: {' '.join(args)}")


def build_logger(logger_project_dir: Path, env: dict[str, str]) -> Path:
    if not logger_project_dir.is_dir():
        raise ScriptError(f"Logger project directory not found: {logger_project_dir}")

    info("Building logger project (Release)")
    run(["dotnet", "build", str(logger_project_dir), "-c", "Release", "--nologo"], env)

    release_dir = logger_project_dir / "bin" / "Release"
    dlls = sorted(release_dir.rglob("CompileCommandsJson.dll")) if release_dir.exists() else []
    if not dlls:
        raise ScriptError("CompileCommandsJson.dll not found")
    return dlls[0].resolve()


def find_solution(build_dir: Path, solution_arg: Path | None) -> Path:
    if solution_arg is not None:
        solution = resolve_existing_path(solution_arg, "Solution")
        if not solution.is_file():
            raise ScriptError(f"Solution is not a file: {solution}")
        return solution

    candidates = sorted(
        (
            path
            for path in build_dir.iterdir()
            if path.is_file() and path.suffix.lower() in {".sln", ".slnx"}
        ),
        key=lambda path: str(path).lower(),
    )
    if not candidates:
        raise ScriptError(f"No .sln/.slnx found in build directory top level: {build_dir}")
    if len(candidates) > 1:
        warn(f"Multiple .sln/.slnx files found, using first: {candidates[0]}")
    return candidates[0].resolve()


def find_msbuild(msbuild_arg: Path | None) -> Path:
    if msbuild_arg is not None:
        msbuild = resolve_existing_path(msbuild_arg, "MSBuild")
        if not msbuild.is_file():
            raise ScriptError(f"MSBuild is not a file: {msbuild}")
        return msbuild

    vswhere_candidates = [
        Path(os.environ[key]) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
        for key in ("ProgramFiles(x86)", "ProgramFiles")
        if os.environ.get(key)
    ]
    for vswhere in vswhere_candidates:
        if not vswhere.is_file():
            continue
        result = subprocess.run(
            [
                str(vswhere),
                "-latest",
                "-products",
                "*",
                "-requires",
                "Microsoft.Component.MSBuild",
                "-find",
                r"MSBuild\**\Bin\MSBuild.exe",
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            continue
        found = next((line.strip() for line in result.stdout.splitlines() if line.strip()), None)
        if found:
            info(f"vswhere found MSBuild: {found}")
            return Path(found).resolve()

    from_path = shutil.which("MSBuild.exe") or shutil.which("msbuild")
    if from_path:
        return Path(from_path).resolve()

    raise ScriptError("msbuild.exe not found. Install VS Build Tools / Visual Studio or pass --msbuild.")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate compile_commands.json from a Windows MSBuild solution.")
    parser.add_argument("build_dir_pos", nargs="?", type=Path, help="Build directory containing the generated solution.")
    parser.add_argument("-BuildDir", "--build-dir", dest="build_dir", type=Path, help="Build directory.")
    parser.add_argument(
        "-Output",
        "-o",
        "--output",
        type=Path,
        default=Path(".vscode") / "compile_commands.json",
        help="Output compile_commands.json path.",
    )
    parser.add_argument(
        "-Configuration",
        "-c",
        "--configuration",
        default="Debug",
        help="MSBuild configuration.",
    )
    parser.add_argument("-Solution", "-s", "--solution", type=Path, help="Solution path.")
    parser.add_argument("-MsBuild", "--msbuild", type=Path, help="MSBuild executable path.")
    args = parser.parse_args()

    if args.build_dir is not None and args.build_dir_pos is not None:
        parser.error("Build directory was passed twice.")
    if args.build_dir is None:
        args.build_dir = args.build_dir_pos
    if args.build_dir is None:
        parser.error("Build directory is required. Pass it positionally or with --build-dir.")
    return args


def main() -> int:
    args = parse_args()
    env = os.environ.copy()
    env["MSBUILDDISABLENODEREUSE"] = "1"

    try:
        repo_root = get_repo_root()
        build_dir = resolve_build_dir(args.build_dir)
        output = resolve_output_path(args.output)
        logger_dll = build_logger(repo_root / "cmake" / "MsBuildCompileCommandsJson", env)
        info(f"Logger DLL: {logger_dll}")

        solution = find_solution(build_dir, args.solution)
        info(f"Using solution: {solution}")

        msbuild = find_msbuild(args.msbuild)
        info(f"MSBuild: {msbuild}")

        logger_arg = f"/logger:{logger_dll};{output}"
        msbuild_args = [
            str(msbuild),
            str(solution),
            "/t:Rebuild",
            f"/p:Configuration={args.configuration}",
            "/m",
            logger_arg,
        ]
        info("Starting Rebuild to generate compile_commands.json")
        run(msbuild_args, env)

        if not output.exists():
            raise ScriptError(f"Rebuild finished but output was not generated: {output}")
        print(f"Generated: {output}")
        return 0
    except ScriptError as exc:
        error(str(exc))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
