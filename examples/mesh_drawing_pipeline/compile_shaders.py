"""显式运行编译和反射 CLI，生成样例的独立布局与裸字节码。"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools-dir", type=Path, default=root / "build/windows-x64-msvc/Debug/_build/Debug")
    parser.add_argument("--output-dir", type=Path, default=root / "build/mesh_drawing_pipeline/shaders")
    args = parser.parse_args()
    suffix = ".exe" if os.name == "nt" else ""
    compiler = args.tools_dir.resolve() / ("radray_shader_compile" + suffix)
    reflector = args.tools_dir.resolve() / ("radray_shader_reflect" + suffix)
    for tool in (compiler, reflector):
        if not tool.is_file():
            parser.error(f"工具不存在：{tool}；先构建两个 CLI 或指定 --tools-dir")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.output_dir) as temporary:
        staged = Path(temporary).resolve()
        manifest = staged / "cube.manifest.json"
        command = [str(compiler), "--shader-root", str(Path(__file__).resolve().parent),
                   "--source", "cube.hlsl", "--include-path", str(root / "shaderlib"),
                   "--output", str(staged / "cube"), "--manifest", str(manifest)]
        if subprocess.run(command, check=False).returncode:
            parser.error("编译失败；已有输出未替换")
        data = json.loads(manifest.read_text(encoding="utf-8"))
        data["policy"] = {"bindings": [{"name": "Pass", "groupSlot": 0, "bindingId": 0}],
                          "pushConstants": [{"name": "MaterialPush", "pushId": 0}]}
        manifest.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        if subprocess.run([str(reflector), "--manifest", str(manifest), "--output", str(staged / "cube")], check=False).returncode:
            parser.error("反射失败；已有输出未替换")
        for output in staged.iterdir():
            output.replace(args.output_dir / output.name)
    print(f"样例布局及 shader 已生成：{args.output_dir.resolve()}")


if __name__ == "__main__":
    main()
