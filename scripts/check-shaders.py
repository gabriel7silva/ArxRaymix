#!/usr/bin/env python3
"""Compile the HLSL embedded in the D3D12 / DXR sources, so a shader error fails CI instead of
turning ray tracing off at run time ("RTAO: lib_6_3 compile failed" in arx.log).

Each shader is a C++ raw string literal (possibly several adjacent ones) in a .cpp file. This
script pulls them out and runs the same compilers and profiles the game uses: DXC lib_6_3 for
the ray tracing library, FXC vs_5_0 / ps_5_0 for the raster passes.

Needs dxc.exe and fxc.exe, from the Windows SDK (found automatically) or passed with --dxc /
--fxc.

    python scripts/check-shaders.py
"""

import argparse
import glob
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (file, literal, compiler, [(profile, entry[, defines]), ...]). Entry None means a library.
SHADERS = [
    ('arx/src/graphics/dxr/D3D12Rtao.cpp', 'kRayLib', 'dxc',
     [('lib_6_3', None), ('cs_6_0', 'CSMain', ['ARX_SOFTWARE_RT=1'])]),
    ('arx/src/graphics/dxr/D3D12Rtao.cpp', 'kComposite', 'fxc',
     [('vs_5_0', 'VSMain'), ('ps_5_0', 'PSMain')]),
    ('arx/src/graphics/dxr/D3D12Rtao.cpp', 'kMask', 'fxc',
     [('vs_5_0', 'VSMain'), ('ps_5_0', 'PSWater'), ('ps_5_0', 'PSWaterZ'), ('ps_5_0', 'PSMetal')]),
    ('arx/src/graphics/d3d12/D3D12Renderer.cpp', 'kShader', 'fxc',
     [('vs_5_0', 'VSMain'), ('ps_5_0', 'PSMain')]),
]


def extract(path, name):
    """Text of `const char * name = R"(...)" R"(...)";`, with the pieces concatenated."""
    with open(os.path.join(ROOT, path), encoding='utf-8') as f:
        source = f.read()
    start = re.search(r'const char \* ' + re.escape(name) + r' = ', source)
    if not start:
        sys.exit('%s: no literal %s' % (path, name))
    pos = start.end()
    parts = []
    while True:
        m = re.compile(r'\s*R"\((.*?)\)"', re.S).match(source, pos)
        if not m:
            break
        parts.append(m.group(1))
        pos = m.end()
    if not parts or not source.startswith(';', pos):
        sys.exit('%s: could not parse %s' % (path, name))
    return ''.join(parts)


def find_sdk_tool(exe):
    kits = os.path.join(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'),
                        'Windows Kits', '10', 'bin')
    found = sorted(glob.glob(os.path.join(kits, '10.*', 'x64', exe)))
    return found[-1] if found else None


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--dxc', help='path to dxc.exe')
    parser.add_argument('--fxc', help='path to fxc.exe')
    args = parser.parse_args()
    tools = {
        'dxc': args.dxc or find_sdk_tool('dxc.exe'),
        'fxc': args.fxc or find_sdk_tool('fxc.exe'),
    }
    for name, tool in tools.items():
        if not tool:
            sys.exit('%s.exe not found; install the Windows SDK or pass --%s' % (name, name))

    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for path, literal, compiler, targets in SHADERS:
            hlsl = os.path.join(tmp, literal + '.hlsl')
            with open(hlsl, 'w', encoding='utf-8') as f:
                f.write(extract(path, literal))
            for target in targets:
                profile, entry = target[0], target[1]
                defines = target[2] if len(target) > 2 else []
                out = os.path.join(tmp, '%s_%s.bin' % (literal, entry or profile))
                cmd = [tools[compiler], '-T', profile, '-Fo', out]
                if compiler == 'fxc':
                    cmd.append('-nologo')
                if entry:
                    cmd += ['-E', entry]
                for define in defines:
                    cmd += ['-D', define]
                cmd.append(hlsl)
                result = subprocess.run(cmd, capture_output=True, text=True)
                label = '%s %s %s' % (literal, profile, entry or '')
                if result.returncode != 0:
                    failed += 1
                    print('FAIL ' + label)
                    print(result.stdout + result.stderr)
                else:
                    print('ok   ' + label)
    if failed:
        sys.exit('%d shader compile(s) failed' % failed)


if __name__ == '__main__':
    main()
