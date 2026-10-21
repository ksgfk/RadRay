#!/usr/bin/env python3
"""Expand ordinary CMake C/C++ unity entries into per-source clangd commands."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import sys
from typing import Any, NoReturn


def fail(message: str) -> NoReturn:
    print(f"[ERR ] {message}", file=sys.stderr)
    sys.exit(1)


def absolute_path(value: str, directory: Path) -> Path:
    return (directory / value).resolve()


def path_key(path: Path) -> str:
    return os.path.normcase(str(path))


def split_windows_command(command: str) -> list[str]:
    """Decode double quotes and backslashes using Windows argv conventions."""
    arguments: list[str] = []
    index = 0
    while index < len(command):
        if command[index] in " \t\r\n":
            index += 1
            continue
        argument: list[str] = []
        quoted = False
        while index < len(command):
            if command[index] in " \t\r\n" and not quoted:
                break
            backslashes = 0
            while index < len(command) and command[index] == "\\":
                backslashes += 1
                index += 1
            if index < len(command) and command[index] == '"':
                argument.append("\\" * (backslashes // 2))
                if backslashes % 2:
                    argument.append('"')
                elif quoted and command[index:index + 2] == '""':
                    argument.append('"')
                    index += 1
                else:
                    quoted = not quoted
                index += 1
            else:
                argument.append("\\" * backslashes)
                if index < len(command):
                    if command[index] in " \t\r\n" and not quoted:
                        break
                    argument.append(command[index])
                    index += 1
        if quoted:
            fail("Unterminated quote in Windows compile command")
        arguments.append("".join(argument))
    return arguments


def entry_arguments(entry: dict[str, Any], command_style: str) -> list[str]:
    if "arguments" in entry:
        arguments = entry["arguments"]
        if not isinstance(arguments, list) or not all(isinstance(arg, str) for arg in arguments):
            fail(f"Invalid arguments array for {entry['file']}")
        arguments = list(arguments)
    else:
        command = entry.get("command")
        if not isinstance(command, str):
            fail(f"Missing compile command for {entry['file']}")
        arguments = split_windows_command(command) if command_style == "windows" else shlex.split(command)
    if not arguments or not arguments[0]:
        fail(f"Empty compile command for {entry['file']}")
    if any(arg.startswith("@") for arg in arguments[1:]):
        fail(f"Response files are not supported in unity commands: {entry['file']}")
    return arguments


def include_directories(arguments: list[str], directory: Path) -> list[Path]:
    directories: list[Path] = []
    prefixes = ("-iquote", "-isystem", "-idirafter", "-imsvc", "/imsvc", "-external:I", "/external:I", "-I", "/I")
    index = 1
    while index < len(arguments):
        argument = arguments[index]
        for prefix in prefixes:
            if argument == prefix:
                index += 1
                if index == len(arguments):
                    fail(f"Missing directory after {prefix}")
                directories.append(absolute_path(arguments[index], directory))
                break
            if argument.startswith(prefix):
                directories.append(absolute_path(argument[len(prefix):], directory))
                break
        index += 1
    return directories


def unity_sources(unity: Path, directories: list[Path]) -> list[Path]:
    if not unity.is_file():
        fail(f"Unity source does not exist: {unity}")
    contents = unity.read_text(encoding="utf-8-sig")
    contents = re.sub(r"/\*.*?\*/", "", contents, flags=re.DOTALL)
    sources: list[Path] = []
    seen: set[str] = set()
    for line_number, line in enumerate(contents.splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("//"):
            continue
        include = re.fullmatch(r'#\s*include\s*"([^"\r\n]+)"\s*(?://.*)?', line)
        if include is None:
            fail(f"Unsupported unity code at {unity}:{line_number}; only source includes and comments are supported")
        included = include.group(1)
        candidates = [absolute_path(included, parent) for parent in (unity.parent, *directories)]
        source = next((candidate for candidate in candidates if candidate.is_file()), None)
        if source is None:
            fail(f"Included source does not exist: {included} (from {unity})")
        if source.suffix.lower() not in {".c", ".cc", ".cpp", ".cxx", ".c++"}:
            fail(f"Unsupported unity include: {source}")
        if path_key(source) in seen:
            fail(f"Repeated source include in {unity}: {source}")
        seen.add(path_key(source))
        sources.append(source)
    if not sources:
        fail(f"No source includes found in {unity}")
    return sources


def indexing_arguments(arguments: list[str]) -> list[str]:
    """Keep textual forced includes; discard shared outputs and MSVC PCH use."""
    result = [arguments[0]]
    index = 1
    value_options = ("-o", "-MF", "-MT", "-MQ", "/Fo", "-Fo", "/Fd", "-Fd", "/Fp", "-Fp")
    joined_options = ("-MF", "-MT", "-MQ", "/Fo", "-Fo", "/Fd", "-Fd", "/Fp", "-Fp", "/Yu", "-Yu", "/Yc", "-Yc")
    while index < len(arguments):
        argument = arguments[index]
        if argument in value_options:
            if index + 1 == len(arguments):
                fail(f"Missing value after {argument}")
            index += 2
            continue
        if argument.startswith(joined_options):
            index += 1
            continue
        result.append(argument)
        index += 1
    return result


def replace_source(arguments: list[str], unity: Path, source: Path, directory: Path) -> list[str]:
    result = list(arguments)
    replacements = 0
    for index, argument in enumerate(arguments[1:], 1):
        prefix = argument[:3] if argument.startswith(("/Tp", "-Tp", "/Tc", "-Tc")) else ""
        value = argument[len(prefix):]
        if value and path_key(absolute_path(value, directory)) == path_key(unity):
            result[index] = prefix + str(source)
            replacements += 1
    if replacements != 1:
        fail(f"Expected one unity input in compile command, found {replacements}: {unity}")
    return result


def expand_database(database: Any, database_dir: Path, command_style: str) -> tuple[list[dict[str, Any]], int]:
    if not isinstance(database, list):
        fail("Compilation database must be a JSON array")
    expanded: list[dict[str, Any]] = []
    unity_count = 0
    for entry in database:
        if not isinstance(entry, dict) or not all(
            isinstance(entry.get(key), str) and entry[key] for key in ("file", "directory")
        ):
            fail("Every compilation entry must have nonempty file and directory strings")
        directory = absolute_path(entry["directory"], database_dir)
        unity = absolute_path(entry["file"], directory)
        if unity.parent.name.lower() != "unity" or not re.fullmatch(r"unity_.+\.(?:c|cxx)", unity.name):
            expanded.append(entry)
            continue
        arguments = entry_arguments(entry, command_style)
        sources = unity_sources(unity, include_directories(arguments, directory))
        arguments = indexing_arguments(arguments)
        for source in sources:
            generated = dict(entry)
            generated.pop("command", None)
            generated.pop("output", None)
            generated["directory"] = str(directory)
            generated["file"] = str(source)
            generated["arguments"] = replace_source(arguments, unity, source, directory)
            expanded.append(generated)
        unity_count += 1
    return expanded, unity_count


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("database", type=Path, help="Input compile_commands.json or its build directory.")
    parser.add_argument(
        "-o", "--output", type=Path, default=Path(".vscode/compile_commands.json"),
        help="Output path (default: .vscode/compile_commands.json).",
    )
    parser.add_argument(
        "--command-style", choices=("windows", "posix"), default="windows" if os.name == "nt" else "posix",
        help="Quoting style for command strings (default: host platform); arguments arrays need no decoding.",
    )
    args = parser.parse_args()
    database_path = args.database.resolve()
    if database_path.is_dir():
        database_path /= "compile_commands.json"
    if not database_path.is_file():
        parser.error(f"Input compilation database does not exist: {database_path}")
    output = args.output.resolve()
    if path_key(database_path) == path_key(output):
        parser.error("Input and output must differ; keep the original build database")
    database = json.loads(database_path.read_text(encoding="utf-8-sig"))
    entries, unity_count = expand_database(database, database_path.parent, args.command_style)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(f".{output.name}.{os.getpid()}.tmp")
    temporary.write_text(json.dumps(entries, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
    temporary.replace(output)
    print(f"[INFO] Expanded {unity_count} unity entries: {len(database)} -> {len(entries)} entries: {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
