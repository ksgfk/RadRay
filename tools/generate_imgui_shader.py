#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True

SHADERS = [
    ("VertexShaderDXIL", "VSMain", "vs_6_0", "dxil"),
    ("PixelShaderDXIL", "PSMain", "ps_6_0", "dxil"),
    ("VertexShaderSPIRV", "VSMain", "vs_6_0", "spv"),
    ("PixelShaderSPIRV", "PSMain", "ps_6_0", "spv"),
]


def get_repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def get_manifest_dxc_version(repo_root: Path) -> str | None:
    manifest_path = repo_root / "project_manifest.json"
    if not manifest_path.exists():
        return None
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    for artifact in manifest.get("Artifacts", []):
        if artifact.get("Name") == "dxc":
            return artifact.get("Version")
    return None


def find_dxc(repo_root: Path, dxc_arg: str | None) -> Path:
    if dxc_arg:
        dxc = Path(dxc_arg)
        if dxc.exists():
            return dxc
        raise FileNotFoundError(f"DXC executable not found: {dxc}")

    exe_name = "dxc.exe" if os.name == "nt" else "dxc"
    version = get_manifest_dxc_version(repo_root)
    candidates: list[Path] = []
    if version:
        candidates.append(repo_root / "SDKs" / "dxc" / f"v{version}" / "extracted" / "bin" / exe_name)

    sdk_root = repo_root / "SDKs" / "dxc"
    if sdk_root.exists():
        candidates.extend(sorted(sdk_root.glob(f"v*/extracted/**/{exe_name}"), reverse=True))

    for candidate in candidates:
        if candidate.exists():
            return candidate
    raise FileNotFoundError("DXC executable not found under SDKs/dxc")


def find_spirv_opt(spirv_opt_arg: str | None) -> Path | None:
    if spirv_opt_arg:
        spirv_opt = Path(spirv_opt_arg)
        if spirv_opt.exists():
            return spirv_opt
        raise FileNotFoundError(f"spirv-opt executable not found: {spirv_opt}")

    exe_name = "spirv-opt.exe" if os.name == "nt" else "spirv-opt"
    from_path = shutil.which(exe_name)
    if from_path:
        return Path(from_path)

    vulkan_sdk = os.environ.get("VULKAN_SDK")
    if vulkan_sdk:
        candidate = Path(vulkan_sdk) / "Bin" / exe_name
        if candidate.exists():
            return candidate
    return None


def run_dxc(
    dxc: Path,
    hlsl: Path,
    entry_point: str,
    profile: str,
    output: Path,
    spirv: bool,
) -> None:
    args = [
        str(dxc),
        "-nologo",
        "-Wno-ignored-attributes",
        "-all_resources_bound",
        "-HV",
        "2021",
        "-O3",
        "-T",
        profile,
        "-E",
        entry_point,
        "-Fo",
        str(output),
    ]
    if spirv:
        args.extend([
            "-spirv",
            "-fspv-target-env=vulkan1.2",
            "-fspv-preserve-bindings",
        ])
    else:
        args.extend([
            "-Qstrip_debug",
            "-Qstrip_reflect",
            "-Qstrip_priv",
            "-Qstrip_rootsignature",
        ])
    args.append(str(hlsl))

    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        command = " ".join(args)
        raise RuntimeError(
            f"DXC failed with exit code {result.returncode}\n"
            f"Command: {command}\n"
            f"stdout:\n{result.stdout}\n"
            f"stderr:\n{result.stderr}"
        )
    if result.stdout.strip():
        print(result.stdout.strip())
    if result.stderr.strip():
        print(result.stderr.strip())


def optimize_spirv(spirv_opt: Path, path: Path) -> None:
    out_path = path.with_suffix(".opt.spv")
    args = [
        str(spirv_opt),
        "-Os",
        "--compact-ids",
        "--strip-debug",
        str(path),
        "-o",
        str(out_path),
    ]
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        command = " ".join(args)
        raise RuntimeError(
            f"spirv-opt failed with exit code {result.returncode}\n"
            f"Command: {command}\n"
            f"stdout:\n{result.stdout}\n"
            f"stderr:\n{result.stderr}"
        )
    out_path.replace(path)


