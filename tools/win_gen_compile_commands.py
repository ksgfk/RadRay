#!/usr/bin/env python3
from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from collections import defaultdict, deque
from pathlib import Path
from typing import Any

sys.dont_write_bytecode = True


EXPORT_TARGET = "RadRayExportCompileCommands"
MSBUILD_PROPERTIES = (
    "CLToolExe",
    "CLToolPath",
    "LLVMInstallDir",
    "ExecutablePath",
    "PlatformToolset",
)


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


def find_solution(build_dir: Path, solution_arg: Path | None) -> Path:
    if solution_arg is not None:
        solution = resolve_existing_path(solution_arg, "Solution")
        if not solution.is_file():
            raise ScriptError(f"Solution is not a file: {solution}")
        if solution.suffix.lower() not in {".sln", ".slnx"}:
            raise ScriptError(f"Unsupported solution format: {solution}")
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
            check=False,
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


def local_name(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def parse_slnx_projects(solution: Path) -> list[Path]:
    try:
        root = ET.parse(solution).getroot()
    except (ET.ParseError, OSError) as exc:
        raise ScriptError(f"Failed to parse solution {solution}: {exc}") from exc

    project_paths: list[str] = []
    for element in root.iter():
        if local_name(element.tag) != "Project":
            continue
        path = element.get("Path")
        if path and Path(path).suffix.lower() == ".vcxproj":
            project_paths.append(path)
    return resolve_project_paths(solution, project_paths)


def parse_sln_projects(solution: Path) -> list[Path]:
    project_pattern = re.compile(
        r'^Project\("\{[^}]+\}"\)\s*=\s*"[^"]*",\s*"([^"]+\.vcxproj)"',
        re.IGNORECASE,
    )
    try:
        text = solution.read_text(encoding="utf-8-sig", errors="replace")
    except OSError as exc:
        raise ScriptError(f"Failed to read solution {solution}: {exc}") from exc
    return resolve_project_paths(solution, project_pattern.findall(text))


def resolve_project_paths(solution: Path, project_paths: list[str]) -> list[Path]:
    projects: list[Path] = []
    seen: set[str] = set()
    for project_path in project_paths:
        normalized = project_path.replace("\\", os.sep).replace("/", os.sep)
        project = (solution.parent / normalized).resolve()
        key = os.path.normcase(str(project))
        if key in seen:
            continue
        if not project.is_file():
            raise ScriptError(f"Solution project does not exist: {project}")
        seen.add(key)
        projects.append(project)
    if not projects:
        raise ScriptError(f"No .vcxproj projects found in solution: {solution}")
    return projects


def parse_solution_projects(solution: Path) -> list[Path]:
    if solution.suffix.lower() == ".slnx":
        return parse_slnx_projects(solution)
    return parse_sln_projects(solution)


def parse_msbuild_json(stdout: str, project: Path) -> dict[str, Any]:
    decoder = json.JSONDecoder()
    for index, char in enumerate(stdout):
        if char != "{":
            continue
        try:
            value, _ = decoder.raw_decode(stdout[index:])
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict) and "TargetResults" in value:
            return value
    raise ScriptError(f"MSBuild returned no structured target result for {project}")


def path_key(path: str | Path) -> str:
    return os.path.normcase(os.path.normpath(os.path.abspath(path)))


def absolute_path(path: str, directory: Path) -> Path:
    candidate = Path(path)
    if not candidate.is_absolute():
        candidate = directory / candidate
    return Path(os.path.abspath(os.path.normpath(candidate)))


def resolve_compiler(properties: dict[str, Any], project: Path) -> Path:
    tool_exe = str(properties.get("CLToolExe", "")).strip().strip('"')
    if not tool_exe:
        raise ScriptError(f"MSBuild did not evaluate CLToolExe for {project}")

    executable = Path(tool_exe)
    if executable.is_absolute() and executable.is_file():
        return executable.resolve()

    candidates: list[Path] = []
    tool_path = str(properties.get("CLToolPath", "")).strip().strip('"')
    if tool_path:
        candidates.append(Path(tool_path) / tool_exe)

    llvm_install_dir = str(properties.get("LLVMInstallDir", "")).strip().strip('"')
    if llvm_install_dir:
        candidates.append(Path(llvm_install_dir) / "bin" / tool_exe)

    executable_path = str(properties.get("ExecutablePath", ""))
    for directory in executable_path.split(";"):
        directory = directory.strip().strip('"')
        if directory:
            candidates.append(Path(directory) / tool_exe)

    from_environment = shutil.which(tool_exe)
    if from_environment:
        candidates.append(Path(from_environment))

    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()

    toolset = properties.get("PlatformToolset", "unknown")
    raise ScriptError(f"Cannot resolve compiler {tool_exe!r} for {project} (toolset {toolset})")


def quote_windows_argument(argument: str | Path) -> str:
    return subprocess.list2cmdline([str(argument)])


def split_msbuild_item_list(value: str) -> list[str]:
    return [item for item in value.split(";") if item]


