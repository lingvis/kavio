#!/usr/bin/env python3
"""Check the pinned Device SDK source and host build prerequisites."""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = Path(__file__).with_name('dependencies.json')


def get_version(command):
    try:
        output = subprocess.check_output([command, '--version'], stderr=subprocess.STDOUT, text=True)
    except (OSError, subprocess.CalledProcessError):
        return None
    match = re.search(r'\d+(?:\.\d+)+|\d+', output)
    return match.group(0) if match else None


def version_tuple(value):
    return tuple(int(item) for item in value.split('.'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--python', action='store_true', help='check Python binding requirements')
    parser.add_argument('--lua', action='store_true', help='check Lua binding requirements')
    parser.add_argument('--ui', action='store_true', help='check LVGL and FreeType requirements')
    parser.add_argument('--all', action='store_true', help='check all optional SDK components')
    args = parser.parse_args()
    enabled = set()
    if args.all or args.python:
        enabled.add('python')
    if args.all or args.lua:
        enabled.add('lua')
    if args.all or args.ui:
        enabled.add('ui')
    manifest = json.loads(MANIFEST.read_text(encoding='utf-8'))
    if 'python' in enabled:
        print(
            '[INFO] Python Native Binding uses the pinned Python 3.8 ABI; '
            'the Python interpreter running this build checker is independent.',
            flush=True,
        )
    failures = []
    for dependency in manifest['thirdParty']:
        if dependency.get('component') and dependency['component'] not in enabled:
            continue
        base = ROOT / dependency['path']
        missing = [str(base / name) for name in dependency['requiredFiles'] if not (base / name).is_file()]
        if missing:
            failures.append(dependency['name'] + ': missing ' + ', '.join(missing))
        else:
            print('[OK] {} {} ({})'.format(dependency['name'], dependency['version'], base), flush=True)
    for dependency in manifest['system']:
        if dependency.get('component') and dependency['component'] not in enabled:
            continue
        command = dependency['command']
        location = shutil.which(command) if not command.startswith('/') else (command if Path(command).is_file() else None)
        if not location:
            failures.append('{}: executable not found: {} (apt: {})'.format(dependency['name'], command, dependency['apt']))
            continue
        version = get_version(command)
        if dependency.get('exactMinorVersion'):
            try:
                version = subprocess.check_output([command, '-c', 'import sys; print("{}.{}".format(*sys.version_info[:2]))'], text=True).strip()
            except (OSError, subprocess.CalledProcessError):
                version = None
            if version != dependency['exactMinorVersion']:
                failures.append('{}: expected {}, found {}'.format(dependency['name'], dependency['exactMinorVersion'], version))
                continue
        elif dependency.get('minimumVersion') and (not version or version_tuple(version) < version_tuple(dependency['minimumVersion'])):
            failures.append('{}: need >= {}, found {}'.format(dependency['name'], dependency['minimumVersion'], version))
            continue
        header = dependency.get('header')
        if header and not Path(header).is_file():
            failures.append('{}: missing {} (apt: {})'.format(dependency['name'], header, dependency['apt']))
            continue
        library = dependency.get('library')
        if library and not Path(library).is_file():
            failures.append('{}: missing {} (apt: {})'.format(dependency['name'], library, dependency['apt']))
            continue
        print('[OK] {}: {} ({})'.format(dependency['name'], version or 'present', location), flush=True)
    if failures:
        for problem in failures:
            print('[FAIL] ' + problem, file=sys.stderr, flush=True)
        return 1
    print('All selected SDK prerequisites are present.', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