def format_bytes(data: bytes, indent: str = "    ", columns: int = 12) -> str:
    lines: list[str] = []
    for offset in range(0, len(data), columns):
        chunk = data[offset : offset + columns]
        values = ", ".join(f"std::byte{{0x{value:02x}}}" for value in chunk)
        lines.append(f"{indent}{values},")
    return "\n".join(lines)


def make_array(name: str, data: bytes) -> str:
    return (
        f"alignas(4) constexpr std::byte kImGui{name}[] = {{\n"
        f"{format_bytes(data)}\n"
        "};\n"
    )


def make_getter(function_name: str, array_name: str) -> str:
    return (
        f"std::span<const std::byte> {function_name}() noexcept {{\n"
        f"    return kImGui{array_name};\n"
        "}\n"
    )


def generate_cpp(hlsl_path: Path, blobs: dict[str, bytes]) -> str:
    source_bytes = hlsl_path.read_bytes()
    try:
        rel_hlsl = hlsl_path.relative_to(get_repo_root()).as_posix()
    except ValueError:
        rel_hlsl = hlsl_path.as_posix()

    arrays = [
        make_array("HLSL", source_bytes),
        make_array("VertexShaderDXIL", blobs["VertexShaderDXIL"]),
        make_array("PixelShaderDXIL", blobs["PixelShaderDXIL"]),
        make_array("VertexShaderSPIRV", blobs["VertexShaderSPIRV"]),
        make_array("PixelShaderSPIRV", blobs["PixelShaderSPIRV"]),
    ]

    getters = [
        make_getter("GetImGuiHLSL", "HLSL"),
        make_getter("GetImGuiVertexShaderDXIL", "VertexShaderDXIL"),
        make_getter("GetImGuiPixelShaderDXIL", "PixelShaderDXIL"),
        make_getter("GetImGuiVertexShaderSPIRV", "VertexShaderSPIRV"),
        make_getter("GetImGuiPixelShaderSPIRV", "PixelShaderSPIRV"),
    ]

    return (
        "// This file is generated by tools/generate_imgui_shader.py.\n"
        f"// Source: {rel_hlsl}\n\n"
        "#include <radray/runtime/imgui_system.h>\n\n"
        "#include <cstddef>\n"
        "#include <span>\n\n"
        "namespace radray {\n"
        "namespace {\n\n"
        + "\n".join(arrays)
        + "\n}  // namespace\n\n"
        + "\n".join(getters)
        + "\n}  // namespace radray\n"
    )


def write_if_changed(path: Path, content: str) -> None:
    old_content = path.read_text(encoding="utf-8") if path.exists() else None
    if old_content == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8", newline="\n")


def main() -> int:
    repo_root = get_repo_root()
    parser = argparse.ArgumentParser(description="Compile and embed RadRay ImGui shaders.")
    parser.add_argument("--dxc", help="Path to dxc executable.")
    parser.add_argument("--spirv-opt", help="Optional path to spirv-opt executable for SPIR-V size optimization.")
    parser.add_argument(
        "--input",
        type=Path,
        default=repo_root / "shaderlib" / "radray_imgui.hlsl",
        help="Input HLSL shader path.",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=repo_root / "modules" / "runtime" / "src" / "imgui" / "radray_imgui_shader.cpp",
        help="Generated C++ output path.",
    )
    args = parser.parse_args()

    hlsl = args.input.resolve()
    output = args.output.resolve()
    dxc = find_dxc(repo_root, args.dxc)
    spirv_opt = find_spirv_opt(args.spirv_opt)

    blobs: dict[str, bytes] = {}
    with tempfile.TemporaryDirectory(prefix="radray_imgui_shader_") as temp_dir_name:
        temp_dir = Path(temp_dir_name)
        for name, entry_point, profile, extension in SHADERS:
            blob_path = temp_dir / f"{name}.{extension}"
            run_dxc(
                dxc=dxc,
                hlsl=hlsl,
                entry_point=entry_point,
                profile=profile,
                output=blob_path,
                spirv=extension == "spv",
            )
            if extension == "spv" and spirv_opt is not None:
                optimize_spirv(spirv_opt, blob_path)
            blobs[name] = blob_path.read_bytes()

    content = generate_cpp(hlsl, blobs)
    write_if_changed(output, content)
    print(f"Generated {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
