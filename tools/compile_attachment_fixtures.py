"""Regenerate the raw shader bytecode used by attachment and sampler GPU tests."""
import argparse
from pathlib import Path
import subprocess


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dxc", type=Path, default=root / "SDKs/radray_dxc/extracted/bin/dxc.exe")
    args = parser.parse_args()
    data = root / "modules/render/tests/data"
    fixtures = (
        ("command_contracts", "VSMain", "vs_6_0"),
        ("command_contracts", "PSMain", "ps_6_0"),
        ("attachment_color", "VSMain", "vs_6_0"),
        ("attachment_color", "PSMain", "ps_6_0"),
        ("attachment_depth", "VSMain", "vs_6_0"),
        ("attachment_read_color", "CSMain", "cs_6_0"),
        ("attachment_read_depth", "CSMain", "cs_6_0"),
        ("attachment_sample_depth", "VSMain", "vs_6_0"),
        ("attachment_sample_depth", "PSMain", "ps_6_0"),
        ("attachment_sample_stencil", "PSMain", "ps_6_0"),
        ("sampler_reduction", "CSMain", "cs_6_0"),
    )
    for name, entry, profile in fixtures:
        for suffix, options in (("dxil", ["-Qstrip_rootsignature"]), ("spirv", ["-spirv", "-fspv-target-env=vulkan1.2", "-fvk-use-dx-layout"])):
            output = data / "shader_bytecode" / f"{name}.{entry}.{suffix}.bin"
            subprocess.run([str(args.dxc), "-I", str(root / "shaderlib"), "-E", entry, "-T", profile, *options, "-Fo", str(output), str(data / f"{name}.hlsl")], check=True)


if __name__ == "__main__":
    main()
