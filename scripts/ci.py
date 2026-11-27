#!/usr/bin/env python3
"""Plan CI builds and execute bounded, chip-specific batches."""

import argparse
from collections import defaultdict
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

import build

ROOT = Path(__file__).resolve().parents[1]
BATCH_SIZE = 8


def shards_for(catalog, selected):
    targets = defaultdict(list)
    for variant in sorted(catalog, key=lambda item: item['name']):
        targets[variant['target']].append(variant)
    names = {item['name'] for item in selected}
    shards = []
    for target, variants in sorted(targets.items()):
        count = (len(variants) + BATCH_SIZE - 1) // BATCH_SIZE
        for index in range(count):
            subset = [item for item in variants[index::count] if item['name'] in names]
            if subset:
                shards.append({'id': f'{target}-{index + 1:02d}', 'target': target,
                               'variants': subset})
    return shards


def select_for_event(catalog, event_name, event, diff):
    if event_name in ('release', 'workflow_dispatch'):
        return catalog
    if event_name == 'pull_request':
        paths = diff(event['pull_request']['base']['sha'],
                     event['pull_request']['head']['sha'])
    elif event_name == 'push':
        before = event['before']
        if not before.strip('0'):
            return catalog
        paths = diff(before, event['after'])
    else:
        raise ValueError(f'Unsupported event: {event_name}')
    if paths is None:
        return catalog
    selected = build.select_changed(catalog, paths)
    if event_name == 'push' and event['ref'] == 'refs/heads/main' and selected:
        return catalog
    return selected


def changed_paths(base, head):
    # A rewritten push may reference a commit absent from checkout's history.
    # None requests a full build; an empty list means a successful, empty diff.
    def has_base():
        return subprocess.run(['git', 'cat-file', '-e', base + '^{commit}'], cwd=ROOT,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0

    if not has_base():
        fetched = subprocess.run(['git', 'fetch', '--no-tags', '--depth=1', 'origin', base],
                                 cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if fetched.returncode != 0 or not has_base():
            print(f'::warning::Diff base {base} is unavailable; building all variants.', flush=True)
            return None
    return subprocess.check_output(['git', 'diff', '--name-only', base, head],
                                   cwd=ROOT, text=True).splitlines()


def emit_output(name, value):
    with open(os.environ['GITHUB_OUTPUT'], 'a') as output:
        output.write(f'{name}={value}\n')


def find_reusable_run(catalog, sha, repository, request, wait=None, attempts=280):
    query = urllib.parse.urlencode({'head_sha': sha, 'event': 'push',
                                    'branch': 'main', 'per_page': 10})
    expected = {'ci-plan'} | {'firmware-' + shard['id'] for shard in shards_for(catalog, catalog)}
    deadline = time.monotonic() + 140 * 60
    for attempt in range(attempts + 1):
        runs = request(f'/repos/{repository}/actions/workflows/build.yml/runs?{query}')
        pending = False
        for run in runs['workflow_runs']:
            if (run['head_sha'] != sha or run['event'] != 'push' or run['head_branch'] != 'main'
                    or run['head_repository']['full_name'] != repository):
                continue
            if run['status'] != 'completed':
                pending = True
                continue
            if run['conclusion'] != 'success':
                continue
            artifacts = request(f"/repos/{repository}/actions/runs/{run['id']}/artifacts?per_page=100")
            available = {item['name'] for item in artifacts['artifacts'] if not item['expired']}
            if expected <= available:
                return str(run['id'])
        remaining = deadline - time.monotonic()
        if not pending or wait is None or attempt == attempts or remaining <= 0:
            return ''
        print('Waiting for the same commit on main before starting another full build...', flush=True)
        wait(min(30, remaining))
    return ''


def github_request(path):
    headers = {'Accept': 'application/vnd.github+json', 'User-Agent': 'xiaoling-ci'}
    token = os.environ.get('GH_TOKEN')
    if token:
        headers['Authorization'] = 'Bearer ' + token
    url = os.environ.get('GITHUB_API_URL', 'https://api.github.com') + path
    with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=30) as response:
        return json.load(response)


def plan():
    event = json.loads(Path(os.environ['GITHUB_EVENT_PATH']).read_text())
    catalog = build._collect_variants()
    selected = select_for_event(catalog, os.environ['GITHUB_EVENT_NAME'], event, changed_paths)
    shards = shards_for(catalog, selected)
    reused_run = ''
    if os.environ['GITHUB_EVENT_NAME'] == 'release':
        tag = event['release']['tag_name']
        if tag != f'xiaoling/v{build.get_project_version()}':
            raise ValueError(f'Release tag does not match PROJECT_VER: {tag}')
        try:
            reused_run = find_reusable_run(catalog, os.environ['GITHUB_SHA'],
                                           os.environ['GITHUB_REPOSITORY'], github_request, wait=time.sleep)
        except (urllib.error.URLError, TimeoutError) as error:
            print(f'::warning::Cannot look up existing artifacts; building the release: {error}')
    result = {'sha': os.environ['GITHUB_SHA'], 'variants': selected, 'shards': shards,
              'full': selected == catalog, 'reused_run': reused_run}
    Path('ci-plan.json').write_text(json.dumps(result, indent=2) + '\n')
    emit_output('matrix', json.dumps({'include': [] if reused_run else shards}, separators=(',', ':')))
    emit_output('count', len(selected))
    emit_output('reused_run', reused_run)
    with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as summary:
        summary.write(f"### Build plan\n\n{len(selected)} variants in {len(shards)} chip batches.\n\n")
        if reused_run:
            summary.write(f'Reusing complete, successful main build: {reused_run}.\n\n')
        summary.write('\n'.join(f"- `{shard['id']}`: " + ', '.join(
            item['name'] for item in shard['variants']) for shard in shards) + '\n')


@contextmanager
def clean_workspace():
    # Materialize tracked files from the LFS-populated checkout. A git archive
    # would contain LFS pointers instead of the actual resource bytes.
    workspace = ROOT / '.cache/ci-source'
    if workspace.exists():
        shutil.rmtree(workspace)
    workspace.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['git', 'clone', '--quiet', '--shared', '--no-checkout',
                    str(ROOT), str(workspace)], check=True)
    try:
        subprocess.run(['git', '-C', str(workspace), 'read-tree', 'HEAD'], check=True)
        paths = subprocess.check_output(['git', 'ls-files', '-z'], cwd=ROOT).split(b'\0')
        for value in paths:
            if not value:
                continue
            relative = Path(os.fsdecode(value))
            destination = workspace / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, destination, follow_symlinks=False)
        yield workspace
    finally:
        shutil.rmtree(workspace)


