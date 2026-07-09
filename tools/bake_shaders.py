#!/usr/bin/env python3
"""shader 变体离线烘焙工具。

读取变体集合 (assets/shader_variants/*.json), 调用 radray_shader_baker (C++ helper)
把每个声明的变体编译为 dxil 和/或 spirv, 按运行时约定的目录树写出字节码 + 反射 sidecar,
供 PrecompiledShaderVariantCache (DXC 缺失路径) 加载。

磁盘布局:
    <bakeRoot>/manifest.json
    <bakeRoot>/<Program.Name>/pass<N>_mask<HEX16>/<target>/{vs,ps,cs}.{dxil|spv}
                                                  <target>/{vs,ps,cs}.refl.json

反射数据只能在 C++ 侧提取 (依赖 ID3D12ShaderReflection / SPIRV-Cross), 故本脚本仅做编排,
真正的编译 + 反射由 radray_shader_baker 完成。

用法:
    python tools/bake_shaders.py --collection assets/shader_variants/forward_pipeline.json \\
        --out build_debug/_build/Debug/shadercache --baker <path/to/radray_shader_baker.exe> \\
        --target all
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True

# stage 名 -> 文件名前缀 (与 PrecompiledShaderVariantCacheImpl::StagePrefix 一致)。
STAGE_PREFIX = {
    "vertex": "vs",
    "pixel": "ps",
    "compute": "cs",
}

# target -> 字节码扩展名 (与 PrecompiledShaderVariantCacheImpl 的 _blobExt 一致)。
TARGET_EXT = {
    "dxil": ".dxil",
    "spirv": ".spv",
}


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def project_bitmask(keywords: list[str], enabled: list[str]) -> int:
    """把启用的 keyword 名字投影为 bitmask (bit 序 = 在 keywords 表中的下标)。

    与 ShaderKeywordSet::Project 一致: 未声明的 keyword 被忽略。
    """
    index = {name: i for i, name in enumerate(keywords)}
    mask = 0
    for name in enabled:
        bit = index.get(name)
        if bit is None:
            print(f"warning: keyword '{name}' not declared in collection Keywords; ignored", file=sys.stderr)
            continue
        mask |= (1 << bit)
    return mask


def resolve_defines(keywords: list[str], mask: int) -> list[str]:
    """把 bitmask 解析为 -D 宏 token (形如 'NAME=1'), 与 ShaderKeywordSet::ResolveDefines 一致。"""
    defines = []
    for i, name in enumerate(keywords):
        if mask & (1 << i):
            defines.append(f"{name}=1")
    return defines


def variant_dirname(pass_index: int, mask: int) -> str:
    # pass<N>_mask<16位大写HEX>, 与 PrecompiledShaderVariantCacheImpl::MakeVariantDir 一致。
    return f"pass{pass_index}_mask{mask:016X}"


def run_baker(
    baker: Path,
    source: Path,
    entry: str,
    stage: str,
    target: str,
    sm: str,
    out_blob: Path,
    out_refl: Path,
    defines: list[str],
    includes: list[Path],
    optimize: bool,
) -> bool:
    cmd = [
        str(baker),
        "--source", str(source),
        "--entry", entry,
        "--stage", stage,
        "--target", target,
        "--sm", sm,
        "--out-blob", str(out_blob),
        "--out-refl", str(out_refl),
    ]
    for d in defines:
        cmd += ["--define", d]
    for inc in includes:
        cmd += ["--include", str(inc)]
    if optimize:
        cmd += ["--optimize"]
    result = subprocess.run(cmd)
    return result.returncode == 0


def bake(
    collection_path: Path,
    out_root: Path,
    baker: Path,
    targets: list[str],
    optimize: bool,
) -> int:
    root = repo_root()
    with collection_path.open("r", encoding="utf-8") as f:
        collection = json.load(f)

    keywords = collection.get("Keywords", [])
    sm = collection.get("ShaderModel", "SM60")
    include_dirs = [root / d for d in collection.get("IncludeDirs", [])]
    programs = collection.get("Programs", [])

    baked_programs = []
    total = 0
    failed = 0

    for prog in programs:
        name = prog["Name"]
        source = root / prog["Source"]
        pass_index = int(prog.get("PassIndex", 0))
        stages = prog.get("Stages", [])
        variants = prog.get("Variants", [])

        if not source.is_file():
            print(f"error: source not found: {source}", file=sys.stderr)
            failed += 1
            continue

        baked_variants = []
        for enabled in variants:
            mask = project_bitmask(keywords, enabled)
            defines = resolve_defines(keywords, mask)
            vdir_name = variant_dirname(pass_index, mask)

            for target in targets:
                ext = TARGET_EXT[target]
                target_dir = out_root / name / vdir_name / target
                target_dir.mkdir(parents=True, exist_ok=True)

                for stage_desc in stages:
                    stage = stage_desc["Stage"]
                    entry = stage_desc["Entry"]
                    prefix = STAGE_PREFIX.get(stage)
                    if prefix is None:
                        print(f"error: unsupported stage '{stage}' in program '{name}'", file=sys.stderr)
                        failed += 1
                        continue
                    out_blob = target_dir / f"{prefix}{ext}"
                    out_refl = target_dir / f"{prefix}.refl.json"
                    total += 1
                    ok = run_baker(
                        baker, source, entry, stage, target, sm,
                        out_blob, out_refl, defines, include_dirs, optimize,
                    )
                    if not ok:
                        print(f"error: bake failed: {name} {vdir_name} {target} {stage}", file=sys.stderr)
                        failed += 1

            baked_variants.append({
                "PassIndex": pass_index,
                "Enabled": enabled,
                "Bitmask": f"{mask:016X}",
            })

        baked_programs.append({
            "Name": name,
            "PassIndex": pass_index,
            "Keywords": keywords,
            "Variants": baked_variants,
        })

    # 顶层 manifest: 记录格式版本 + 已产出 target + program 索引。
    manifest = {
        "FormatVersion": 1,
        "Targets": targets,
        "ShaderModel": sm,
        "Programs": baked_programs,
    }
    out_root.mkdir(parents=True, exist_ok=True)
    with (out_root / "manifest.json").open("w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=4, ensure_ascii=False)

    print(f"bake done: {total - failed}/{total} stage-compiles ok, out={out_root}")
    return 1 if failed else 0


def main() -> int:
    root = repo_root()
    parser = argparse.ArgumentParser(description="Bake RadRay shader variants to disk.")
    parser.add_argument(
        "--collection",
        type=Path,
        default=root / "assets" / "shader_variants" / "forward_pipeline.json",
        help="Path to the shader variant collection JSON.",
    )
    parser.add_argument(
        "--out",
        type=Path,
        required=True,
        help="Output bake root directory (runtime loads from <exe>/shadercache).",
    )
    parser.add_argument(
        "--baker",
        type=Path,
        required=True,
        help="Path to the radray_shader_baker executable.",
    )
    parser.add_argument(
        "--target",
        choices=["dxil", "spirv", "all"],
        default="all",
        help="Which target(s) to bake.",
    )
    parser.add_argument(
        "--optimize",
        action="store_true",
        help="Compile with optimizations (-O3).",
    )
    args = parser.parse_args()

    if not args.collection.is_file():
        print(f"error: collection not found: {args.collection}", file=sys.stderr)
        return 2
    if not args.baker.is_file():
        print(f"error: baker executable not found: {args.baker}", file=sys.stderr)
        return 2

    targets = ["dxil", "spirv"] if args.target == "all" else [args.target]
    return bake(args.collection, args.out, args.baker, targets, args.optimize)


if __name__ == "__main__":
    raise SystemExit(main())
