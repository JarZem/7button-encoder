#!/usr/bin/env python3
"""Delete the local per-device private key after a successful flash; optionally remove the remaining public build copies too."""

from __future__ import annotations

import argparse
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description='Remove local ESP credential build artifacts after successful flashing.')
    parser.add_argument('--dir', type=Path, default=Path('device_credentials'))
    parser.add_argument('--all', action='store_true', help='Also remove public certificate/build copies from the workstation')
    args = parser.parse_args()

    directory = args.dir.expanduser().resolve()
    private_key = directory / 'device_private.pem'
    if private_key.exists():
        private_key.unlink()
        print(f'Deleted private key: {private_key}')
    else:
        print(f'Private key already absent: {private_key}')

    if args.all:
        for name in ('device_cert.pem', 'root_ca_cert.pem', 'ota_server_cert.pem', 'ota_server_public.pem'):
            path = directory / name
            if path.exists():
                path.unlink()
                print(f'Deleted public build copy: {path}')


if __name__ == '__main__':
    main()
