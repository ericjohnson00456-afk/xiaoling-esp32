#!/usr/bin/env python3
"""Generate application defaults from the selected board's manufacturer data."""

import argparse
import json
from pathlib import Path
import re


FIELDS = {'product_id': 'ProductId', 'secret_id': 'SecretId'}


def read_defaults(path: Path) -> dict[str, str]:
    data = json.loads(path.read_text())
    if not isinstance(data, dict) or data.keys() - FIELDS.keys():
        raise ValueError(f'Invalid application defaults: {path}')
    if any(not isinstance(value, str) for value in data.values()):
        raise ValueError(f'Application defaults must be strings: {path}')
    return data


def generate(board_config: Path, boards_dir: Path, output: Path) -> None:
    board = json.loads(board_config.read_text())
    manufacturer = board.get('manufacturer', '')
    if not isinstance(manufacturer, str) or (manufacturer and not re.fullmatch(
            r'[a-z0-9][a-z0-9._-]*', manufacturer)) or manufacturer in ('.', '..'):
        raise ValueError(f'Invalid board manufacturer: {board_config}')
    defaults = {'Global': read_defaults(boards_dir / 'defaults.json'), 'Manufacturer': {}}
    if manufacturer:
        path = boards_dir / manufacturer / 'defaults.json'
        if path.exists():
            defaults['Manufacturer'] = read_defaults(path)
    lines = ['// Generated from application defaults; do not edit.', '#pragma once',
             '#include <string_view>', '', 'namespace ProductDefaults {']
    for scope, fields in defaults.items():
        for key, suffix in FIELDS.items():
            value = json.dumps(fields.get(key, ''), ensure_ascii=False)
            lines.append(f'inline constexpr std::string_view k{scope}{suffix} = {value};')
    lines.append('}  // namespace ProductDefaults')
    text = '\n'.join(lines) + '\n'
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.exists() or output.read_text() != text:
        output.write_text(text)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--board-config', type=Path, required=True)
    parser.add_argument('--boards-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    generate(args.board_config, args.boards_dir, args.output)
