#!/usr/bin/env python3
"""Export copied CAD concurrently; check geometry, components and temporary cleanup.

Run with the actual native kicad-cli. Optional --fixture accepts a disposable
project directory (copied again); the default includes a local QA STEP component.
No canonical CAD files are modified.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile


def geometry(path):
    data = path.read_bytes()
    magic, version, size = struct.unpack_from('<III', data)
    assert (magic, version, size) == (0x46546C67, 2, len(data)), path
    offset = 12
    chunks = {}
    while offset < len(data):
        length, kind = struct.unpack_from('<II', data, offset)
        chunks[kind] = data[offset + 8:offset + 8 + length]
        offset += 8 + length
    doc = json.loads(chunks[0x4E4F534A])
    binary = chunks[0x004E4942]
    # Compare every accessor's actual bytes, including positions, normals, colours
    # and indices, without depending on buffer packing or timestamp metadata.
    accessors = []
    widths = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}
    sizes = {5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4}
    for accessor in doc['accessors']:
        view = doc['bufferViews'][accessor['bufferView']]
        start = view.get('byteOffset', 0) + accessor.get('byteOffset', 0)
        width = widths[accessor['type']] * sizes[accessor['componentType']]
        stride = view.get('byteStride', width)
        content = b''.join(binary[start + i * stride:start + i * stride + width]
                           for i in range(accessor['count']))
        accessors.append({key: accessor[key] for key in
                          ('type', 'count', 'componentType', 'normalized') if key in accessor}
                         | {'sha256': hashlib.sha256(content).hexdigest()})
    scene = {'accessors': accessors, 'meshes': doc['meshes'],
             'materials': doc.get('materials', []),
             'nodes': [{k: v for k, v in node.items() if k not in ('name', 'extras')}
                       for node in doc['nodes']], 'scenes': doc['scenes']}
    digest = hashlib.sha256(json.dumps(scene, sort_keys=True).encode()).hexdigest()
    return {'geometry': digest, 'meshes': len(doc['meshes']),
            'nodes': len(doc['nodes']), 'accessors': len(accessors)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('cli', type=Path)
    parser.add_argument('--fixture', type=Path)
    parser.add_argument('--rounds', type=int, default=4)
    args = parser.parse_args()
    cli = str(args.cli.resolve())
    source = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix='backplane-glb-race-') as temp:
        root = Path(temp)
        project = root / 'Copied 日本 project'
        if args.fixture:
            shutil.copytree(args.fixture, project)
            boards = list(project.glob('*.kicad_pcb'))
            assert len(boards) == 1, boards
            board = boards[0]
        else:
            project.mkdir()
            shutil.copy2(source / 'qa/data/pcbnew/step_model_colors/TO-252-2.step',
                         project / 'component.step')
            board = project / 'fixture.kicad_pcb'
            footprints = ''.join(f'''(footprint "Regression:local" (layer "F.Cu")
              (at {12 + i % 4 * 6} {12 + i // 4 * 6})
              (pad "1" smd rect (at 0 0) (size 1 2) (layers "F.Cu" "F.Paste" "F.Mask"))
              (model "${{KIPRJMOD}}/component.step"
                (offset (xyz 0 0 0)) (scale (xyz 1 1 1)) (rotate (xyz 0 0 0))))'''
                                 for i in range(12))
            board.write_text('''(kicad_pcb (version 20240108) (generator "pcbnew")
              (general (thickness 1.6))
              (layers (0 "F.Cu" signal) (31 "B.Cu" signal)
                      (36 "B.SilkS" user "b.silkscreen") (37 "F.SilkS" user "f.silkscreen")
                      (44 "Edge.Cuts" user))
              (gr_rect (start 5 5) (end 40 35) (stroke (width 0.05) (type default))
                       (fill none) (layer "Edge.Cuts"))
            ''' + footprints + ')\n')
        original = {p.relative_to(project): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in project.rglob('*') if p.is_file()}
        home = root / 'home'
        home.mkdir()
        env = os.environ | {'HOME': str(home), 'XDG_CONFIG_HOME': str(home / 'config'),
                            'XDG_CACHE_HOME': str(home / 'cache'),
                            'KICAD_CONFIG_HOME': str(home / 'kicad'),
                            'OMP_NUM_THREADS': '2', 'VECLIB_MAXIMUM_THREADS': '2'}
        output = root / 'Output 日本 shared'
        output.mkdir()

        def export(name, bare=False):
            target = output / (name + '.glb')
            result = subprocess.run([cli, 'pcb', 'export', 'glb',
                                     '--no-components' if bare else '--subst-models',
                                     '--include-pads', '--include-soldermask', '--include-silkscreen',
                                     '-f', '-o', str(target), str(board)],
                                    env=env, capture_output=True, text=True, timeout=120)
            assert result.returncode == 0, (name, result.returncode, result.stdout, result.stderr)
            return geometry(target)

        full, bare = export('reference-full'), export('reference-bare', True)
        assert full['meshes'] > bare['meshes'] and full['geometry'] != bare['geometry'], (full, bare)
        print('PASS component geometry differs from bare board', full, bare, flush=True)
        with ThreadPoolExecutor(max_workers=2) as pool:
            for round_number in range(args.rounds):
                results = list(pool.map(lambda item: export(*item),
                    [(f'full-{round_number}', False), (f'bare-{round_number}', True)]))
                assert results == [full, bare], (round_number, results, full, bare)
                # Also cover two consumers exporting the identical cache pathname.
                same = list(pool.map(lambda _: export(f'same-{round_number}'), range(2)))
                assert same == [full, full], (round_number, same, full)
                print('PASS concurrent distinct and identical destinations', round_number, flush=True)
        assert not list(output.glob('$tempfile$*')), list(output.iterdir())
        for name, digest in original.items():
            assert hashlib.sha256((project / name).read_bytes()).hexdigest() == digest, name
        print('GLB_CONCURRENT_GEOMETRY_COMPONENTS_PRESERVATION_PASS', flush=True)


if __name__ == '__main__':
    main()
