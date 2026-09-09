#!/usr/bin/env python3
"""Capture a packaged plugin in a new, isolated data directory."""
import argparse
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def manifest(path):
    if path.is_file():
        return {path.name: sha(path)}
    return {str(p.relative_to(path)): sha(p)
            for p in sorted(path.rglob('*')) if p.is_file() and '__pycache__' not in p.parts}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path, help='new case directory; never reused')
    parser.add_argument('--host', type=Path, required=True)
    parser.add_argument('--plugin', type=Path, required=True)
    parser.add_argument('--model', choices=['MD', 'MM'], required=True)
    parser.add_argument('--rom', type=Path, required=True)
    parser.add_argument('--seed', type=Path, help='optional product folder containing config/ and nvram/')
    parser.add_argument('--patch-ram', type=Path, help='MM transport fixture produced by mdLatencyFixture')
    parser.add_argument('--scenario', choices=['notes', 'chords', 'input', 'transport'], default='notes')
    parser.add_argument('--rate', type=int, default=48000)
    parser.add_argument('--block', type=int, default=128)
    parser.add_argument('--phase', type=int, default=0)
    parser.add_argument('--seconds', type=float, default=20)
    parser.add_argument('--latency-blocks', type=int, default=0)
    parser.add_argument('--variable', action='store_true')
    parser.add_argument('--offline', action='store_true')
    parser.add_argument('--suppress-message-loop', action='store_true', help='diagnostic comparison only')
    parser.add_argument('--reprepare', type=float, default=-1)
    parser.add_argument('--restore', type=float, default=-1)
    args = parser.parse_args()
    if not (8000 <= args.rate <= 192000 and 1 <= args.block <= 8192
            and 0 <= args.phase < args.block and 20 <= args.seconds <= 600
            and args.latency_blocks in (0, 1, 2, 4, 8)):
        parser.error('invalid rate, block, phase, duration or latency setting')
    if args.patch_ram and args.model != 'MM':
        parser.error('--patch-ram requires MM')
    if args.scenario == 'transport' and not args.patch_ram:
        parser.error('transport requires a prepared --patch-ram fixture (see README)')
    for name in ('host', 'plugin', 'rom', 'seed', 'patch_ram'):
        path = getattr(args, name)
        if path is not None:
            path = path.resolve(strict=True)
            setattr(args, name, path)
    case = args.output.resolve()
    case.mkdir(parents=True, exist_ok=False)
    model = {'MD': 'Machinedrum', 'MM': 'Monomachine'}[args.model]
    data = case / 'data' / 'Gearmulator Preview' / model
    for folder in ('config', 'nvram'):
        target = data / folder
        if args.seed and (args.seed / folder).is_dir():
            shutil.copytree(args.seed / folder, target)
        else:
            target.mkdir(parents=True)
    (data / 'roms').mkdir()
    # Copies (not hard links) keep all writable test state independent of the seed.
    shutil.copyfile(args.rom, data / 'roms' / args.rom.name)
    if args.patch_ram:
        shutil.copyfile(args.patch_ram, data / 'nvram' / 'mm-factory-live3-be.bin')
    config = data / 'config' / f'Gearmulator {args.model}.xml'
    tree = ET.parse(config) if config.exists() else ET.ElementTree(ET.Element('PROPERTIES'))
    settings = {'enableMcpServer': 0, 'latencyBlocks': args.latency_blocks}
    for name, value in settings.items():
        for element in tree.getroot().findall(f"VALUE[@name='{name}']"):
            tree.getroot().remove(element)
        ET.SubElement(tree.getroot(), 'VALUE', name=name, val=str(value))
    tree.write(config, encoding='utf-8', xml_declaration=True)
    command = [str(args.host), str(args.plugin), str(case / 'capture'),
               str(args.rate), str(args.block), str(args.seconds), str(args.reprepare),
               'variable' if args.variable else 'fixed', '0' if args.offline else '-1', 'fast',
               '36' if args.model == 'MD' else '60', str(args.phase), args.scenario,
               'no-messages' if args.suppress_message_loop else 'messages', str(args.restore)]
    # Keep paths in this private receipt; publish only the explicitly sanitized summary.
    receipt = {'options': vars(args), 'command': command, 'host_sha256': sha(args.host),
               'plugin_sha256': manifest(args.plugin), 'initial_data_sha256': manifest(data),
               'settings': settings, 'tools_sha256': {name: sha(Path(__file__).with_name(name))
                   for name in ('CMakeLists.txt', 'README.md', 'latency_host.cpp', 'run.py',
                                'analyze.py', 'test_analysis.py', 'create_transport_fixture.cpp', 'fixture_helpers.h')},
               'analysis_runtime': {name: importlib.metadata.version(name) for name in ('numpy', 'scipy')},
               'rom_sha256': sha(args.rom),
               'seed_sha256': manifest(args.seed) if args.seed else None}
    receipt_path = case / 'run.json'
    receipt_path.write_text(json.dumps(receipt, indent=2, default=str) + '\n')
    env = {k: v for k, v in os.environ.items() if not k.startswith('GEARMULATOR_')}
    env['GEARMULATOR_DATA_ROOT'] = str(case / 'data')
    with (case / 'host.log').open('w') as log:
        result = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                                timeout=max(180, args.seconds * 4))
    receipt['returncode'] = result.returncode
    receipt['capture_sha256'] = {p.name: sha(p) for p in case.glob('capture.*') if p.is_file()}
    receipt['inputs_unchanged'] = (manifest(args.plugin) == receipt['plugin_sha256']
                                   and sha(args.host) == receipt['host_sha256']
                                   and sha(args.rom) == receipt['rom_sha256']
                                   and (not args.seed or manifest(args.seed) == receipt['seed_sha256']))
    receipt_path.write_text(json.dumps(receipt, indent=2, default=str) + '\n')
    if not receipt['inputs_unchanged']:
        raise RuntimeError('a measured input changed during capture')
    if result.returncode:
        raise RuntimeError(f'host failed ({result.returncode}); see {case / "host.log"}')
    subprocess.run([sys.executable, str(Path(__file__).with_name('analyze.py')), str(case)], check=True)


if __name__ == '__main__':
    main()
