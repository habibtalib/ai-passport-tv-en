#!/usr/bin/env python3
"""Bootstrap the standalone TV service on macOS/Linux without changing media code."""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import re
import secrets
import shutil
import socket
import string
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def service_directory(root: Path) -> Path:
    exported = root / 'service'
    return exported if exported.is_dir() else root.parents[1] / 'companion/tv'


def read_settings(path: Path) -> dict[str, str]:
    if path.stat().st_size > 65536:
        raise ValueError(f'{path.name}: configuration too large')
    settings = {}
    for number, raw in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        key, separator, value = line.partition('=')
        key, value = key.strip(), value.strip()
        if not separator or not re.fullmatch(r'TV_[A-Z0-9_]+', key) or key in settings:
            raise ValueError(f'{path.name}:{number}: invalid or duplicate setting (need unique TV_NAME=value)')
        if len(value) >= 2 and value[0] in "\"'" and value[-1] == value[0]:
            value = value[1:-1]
        settings[key] = value
    return settings


def effective_environment(root: Path, inherited: dict[str, str], platform: str) -> dict[str, str]:
    result = {**inherited, **read_settings(root / 'stream.env.example')}
    if (root / 'server.env').exists():
        result.update(read_settings(root / 'server.env'))
    result.update({key: value for key, value in inherited.items() if key.startswith('TV_')})
    if platform == 'darwin':
        # Only Linux SO_MAX_PACING_RATE is disabled; userspace pacing stays intact.
        result['TV_TCP_PACING_RATE'] = '0'
    result.setdefault('TV_BIND', '0.0.0.0')
    result.setdefault('TV_PORT', '10745')
    result['TV_CHANNELS_FILE'] = str(root / 'channels.txt')
    if not result['TV_BIND'] or not 1 <= int(result['TV_PORT']) <= 65535:
        raise ValueError('invalid TV_BIND / TV_PORT (bind address or port)')
    if 'TV_PAIRING_TOKEN' in result:
        raise ValueError('unset TV_PAIRING_TOKEN; this launcher uses only pairing.token')
    return result


def write_new(path: Path, content: str) -> bool:
    try:
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError:
        return False
    with os.fdopen(fd, 'w', encoding='utf-8') as stream:
        stream.write(content)
        stream.flush()
        os.fsync(stream.fileno())
    return True


def prepare_configuration(root: Path, interactive: bool) -> None:
    defaults = (root / 'stream.env.example').read_text(encoding='utf-8')
    write_new(root / 'server.env', '# Local overrides; restart after editing.\nTV_BIND=0.0.0.0\nTV_PORT=10745\n' + defaults)
    write_new(root / 'pairing.token', secrets.token_hex(24) + '\n')
    write_new(root / 'pairing.code', ''.join(secrets.choice(string.ascii_uppercase + string.digits)
                                           for _ in range(4)) + '\n')
    write_new(root / 'channels.txt', (root / 'channels.example.txt').read_text(encoding='utf-8'))
    channels = root / 'channels.txt'
    if any(line.strip() and not line.lstrip().startswith('#') for line in channels.read_text(encoding='utf-8').splitlines()):
        return
    if not interactive:
        raise ValueError('add a live URL to channels.txt before starting')
    print('First channel setup (HTTP/HTTPS live source)')
    name = input('Channel name: ').strip()
    url = input('Live URL: ').strip()
    if not name or '|' in name or not url.startswith(('http://', 'https://')) or '|' in url:
        raise ValueError('invalid channel name or URL')
    # Keep comments and never overwrite an existing nonempty user's channel list.
    with channels.open('a', encoding='utf-8') as stream:
        stream.write(f'\ndemo | {name} | {url}\n')


