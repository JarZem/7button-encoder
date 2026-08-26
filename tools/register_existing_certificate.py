#!/usr/bin/env python3
from __future__ import annotations

import json
import ssl
import urllib.error
import urllib.request
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
CREDENTIALS = PROJECT / 'device_credentials'
CERT = CREDENTIALS / 'device_cert.pem'
ROOT_CA = CREDENTIALS / 'root_ca_cert.pem'
MANIFEST = PROJECT / '.jarzem_ota' / 'project.json'
ACCEPTED = {'REGISTERED', 'UNCHANGED', 'REPLACED'}


def main() -> None:
    for path in (CERT, ROOT_CA, MANIFEST):
        if not path.is_file():
            raise SystemExit(f'Required file missing: {path}')

    config = json.loads(MANIFEST.read_text(encoding='utf-8'))
    publish_url = str(config.get('publish_url') or '').rstrip('/')
    if not publish_url.startswith('https://'):
        raise SystemExit('Invalid or missing publish_url in .jarzem_ota/project.json')

    host = publish_url[len('https://'):].split('/', 1)[0].split(':', 1)[0]
    manufacturing_url = f'https://{host}:8451/api/manufacturing/register-device'
    body = json.dumps({
        'device_certificate_pem': CERT.read_text(encoding='ascii')
    }).encode('utf-8')
    request = urllib.request.Request(
        manufacturing_url,
        data=body,
        headers={'Content-Type': 'application/json'},
        method='POST',
    )
    context = ssl.create_default_context(cafile=str(ROOT_CA))

    try:
        with urllib.request.urlopen(request, timeout=15, context=context) as response:
            result = json.loads(response.read().decode('utf-8'))
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode(errors='replace')
        raise SystemExit(f'Existing certificate registration rejected HTTP {exc.code}: {detail}')
    except Exception as exc:
        raise SystemExit(f'Existing certificate registration failed: {exc}')

    status = str(result.get('status') or '')
    if status not in ACCEPTED:
        raise SystemExit(f'Existing certificate registration rejected: {result}')
    print(f'OTA certificate registry ready: {status}')


if __name__ == '__main__':
    main()