def run_batch(shard):
    variants = json.loads(os.environ['CI_VARIANTS'])
    output = ROOT / 'ci-artifacts' / shard
    output.mkdir(parents=True, exist_ok=True)
    records = []
    for variant in variants:
        name = variant['name']
        print(f'::group::Build {name}', flush=True)
        start = time.monotonic()
        log = output / f'{name}.log'
        with clean_workspace() as workspace:
            environment = os.environ.copy()
            environment['CCACHE_BASEDIR'] = str(workspace)
            with log.open('w') as stream:
                result = subprocess.run([
                    sys.executable, str(workspace / 'scripts/build.py'), variant['board'],
                    '--name', name, '--build-dir', str(workspace / 'build'),
                ], cwd=workspace, env=environment, stdout=stream, stderr=subprocess.STDOUT)
            success = result.returncode == 0
            if success:
                archive = workspace / 'releases' / f'v{build.get_project_version()}_{name}.zip'
                destination = output / f'xiaoling_{name}_v{build.get_project_version()}.zip'
                shutil.copyfile(archive, destination)
                digest = hashlib.sha256(destination.read_bytes()).hexdigest()
            else:
                digest = None
                print('\n'.join(log.read_text(errors='replace').splitlines()[-100:]), flush=True)
                print(f'::error title=Build failed::{name}; see {name}.log', flush=True)
            records.append({**variant, 'success': success, 'seconds': round(time.monotonic() - start),
                            'sha256': digest})
            print(f'::endgroup::', flush=True)
    (output / f'{shard}.json').write_text(json.dumps({
        'sha': os.environ['GITHUB_SHA'], 'shard': shard, 'variants': records,
    }, indent=2) + '\n')
    with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as summary:
        summary.write('| Variant | Result | Seconds |\n| --- | --- | ---: |\n')
        for record in records:
            summary.write(f"| {record['name']} | {'passed' if record['success'] else 'failed'} | {record['seconds']} |\n")
    return int(any(not record['success'] for record in records))


def release_assets(source, destination):
    catalog = build._collect_variants()
    expected = {item['name']: item for item in catalog}
    records = {}
    for manifest in source.glob('*.json'):
        content = json.loads(manifest.read_text())
        if content['sha'] != os.environ['GITHUB_SHA']:
            raise ValueError(f'Wrong firmware commit: {manifest.name}')
        for record in content['variants']:
            name = record['name']
            if name in records or name not in expected or not record['success']:
                raise ValueError(f'Duplicate, unknown or failed firmware: {name}')
            if any(record[key] != expected[name][key] for key in ('board', 'target')):
                raise ValueError(f'Wrong firmware board/target: {name}')
            records[name] = record
    if records.keys() != expected.keys():
        raise ValueError('Release requires every variant: ' + ', '.join(sorted(expected.keys() - records.keys())))
    files = []
    for name, record in records.items():
        archive = source / f'xiaoling_{name}_v{build.get_project_version()}.zip'
        if hashlib.sha256(archive.read_bytes()).hexdigest() != record['sha256']:
            raise ValueError(f'Firmware checksum mismatch: {name}')
        files.append(archive)
    # Validate the whole set before making anything available to the upload step.
    destination.mkdir(parents=True, exist_ok=True)
    for archive in files:
        shutil.copyfile(archive, destination / archive.name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('plan')
    batch = commands.add_parser('build')
    batch.add_argument('shard')
    release = commands.add_parser('release-assets')
    release.add_argument('source', type=Path)
    release.add_argument('destination', type=Path)
    args = parser.parse_args()
    if args.command == 'plan':
        plan()
        return 0
    if args.command == 'release-assets':
        release_assets(args.source, args.destination)
        return 0
    return run_batch(args.shard)


if __name__ == '__main__':
    raise SystemExit(main())