def check_configuration(root: Path, env: dict[str, str], service: Path, python: Path) -> None:
    # Import the actual service in a clean process with the effective environment.
    # No channel URL is fetched and no listening socket is opened here.
    check = '''from pathlib import Path
import sys
from server.standalone import PairingSession
from server.live import load_channels
from server.tv_server import load_token
from server.pairing import PairingBackend
token = load_token(Path(sys.argv[1]))
if token is None or len(token) > 127:
    raise ValueError("pairing token must fit the device's 16..127 ASCII capacity")
load_channels(Path(sys.argv[2]))
PairingBackend.from_env(token, Path(sys.argv[3]), Path(sys.argv[4]))
'''
    subprocess.run([str(python), '-c', check, str(root / 'pairing.token'), str(root / 'channels.txt'),
                    str(root / 'pairing.code'), str(root / 'paired-devices.json')],
                   cwd=service, env=env, check=True)


def ensure_runtime(root: Path, service: Path) -> Path:
    venv = root / '.venv'
    python = venv / 'bin/python'
    if not python.is_file():
        if importlib.util.find_spec('ensurepip') is None:
            raise ValueError('missing Python venv support; install python3-venv (see README)')
        print('Creating virtual environment', flush=True)
        subprocess.run([sys.executable, '-m', 'venv', str(venv)], check=True)
    requirements = service / 'requirements.txt'
    digest = hashlib.sha256(requirements.read_bytes()).hexdigest()
    stamp = venv / '.tv-requirements.sha256'
    healthy = subprocess.run([str(python), '-c', 'import numpy'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
    if not healthy or not stamp.is_file() or stamp.read_text().strip() != digest:
        print('Installing service dependencies (network needed on first run)', flush=True)
        subprocess.run([str(python), '-m', 'pip', 'install', '-r', str(requirements)], check=True)
        stamp.write_text(digest + '\n')
    return python


def local_addresses() -> list[str]:
    values = set()
    try:
        values.update(socket.gethostbyname_ex(socket.gethostname())[2])
    except OSError:
        pass
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as connection:
            connection.connect(('192.0.2.1', 9))  # Route lookup only; no packet sent.
            values.add(connection.getsockname()[0])
    except OSError:
        pass
    return sorted(value for value in values if not value.startswith('127.'))


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--setup-only', action='store_true', help='prepare configuration and dependencies, do not start')
    parser.add_argument('--check', action='store_true', help='check environment and configuration only; do not install or start')
    parser.add_argument('--show-pairing', action='store_true', help='display the pairing code locally')
    args = parser.parse_args(argv)
    if sys.platform not in ('darwin', 'linux'):
        raise ValueError('macOS and Linux only')
    root, service = ROOT, service_directory(ROOT)
    if args.show_pairing:
        code = (root / 'pairing.code').read_text().strip()
        if not re.fullmatch(r'[A-Z0-9]{4}', code, re.ASCII):
            raise ValueError('invalid pairing.code')
        print(code)
        return 0
    for executable in ('ffmpeg', 'ffprobe'):
        if not shutil.which(executable):
            raise ValueError(f'missing {executable}; install prerequisites from README')
    if not args.check:
        prepare_configuration(root, sys.stdin.isatty())
    env = effective_environment(root, dict(os.environ), sys.platform)
    if args.check:
        python = root / '.venv/bin/python'
        if not python.is_file():
            raise ValueError('run --setup-only first')
        check_configuration(root, env, service, python)
        print('PASS: Python / FFmpeg / channels / pairing / server configuration')
        return 0
    python = ensure_runtime(root, service)
    check_configuration(root, env, service, python)
    print('Configuration ready', flush=True)
    print(f"Device port: {env['TV_PORT']}", flush=True)
    for address in local_addresses():
        print(f'Host address: {address}', flush=True)
    print('Pairing code: ./start-server.sh --show-pairing', flush=True)
    print('macOS: userspace pacing' if sys.platform == 'darwin' else 'Linux: configured TCP + userspace pacing', flush=True)
    if args.setup_only:
        return 0
    print('Starting; keep this window open, Ctrl+C to stop', flush=True)
    os.chdir(service)
    os.execve(str(python), [str(python), '-u', '-m', 'server.standalone', '--bind', env['TV_BIND'], '--port', env['TV_PORT'], '--token-file', str(root / 'pairing.token')], env)
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'Cannot start: {error}', file=sys.stderr)
        raise SystemExit(1)
    except (KeyboardInterrupt, EOFError):
        print('\nCancelled', file=sys.stderr)
        raise SystemExit(130)
