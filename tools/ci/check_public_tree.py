#!/usr/bin/env python3
"""Check source exports or Git index contents; never echo matched credentials."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[2]
ALLOWED_EXTENSIONS = {'.c', '.h', '.s', '.py', '.ps1', '.md', '.txt', '.json', '.yml', '.yaml', '.uvprojx'}
ALLOWED_DOTFILES = {'.gitignore', '.gitattributes'}
BLOCKED_DIRS = {'.codex', '.openai', '.claude', '.cursor', '.vscode-server',
                'objects', 'listings', 'debugconfig', 'tmp', 'releases', '.venv', 'venv'}
PATTERNS = {
    'API or access token': re.compile(
        r'\b(?:sk-(?:proj-|svcacct-)?[A-Za-z0-9_-]{20,}|gh[pousr]_[A-Za-z0-9]{20,}'
        r'|github_pat_[A-Za-z0-9_]{20,}|AKIA[0-9A-Z]{16}|LTAI[A-Za-z0-9]{16,})'),
    'private key': re.compile(r'-----BEGIN (?:RSA |EC |OPENSSH |ENCRYPTED )?PRIVATE KEY-----'),
    'signed resource URL': re.compile(r'(?i)[?&](?:Signature|OSSAccessKeyId|X-Amz-Signature|x-oss-signature)='),
    'personal absolute path': re.compile(r'(?i)[A-Z]:[\\/](?:Users|Tools)[\\/]'),
    'JWT-like credential': re.compile(r'\beyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}'),
}
CREDENTIAL_MACRO = re.compile(
    r'^\s*#define\s+(MQTT_(?:PASSWORD|USERNAME|CLIENT_ID|BROKER_HOST|OTA_CMD_TOPIC|OTA_STATUS_TOPIC)'
    r'|OTA_FIRMWARE_HMAC_KEY_HEX)\s+"([^"\r\n]*)"', re.MULTILINE)


def inspect(name: str, data: bytes) -> list[str]:
    """Return rule labels only; callers must never print the matching value."""
    path = Path(name)
    reasons = []
    parts = {part.lower() for part in path.parts}
    if parts & BLOCKED_DIRS or path.name.lower() in {'otasecrets.h', 'auth.json', 'credentials.json'}:
        reasons.append('private or generated file path')
    if path.name.startswith('.env') or '.uvguix.' in path.name.lower():
        reasons.append('local configuration')
    if path.name not in ALLOWED_DOTFILES and path.suffix.lower() not in ALLOWED_EXTENSIONS:
        reasons.append('not an approved source/document format')
    if b'\x00' in data:
        return reasons + ['binary content']
    try:
        text = data.decode('utf-8-sig')
    except UnicodeDecodeError:
        return reasons + ['not UTF-8 source text']
    reasons.extend(label for label, pattern in PATTERNS.items() if pattern.search(text))
    for match in CREDENTIAL_MACRO.finditer(text):
        value = match.group(2)
        if value and 'replace-' not in value:
            reasons.append('non-placeholder credential macro: ' + match.group(1))
    return reasons


def tracked_entries(root: Path):
    """Read staged blobs, so cleaning only the working file cannot hide a leak."""
    top = subprocess.check_output(['git', 'rev-parse', '--show-toplevel'], cwd=root).decode('utf-8').strip()
    if Path(top).resolve() != root.resolve():
        raise RuntimeError('--tracked requires this directory to be the Git root')
    entries = subprocess.check_output(['git', 'ls-files', '--stage', '-z'], cwd=root).split(b'\x00')
    for entry in entries:
        if not entry:
            continue
        meta, raw_name = entry.split(b'\t', 1)
        mode, oid, stage = meta.decode('ascii').split()
        name = raw_name.decode('utf-8')
        if mode not in {'100644', '100755'} or stage != '0':
            raise RuntimeError('Unsupported link, submodule or conflict in index: ' + name)
        yield name, subprocess.check_output(['git', 'cat-file', 'blob', oid], cwd=root)


def disk_entries(root: Path):
    for path in sorted(root.rglob('*')):
        rel = path.relative_to(root)
        if '.git' in rel.parts:
            continue
        if path.is_symlink():
            raise RuntimeError('Symlink is not allowed: ' + rel.as_posix())
        if '__pycache__' in rel.parts:
            continue
        if path.is_file():
            yield rel.as_posix(), path.read_bytes()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tracked', action='store_true', help='inspect staged Git blobs, not working-tree files')
    args = parser.parse_args()
    findings = []
    count = 0
    for name, data in tracked_entries(ROOT) if args.tracked else disk_entries(ROOT):
        count += 1
        findings.extend((name, reason) for reason in inspect(name, data))
    if not count:
        raise SystemExit('FAIL: no files to inspect')
    for name, reason in findings:
        print(f'FAIL: {name}: {reason}')
    if findings:
        raise SystemExit(1)
    print(f'Public source scan PASS: {count} files; heuristic check, not a security guarantee')


if __name__ == '__main__':
    main()
