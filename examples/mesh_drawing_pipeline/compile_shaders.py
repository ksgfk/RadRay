"""显式生成教学样例的 DXIL/SPIR-V；不下载依赖，也不由 CMake 自动执行。"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dxc", type=Path, default=root / "SDKs/radray_dxc/extracted/bin/dxc.exe")
    parser.add_argument("--output-dir", type=Path, default=root / "build/mesh_drawing_pipeline/shaders")
    args = parser.parse_args()
    dxc = args.dxc.resolve()
    if not dxc.is_file():
        parser.error(f"DXC 不存在：{dxc}；请手动准备 SDK 或指定 --dxc")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).resolve().parent / "cube.hlsl"
    # 所有 stage 成功后才替换输出，避免把半次编译结果混入现有样例。
    with tempfile.TemporaryDirectory(dir=args.output_dir) as temporary:
        staged = Path(temporary)
        for entry, profile in (("VSMain", "vs_6_0"), ("PSMain", "ps_6_0")):
            for target in ("dxil", "spirv"):
                options = (["-Qstrip_rootsignature"] if target == "dxil" else
                           ["-spirv", "-fspv-target-env=vulkan1.2", "-fvk-use-dx-layout"])
                output = staged / f"cube.{entry}.{target}.bin"
                command = [str(dxc), "-I", str(root / "shaderlib"), "-E", entry, "-T", profile,
                           *options, "-Fo", str(output), str(source)]
                result = subprocess.run(command, check=False)
                if result.returncode != 0:
                    parser.error(f"{entry}/{target} 编译失败；已有输出未替换")
        for output in staged.iterdir():
            output.replace(args.output_dir / output.name)
    print(f"样例 shader 已生成：{args.output_dir.resolve()}")


if __name__ == "__main__":
    main()
