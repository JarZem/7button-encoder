#!/usr/bin/env python3
"""Delete the local per-device private key and build artifacts after a successful flash."""

from __future__ import annotations

import argparse
import shutil
from pathlib import Path

KNOWN_BUILD_DIRS = ('build', 'build_full', 'build_wifi_diag')


def main() -> None:
    parser = argparse.ArgumentParser(description='Remove local ESP private credential copies after successful flashing.')
    parser.add_argument('--dir', type=Path, default=Path('device_credentials'))
    parser.add_argument('--all', action='store_true', help='Also remove public certificate copies from the credential workspace')
    parser.add_argument('--keep-build', action='store_true', help='Do not remove ESP-IDF build directories (not recommended after provisioning)')
    args = parser.parse_args()

    project_dir = Path.cwd().resolve()
    directory = args.dir.expanduser().resolve()
    private_key = directory / 'device_private.pem'
    if private_key.exists():
        private_key.unlink()
        print(f'Deleted private key: {private_key}')
    else:
        print(f'Private key already absent: {private_key}')

    if not args.keep_build:
        for name in KNOWN_BUILD_DIRS:
            build_dir = project_dir / name
            if build_dir.is_dir():
                shutil.rmtree(build_dir)
                print(f'Deleted build directory containing embedded private-key copies: {build_dir}')

    if args.all:
        for name in ('device_cert.pem', 'root_ca_cert.pem', 'ota_server_cert.pem', 'ota_server_public.pem'):
            path = directory / name
            if path.exists():
                path.unlink()
                print(f'Deleted public build copy: {path}')

    print('Any firmware .bin copied outside the build directories still contains the embedded device private key and must be treated as sensitive.')


if __name__ == '__main__':
    main()
