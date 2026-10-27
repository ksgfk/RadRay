"""Regenerate the raw shader bytecode used by attachment and sampler GPU tests."""
import argparse
from pathlib import Path
import subprocess


def compile_fixtures(root, dxc, output_dir, environment=None):
    output_dir.mkdir(parents=True, exist_ok=True)
    data = root / "modules/render/tests/data"
    fixtures = (
        ("interop_texture_depth", "VSMain", "vs_6_0"),
        ("interop_texture_depth", "PSMain", "ps_6_0"),
        ("interop_sample", "CSMain", "cs_6_0"),
        ("interop_storage_rgba8", "CSMain", "cs_6_0"),
        ("interop_storage_bgra8", "CSMain", "cs_6_0"),
        ("interop_storage_rgba16", "CSMain", "cs_6_0"),
        ("interop_storage_r32", "CSMain", "cs_6_0"),
        ("interop_color", "VSMain", "vs_6_0"),
        ("interop_color", "PSMain", "ps_6_0"),
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
    variants = [(name, name, entry, profile, []) for name, entry, profile in fixtures]
    for kind, dims in (("read", (1, 2, 3, 4, 5, 6)), ("write", (1, 2, 3))):
        for dim in dims:
            for value in (("float",) if dim in (4, 5) else ("float", "uint", "int")):
                source = f"interop_texture_{kind}"
                variants.append((f"{source}_{dim}_{value}", source, "CSMain", "cs_6_0", [f"-DDIMENSION={dim}", f"-DVALUE_TYPE={value}4"]))
    for value in ("float", "uint", "int"):
        for entry, profile in (("VSMain", "vs_6_0"), ("PSMain", "ps_6_0")):
            variants.append((f"interop_texture_color_{value}", "interop_texture_color", entry, profile, [f"-DVALUE_TYPE={value}4"]))
    for name, source, entry, profile, defines in variants:
        for suffix, options in (("dxil", ["-Qstrip_rootsignature"]), ("spirv", ["-spirv", "-fspv-target-env=vulkan1.2", "-fvk-use-dx-layout"])):
            if (name == "interop_storage_bgra8" or source == "interop_texture_write") and suffix == "spirv":
                options.append("-fspv-use-unknown-image-format")
            output = output_dir / f"{name}.{entry}.{suffix}.bin"
            result = subprocess.run([str(dxc), "-I", str(root / "shaderlib"), "-E", entry, "-T", profile, *options, *defines, "-Fo", str(output), str(data / f"{source}.hlsl")], env=environment)
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
