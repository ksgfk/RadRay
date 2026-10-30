"""Explicitly prepare raw runtime test bytecode; configuration never invokes DXC."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--output-dir', type=Path, default=root / 'build/runtime-test-shaders')
    parser.add_argument('--dxc', type=Path, default=root / 'SDKs/radray_dxc/extracted/bin/dxc.exe')
    args = parser.parse_args()
    sources = sorted((root / 'modules/runtime/tests/data').glob('*.hlsl'))
    sources += sorted((root / 'shaderlib/core').glob('*.hlsli')) + [Path(__file__)]
    identity = hashlib.sha256(''.join(p.read_text(encoding='utf-8') for p in sources).encode()).hexdigest()
    output = args.output_dir.resolve()
    stamp = output / 'manifest.json'
    if args.check:
        if not stamp.is_file():
            parser.error('Generate fixtures with python tools/compile_runtime_test_shaders.py first')
        record = json.loads(stamp.read_text(encoding='utf-8'))
        if record['sources'] != identity or any(not (output / name).is_file() or hashlib.sha256((output / name).read_bytes()).hexdigest() != digest for name, digest in record['files'].items()):
            parser.error('Runtime shader fixtures are stale; regenerate them explicitly')
        print('Runtime shader fixtures verified')
        return
    output.mkdir(parents=True, exist_ok=True)
    files = {}
    for name, stages in [('graph_compute', ['CS']), ('graph_sample', ['VS', 'PS'])]:
        for stage in stages:
            for backend, flags in [('dxil', ['-Qstrip_rootsignature']), ('spirv', ['-spirv', '-fspv-target-env=vulkan1.2', '-fvk-use-dx-layout'])]:
                path = output / f'{name}.{stage}Main.{backend}.bin'
                subprocess.run([str(args.dxc), '-I', str(root / 'shaderlib'), '-E', stage + 'Main', '-T', stage.lower() + '_6_0', '-O3', *flags,
                                '-Fo', str(path), str(root / f'modules/runtime/tests/data/{name}.hlsl')], check=True)
                files[path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
    stamp.write_text(json.dumps({'sources': identity, 'files': files}, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
