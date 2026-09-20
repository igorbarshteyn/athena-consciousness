#!/usr/bin/env python3
"""Collect a private, read-only supplement; never execute Athena or read memory content."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

FLAGS = ['ATHENA_R26', 'ATHENA_R26_ACCESS', 'ATHENA_R26_META', 'ATHENA_R26_MEMORY',
         'ATHENA_R26_DIALOGUE', 'ATHENA_R26_AFFECT', 'ATHENA_R26_PRIVATE',
         'ATHENA_R26_TIMING', 'ATHENA_R26_SPEECH', 'ATHENA_INITIATIVE_POLICY',
         'ATHENA_DREAM_AFTER_S', 'ATHENA_EMOTION_CPU', 'ATHENA_MIND_DEBUG']

def digest(path):
    h = hashlib.sha256()
    before = path.stat()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b''):
            h.update(block)
    after = path.stat()
    if (before.st_size, before.st_mtime_ns, before.st_ino) != (after.st_size, after.st_mtime_ns, after.st_ino):
        raise ValueError('File changed during hashing: ' + str(path))
    return h.hexdigest()

def git_info(path):
    if not (path / '.git').exists():
        return {'status': 'no-git-checkout'}
    env = dict(os.environ, GIT_OPTIONAL_LOCKS='0')
    try:
        head = subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True, stderr=subprocess.DEVNULL, env=env).strip()
        dirty = bool(subprocess.check_output(['git', '-C', str(path), 'status', '--porcelain', '--untracked-files=no'], text=True, stderr=subprocess.DEVNULL, env=env).strip())
        return {'commit': head, 'tracked_changes': dirty}
    except (OSError, subprocess.CalledProcessError):
        return {'status': 'git-inspection-failed'}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--athena', required=True, type=Path)
    parser.add_argument('--overlay', type=Path)
    parser.add_argument('--output', required=True, type=Path, help='Private JSON destination outside both checkouts; new file only.')
    parser.add_argument('--hash-models', action='store_true', help='Read every listed model file to calculate SHA-256; potentially about 179 GB for Qwen alone.')
    parser.add_argument('--model', action='append', default=[], metavar='COMPONENT=PATH', help='Replace all reference paths for one runtime model role with this explicit path; repeat for split shards.')
    args = parser.parse_args()
    root = args.athena.resolve()
    output = args.output.resolve()
    if not root.is_dir():
        raise ValueError('--athena is not a directory')
    if output.is_relative_to(root) or (args.overlay and output.is_relative_to(args.overlay.resolve())):
        raise ValueError('Choose a private output path outside the source checkouts.')
    manifest = json.loads((Path(__file__).resolve().parents[1] / 'model-artifacts.json').read_text())
    overrides = {}
    allowed = {r['component_ref'] for r in manifest['artifacts']}
    for value in args.model:
        component, path = value.split('=', 1)
        if component not in allowed:
            raise ValueError('Unknown runtime model role: ' + component)
        p = Path(path).expanduser()
        overrides.setdefault(component, []).append(p.resolve() if p.is_absolute() else (root / p).resolve())
    candidates = [(r['component_ref'], root / r['local_reference_path'], r.get('registry_reported_sha256'))
                  for r in manifest['artifacts'] if r['component_ref'] not in overrides]
    candidates += [(component, p, None) for component, paths in overrides.items() for p in paths]
    records = []
    for component, path, reference in candidates:
        r = {'component_ref': component, 'path': str(path), 'present': path.is_file(),
             'sha256': None, 'identity_status': 'explicit override' if component in overrides else 'reference path only; active use not established'}
        if path.is_file():
            r['size_bytes'] = path.stat().st_size
            if args.hash_models:
                print('Hashing ' + path.name, file=sys.stderr)
                r['sha256'] = digest(path)
                r['matches_recorded_registry_reference'] = (r['sha256'] == reference) if reference else None
                r['match_note'] = 'A mismatch may indicate a legitimate different revision; it is not by itself proof of compromise.'
        records.append(r)
    binaries = []
    for component, rel in [('C-brain', 'whisper.cpp/build/bin/whisper-talk-llama'),
                           ('C-speech', 'orpheus/build/orpheus-speak'),
                           ('C-calibrate', 'whisper.cpp/build/bin/athena-emotion-calibrate'),
                           ('S-llama-server', 'llama.cpp/build/bin/llama-server')]:
        path = root / rel
        binaries.append({'component_ref': component, 'path': rel, 'present': path.is_file(),
                         'sha256': digest(path) if path.is_file() else None})
    result = {'format': 'Athena private deployment supplement 1.0', 'collected_at': datetime.now(timezone.utc).isoformat(),
              'scope': 'Explicit reference/override file metadata and shell allowlist only; neither active process inspection nor proof of installed-source-to-binary provenance.',
              'privacy': 'No personal memory, raw image/audio/log contents, unrestricted environment, hostname, username or credentials are collected. Paths may identify local directories; keep this output private.',
              'sources': {'stock': git_info(root), 'whisper': git_info(root / 'whisper.cpp'), 'tts_llama': git_info(root / 'llama.cpp')},
              'model_files': records, 'binaries': binaries,
              'current_shell_flags': {key: os.environ[key] for key in FLAGS if key in os.environ},
              'flags_note': 'Current collector shell only; does not prove the environment of a running Athena process.',
              'remaining': ['Actual loaded model/config/feature report', 'Linked native libraries and package SBOM', 'Driver/OS build', 'Conversion logs and dataset rights', 'Runtime traces and acceptance', 'Maintainer review and signature']}
    if args.overlay:
        result['sources']['overlay'] = git_info(args.overlay.resolve())
    output.parent.mkdir(parents=True, exist_ok=True)
    # Exclusive creation avoids replacing a prior historical record; 0600 protects new bytes.
    fd = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'w') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')
    print('Saved private supplement: ' + str(output))

if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError) as error:
        print('FAIL: ' + str(error), file=sys.stderr)
        sys.exit(1)