def extract_entries(data: dict[str, Any], project: Path) -> list[dict[str, str]]:
    target_results = data.get("TargetResults", {})
    target_result = target_results.get(EXPORT_TARGET)
    if not isinstance(target_result, dict):
        raise ScriptError(f"MSBuild omitted {EXPORT_TARGET} result for {project}")

    items = target_result.get("Items", [])
    if not items:
        return []

    command_items = [item for item in items if item.get("EntryKind") == "Command"]
    output_items = [item for item in items if item.get("EntryKind") == "Output"]
    if not command_items or not output_items:
        raise ScriptError(f"MSBuild returned incomplete command/output data for {project}")

    outputs: defaultdict[str, deque[tuple[Path, Path]]] = defaultdict(deque)
    for item in output_items:
        source_text = str(item.get("Source", ""))
        output_text = str(item.get("CompileOutput", ""))
        working_text = str(item.get("WorkingDirectory", ""))
        if not source_text or not output_text or not working_text:
            raise ScriptError(f"Incomplete output metadata for {project}: {item}")
        working_directory = absolute_path(working_text, project.parent)
        source = absolute_path(source_text, working_directory)
        output = absolute_path(output_text, working_directory)
        outputs[path_key(source)].append((source, output))

    compiler = resolve_compiler(data.get("Properties", {}), project)
    entries: list[dict[str, str]] = []
    for item in command_items:
        working_text = str(item.get("WorkingDirectory", ""))
        options = str(item.get("Identity", "")).strip()
        source_texts = split_msbuild_item_list(str(item.get("Source", "")))
        if not source_texts or not working_text or not options:
            raise ScriptError(f"Incomplete command metadata for {project}: {item}")

        working_directory = absolute_path(working_text, project.parent)
        for source_text in source_texts:
            source = absolute_path(source_text, working_directory)
            matching_outputs = outputs[path_key(source)]
            if not matching_outputs:
                raise ScriptError(f"No object output was evaluated for {source} in {project}")
            _, output = matching_outputs.popleft()

            command = " ".join(
                (
                    quote_windows_argument(compiler),
                    options,
                    quote_windows_argument(source),
                )
            )
            entries.append(
                {
                    "directory": str(working_directory),
                    "command": command,
                    "file": str(source),
                    "output": str(output),
                }
            )

    remaining_outputs = sum(len(values) for values in outputs.values())
    if remaining_outputs:
        raise ScriptError(f"{remaining_outputs} object outputs had no compile command in {project}")
    return entries


def query_project(
    msbuild: Path,
    project: Path,
    export_targets: Path,
    configuration: str,
    platform: str,
    env: dict[str, str],
) -> list[dict[str, str]]:
    command = [
        str(msbuild),
        str(project),
        "-nologo",
        "-verbosity:quiet",
        f"-target:{EXPORT_TARGET}",
        f"-property:Configuration={configuration}",
        f"-property:Platform={platform}",
        "-property:DesignTimeBuild=true",
        "-property:BuildProjectReferences=false",
        f"-property:ForceImportAfterCppTargets={export_targets}",
        f"-getProperty:{','.join(MSBUILD_PROPERTIES)}",
        f"-getTargetResult:{EXPORT_TARGET}",
    ]
    result = subprocess.run(command, env=env, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        details = (result.stderr or result.stdout).strip()
        raise ScriptError(f"MSBuild query failed for {project}:\n{details}")
    data = parse_msbuild_json(result.stdout, project)
    return extract_entries(data, project)


def query_projects(
    msbuild: Path,
    projects: list[Path],
    export_targets: Path,
    configuration: str,
    platform: str,
    jobs: int,
    env: dict[str, str],
) -> list[dict[str, str]]:
    results: list[list[dict[str, str]] | None] = [None] * len(projects)
    failures: list[str] = []

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as executor:
        futures = {
            executor.submit(
                query_project,
                msbuild,
                project,
                export_targets,
                configuration,
                platform,
                env,
            ): index
            for index, project in enumerate(projects)
        }
        for future in concurrent.futures.as_completed(futures):
            index = futures[future]
            try:
                results[index] = future.result()
            except Exception as exc:
                failures.append(str(exc))

    if failures:
        failures.sort()
        raise ScriptError("One or more project queries failed:\n\n" + "\n\n".join(failures))

    entries: list[dict[str, str]] = []
    for project_entries in results:
        if project_entries:
            entries.extend(project_entries)
    return entries


def write_compilation_database(output: Path, entries: list[dict[str, str]]) -> None:
    temporary = output.with_name(f".{output.name}.{os.getpid()}.tmp")
    try:
        with temporary.open("w", encoding="utf-8", newline="\n") as stream:
            json.dump(entries, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
        os.replace(temporary, output)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate compile_commands.json by evaluating Windows MSBuild projects without compiling."
    )
    parser.add_argument("build_dir_pos", nargs="?", type=Path, help="Build directory containing the solution.")
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
    parser.add_argument("-Platform", "--platform", default="x64", help="MSBuild platform.")
    parser.add_argument(
        "-Jobs",
        "-j",
        "--jobs",
        type=int,
        default=min(16, os.cpu_count() or 1),
        help="Parallel project queries.",
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
    if args.jobs < 1:
        parser.error("--jobs must be at least 1.")
    return args


def main() -> int:
    args = parse_args()
    env = os.environ.copy()
    env["MSBUILDDISABLENODEREUSE"] = "1"
    env.setdefault("VSLANG", "1033")

    try:
        repo_root = get_repo_root()
        build_dir = resolve_build_dir(args.build_dir)
        output = resolve_output_path(args.output)
        export_targets = resolve_existing_path(
            repo_root / "tools" / "win_gen_compile_commands.targets",
            "MSBuild export targets",
        )
        solution = find_solution(build_dir, args.solution)
        projects = parse_solution_projects(solution)
        msbuild = find_msbuild(args.msbuild)

        info(f"Solution: {solution}")
        info(f"MSBuild: {msbuild}")
        info(
            f"Evaluating {len(projects)} projects for "
            f"{args.configuration}|{args.platform} with {args.jobs} workers"
        )
        entries = query_projects(
            msbuild,
            projects,
            export_targets,
            args.configuration,
            args.platform,
            args.jobs,
            env,
        )
        if not entries:
            raise ScriptError("No C/C++ compile commands were generated")

        write_compilation_database(output, entries)
        info(f"Generated {len(entries)} entries: {output}")
        return 0
    except ScriptError as exc:
        error(str(exc))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
