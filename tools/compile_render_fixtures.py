"""Compile the render test shaders in modules/render/tests/data to raw DXIL and SPIR-V.

Each entry point becomes shader_bytecode/<source>.<entry>.<dxil|spirv>.bin. The render
tests read only these files, so a test process never loads DXC.
"""
import argparse
from pathlib import Path
import subprocess

FIXTURES = (
    ("compute", "TransformInput", "cs_6_0"),
    ("compute", "ReadConstants", "cs_6_0"),
    ("compute", "ReadPushConstants", "cs_6_0"),
    ("compute", "SampleImage", "cs_6_0"),
    ("compute", "CombineGroups", "cs_6_0"),
    ("graphics", "FullscreenVS", "vs_6_0"),
    ("graphics", "SolidPS", "ps_6_0"),
    ("geometry", "VSMain", "vs_6_0"),
)

TARGETS = (
    ("dxil", ["-Qstrip_rootsignature"]),
    ("spirv", ["-spirv", "-fspv-target-env=vulkan1.2", "-fvk-use-dx-layout"]),
)


def compile_fixtures(root, dxc, output_dir, environment=None):
    output_dir.mkdir(parents=True, exist_ok=True)
    data = root / "modules/render/tests/data"
    for source, entry, profile in FIXTURES:
        for suffix, options in TARGETS:
            output = output_dir / f"{source}.{entry}.{suffix}.bin"
            result = subprocess.run([str(dxc), "-I", str(root / "shaderlib"), "-E", entry, "-T", profile,
                                     *options, "-Fo", str(output), str(data / f"{source}.hlsl")], env=environment)
            if result.returncode:
                return False
    return True


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dxc", type=Path, default=root / "SDKs/radray_dxc/extracted/bin/dxc.exe")
    parser.add_argument("--output-dir", type=Path, default=root / "build/test_shaders/shader_bytecode")
    args = parser.parse_args()
    if not args.dxc.is_file():
        parser.error(f"DXC not found: {args.dxc}; restore the SDK manually or pass --dxc")
    if not compile_fixtures(root, args.dxc.resolve(), args.output_dir.resolve()):
        parser.error("shader compilation failed; see DXC diagnostics above")
    print("Raw render fixtures updated. Run tools/compile_test_shaders.py to prepare and verify the complete bundle.")


if __name__ == "__main__":
    main()
