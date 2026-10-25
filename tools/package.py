#!/usr/bin/env python3
"""Package the complete VST3 bundle, fixtures, tests and provenance; never a bare DLL."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile

parser = argparse.ArgumentParser()
parser.add_argument('--x64', type=Path, required=True)
parser.add_argument('--x86', type=Path, required=True)
parser.add_argument('--out', type=Path, default=Path('artifacts'))
args = parser.parse_args()
repo = Path(__file__).resolve().parents[1]
args.out.mkdir(parents=True, exist_ok=True)
name = 'PluginScaler-0.1.0-prototype-windows'
stage = args.out / name
if stage.exists():
    raise SystemExit(f'Refusing to overwrite existing package: {stage}')
binary = stage / 'PluginScaler.vst3' / 'Contents' / 'x86_64-win'
binary.mkdir(parents=True)
for source, target in [
    (args.x64 / 'PluginScaler.vst3', 'PluginScaler.vst3'),
    (args.x64 / 'scaler-helper64.exe', 'scaler-helper64.exe'),
    (args.x86 / 'scaler-helper32.exe', 'scaler-helper32.exe'),
    (args.x64 / 'scaler-fixture.dll', 'scaler-fixture.dll'),
    (args.x86 / 'scaler-fixture.dll', 'scaler-fixture32.dll'),
    (args.x64 / 'fixture-vst3.vst3', 'fixture-vst3.vst3'),
    (args.x64 / 'core-tests.exe', 'core-tests.exe'),
    (args.x64 / 'integration-tests.exe', 'integration-tests.exe'),
    (args.x64 / 'adapter-tests.exe', 'adapter-tests.exe'),
]:
    if not source.is_file() or source.read_bytes()[:2] != b'MZ':
        raise SystemExit(f'Missing/invalid Windows binary: {source}')
    shutil.copy2(source, binary / target)
(binary / 'PluginScaler.ini').write_text('[plugin]\npath=scaler-fixture.dll\n', encoding='ascii')
for document in ['README.md', 'START-HERE.md']:
    shutil.copy2(repo / document, stage / document)
shutil.copytree(repo / 'docs', stage / 'docs')
shutil.copytree(repo / 'licenses', stage / 'licenses')
evidence = stage / 'evidence'
evidence.mkdir()
for folder in [args.x64, args.x86]:
    for pattern in ['ctest-*-results.txt', 'integration-x86-results.txt', 'editor-*.bmp']:
        for file in folder.glob(pattern):
            shutil.copy2(file, evidence / file.name)
manifest = {
    'version': '0.1.0-prototype',
    'source_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=repo, text=True).strip(),
    'source_branch': subprocess.check_output(['git', 'branch', '--show-current'], cwd=repo, text=True).strip(),
    'source_dirty': bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=repo, text=True).strip()),
    'vst3_interfaces_commit': '4f547e8e102b47de4a8b8aaf343c73b700786372',
    'qualification': 'Experimental prototype. Read docs/VALIDATION.md for actual tested environments.',
    'sha256': {},
}
for file in sorted(stage.rglob('*')):
    if file.is_file():
        manifest['sha256'][file.relative_to(stage).as_posix()] = hashlib.sha256(file.read_bytes()).hexdigest()
(stage / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
archive = args.out / f'{name}.zip'
with zipfile.ZipFile(archive, 'x', compression=zipfile.ZIP_DEFLATED) as package:
    for file in sorted(stage.rglob('*')):
        if file.is_file():
            package.write(file, file.relative_to(args.out))
print(f'{archive}: {archive.stat().st_size} bytes; SHA256 {hashlib.sha256(archive.read_bytes()).hexdigest()}')
