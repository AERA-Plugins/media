#!/usr/bin/env python3
"""Create AERA's bounded deterministic XZ runtime payload."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile

def digest_file(source):
    digest = hashlib.sha256()
    while block := source.read(1024 * 1024):
        digest.update(block)
    return digest.hexdigest()

def safe_path(path):
    return path and len(path.encode()) < 240 and all(
        part not in ('', '.', '..') for part in path.split('/')) and not path.startswith('/')

def pack(root, output):
    output.mkdir(parents=True, exist_ok=True)
    files = sorted(p for p in root.rglob('*') if p.is_file() or p.is_symlink())
    if not files or len(files) > 4096:
        raise ValueError('Invalid file count')
    inventory = []
    with tempfile.TemporaryFile() as expanded:
        # Host API 1 uses this established container signature for all runtime plugins.
        expanded.write(b'AERAWEB1' + struct.pack('<I', len(files)))
        for path in files:
            name = path.relative_to(root).as_posix()
            if not safe_path(name):
                raise ValueError(f'Unsafe path: {name}')
            link = path.is_symlink()
            target = path.resolve(strict=True)
            if link and not target.is_relative_to(root.resolve()):
                raise ValueError(f'Escaping symlink: {name}')
            if link:
                data = target.relative_to(root.resolve()).as_posix().encode()
                mode = 0
            else:
                data = path.read_bytes()
                mode = 0o755 if path.stat().st_mode & 0o111 else 0o644
            if len(data) > 100 * 1024 * 1024:
                raise ValueError(f'Oversize member: {name}')
            encoded = name.encode()
            expanded.write(struct.pack('<HHQ', len(encoded), mode, len(data)))
            expanded.write(encoded)
            expanded.write(b'\0' * (-expanded.tell() % 4))
            expanded.write(data)
            inventory.append(dict(path=name, mode=mode, size=len(data),
                                  sha256=hashlib.sha256(data).hexdigest()))
        length = expanded.tell()
        if length > 512 * 1024 * 1024:
            raise ValueError('Runtime exceeds AERA Host API 1 cap')
        expanded.seek(0)
        expanded_hash = digest_file(expanded)
        expanded.seek(0)
        with (output / 'runtime.xz').open('wb') as compressed:
            subprocess.run(['xz', '-c', '--threads=1', '--check=crc32', '--arm64',
                            '--lzma2=preset=9e,lc=2,lp=2'], stdin=expanded,
                           stdout=compressed, check=True)
    payload = output / 'runtime.xz'
    with payload.open('rb') as source:
        compressed_hash = digest_file(source)
    metadata = dict(compressed=payload.stat().st_size, expanded=length,
                    sha256=compressed_hash, expanded_sha256=expanded_hash,
                    files=len(files))
    (output / 'manifest.json').write_text(json.dumps(inventory, indent=2) + '\n')
    (output / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(json.dumps(metadata, indent=2))

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('output', type=Path)
    arguments = parser.parse_args()
    pack(arguments.root.resolve(strict=True), arguments.output)
