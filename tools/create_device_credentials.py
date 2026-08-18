#!/usr/bin/env python3
"""Create one CA-signed ESP device identity locally, register only its public certificate with OTA, and fetch OTA public trust material for the firmware build."""

from __future__ import annotations

import argparse
import getpass
import json
import ssl
import urllib.parse
import urllib.request
from datetime import datetime, timedelta, timezone
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

ROOT_CA_CERT_NAME = 'root_ca_cert.pem'
ROOT_CA_KEY_NAME = 'root_ca_private.pem'
DEVICE_CERT_NAME = 'device_cert.pem'
DEVICE_KEY_NAME = 'device_private.pem'
OTA_CERT_NAME = 'ota_server_cert.pem'
OTA_PUBLIC_NAME = 'ota_server_public.pem'
PKI_URI_PREFIX = 'urn:jarzem:esp:pki:'
ROLE_URI = PKI_URI_PREFIX + 'role:device'


def ask(prompt: str, default: str | None = None) -> str:
    suffix = f' [{default}]' if default is not None else ''
    value = input(f'{prompt}{suffix}: ').strip()
    return value or (default or '')


def normalize_device_id(value: str) -> tuple[str, str]:
    compact = value.strip().lower().replace('0x', '').replace(':', '')
    if len(compact) != 16 or any(ch not in '0123456789abcdef' for ch in compact):
        raise ValueError('device id must be an 8-byte IEEE address')
    colon = ':'.join(compact[i:i + 2] for i in range(0, 16, 2))
    return compact, colon


def uri_value(value: str) -> str:
    return urllib.parse.quote(value, safe='-._~')


def load_ca(ca_dir: Path) -> tuple[x509.Certificate, ec.EllipticCurvePrivateKey]:
    cert_path = ca_dir / ROOT_CA_CERT_NAME
    key_path = ca_dir / ROOT_CA_KEY_NAME
    cert = x509.load_pem_x509_certificate(cert_path.read_bytes())
    password_text = getpass.getpass('Root CA private key password (empty only if unencrypted): ')
    password = password_text.encode('utf-8') if password_text else None
    key = serialization.load_pem_private_key(key_path.read_bytes(), password=password)
    if not isinstance(key, ec.EllipticCurvePrivateKey):
        raise RuntimeError('Root CA private key is not EC')
    if cert.public_key().public_numbers() != key.public_key().public_numbers():
        raise RuntimeError('Root CA certificate/private key mismatch')
    return cert, key


def make_device_certificate(ca_cert: x509.Certificate, ca_key: ec.EllipticCurvePrivateKey,
                            ecosystem: str, device_id: str, compact_id: str,
                            device_group: str, device_model: str, product_role: str,
                            hardware_revision: str, chip_family: str, flash_size: str,
                            days: int) -> tuple[ec.EllipticCurvePrivateKey, x509.Certificate]:
    device_key = ec.generate_private_key(ec.SECP256R1())
    now = datetime.now(timezone.utc)
    subject = x509.Name([
        x509.NameAttribute(NameOID.ORGANIZATION_NAME, ecosystem),
        x509.NameAttribute(NameOID.ORGANIZATIONAL_UNIT_NAME, device_group),
        x509.NameAttribute(NameOID.COMMON_NAME, f'ESP Device {device_id}'),
        x509.NameAttribute(NameOID.SERIAL_NUMBER, device_id),
    ])
    uris = [
        ROLE_URI,
        f'{PKI_URI_PREFIX}device:{compact_id}',
        f'{PKI_URI_PREFIX}group:{uri_value(device_group)}',
        f'{PKI_URI_PREFIX}model:{uri_value(device_model)}',
        f'{PKI_URI_PREFIX}product-role:{uri_value(product_role)}',
        f'{PKI_URI_PREFIX}hardware:{uri_value(hardware_revision)}',
        f'{PKI_URI_PREFIX}chip:{uri_value(chip_family)}',
        f'{PKI_URI_PREFIX}flash:{uri_value(flash_size)}',
    ]
    cert = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(ca_cert.subject)
        .public_key(device_key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - timedelta(minutes=5))
        .not_valid_after(now + timedelta(days=days))
        .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
        .add_extension(x509.KeyUsage(
            digital_signature=True, content_commitment=False, key_encipherment=False,
            data_encipherment=False, key_agreement=False, key_cert_sign=False,
            crl_sign=False, encipher_only=False, decipher_only=False,
        ), critical=True)
        .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.CLIENT_AUTH]), critical=False)
        .add_extension(x509.SubjectAlternativeName([x509.UniformResourceIdentifier(uri) for uri in uris]), critical=False)
        .add_extension(x509.SubjectKeyIdentifier.from_public_key(device_key.public_key()), critical=False)
        .add_extension(x509.AuthorityKeyIdentifier.from_issuer_public_key(ca_key.public_key()), critical=False)
        .sign(ca_key, hashes.SHA256())
    )
    return device_key, cert


def https_context(root_ca_path: Path) -> ssl.SSLContext:
    return ssl.create_default_context(cafile=str(root_ca_path))


def ota_get(ota_base: str, endpoint: str, root_ca_path: Path) -> bytes:
    req = urllib.request.Request(f'{ota_base.rstrip("/")}{endpoint}')
    with urllib.request.urlopen(req, timeout=15, context=https_context(root_ca_path)) as response:
        return response.read()


def register_with_ota(ota_base: str, root_ca_path: Path, certificate_pem: bytes) -> dict:
    body = json.dumps({'device_certificate_pem': certificate_pem.decode('ascii')}).encode('utf-8')
    req = urllib.request.Request(
        f'{ota_base.rstrip("/")}/api/manufacturing/register-device',
        data=body,
        headers={'Content-Type': 'application/json'},
        method='POST',
    )
    with urllib.request.urlopen(req, timeout=15, context=https_context(root_ca_path)) as response:
        return json.loads(response.read().decode('utf-8'))


def main() -> None:
    parser = argparse.ArgumentParser(description='Create and register one ESP device certificate; the device private key never leaves this computer.')
    parser.add_argument('--device-id')
    parser.add_argument('--group')
    parser.add_argument('--device-model')
    parser.add_argument('--product-role')
    parser.add_argument('--hardware-revision')
    parser.add_argument('--chip-family', default='ESP32-C6')
    parser.add_argument('--flash-size', default='16MB')
    parser.add_argument('--ecosystem', default='JaroslavZemanESP')
    parser.add_argument('--ca-dir', type=Path)
    parser.add_argument('--ota-url', help='Manufacturing HTTPS base URL, e.g. https://192.168.2.120:8451')
    parser.add_argument('--out', type=Path, default=Path('device_credentials'))
    parser.add_argument('--days', type=int, default=3650)
    args = parser.parse_args()

    print('This script creates one ESP identity locally, signs it with the offline Root CA, registers only the public certificate in OTA, and prepares the public OTA trust files for CMake.')

    raw_device_id = args.device_id or ask('Device Zigbee IEEE', '20:6e:f1:ff:fe:0d:45:94')
    compact_id, device_id = normalize_device_id(raw_device_id)
    device_group = args.group or ask('Device group/family', 'remotecontrol7-encoder')
    device_model = args.device_model or ask('Device model', 'ESP32-C6-ENC')
    product_role = args.product_role or ask('Product role/function', 'six-strip-cct-led-controller')
    hardware_revision = args.hardware_revision or ask('Hardware revision', 'RevA')
    chip_family = args.chip_family or ask('Chip family', 'ESP32-C6')
    flash_size = args.flash_size or ask('Flash size', '16MB')
    ecosystem = args.ecosystem or ask('Ecosystem', 'JaroslavZemanESP')
    ca_dir = (args.ca_dir or Path(ask('Offline CA directory', '../ota_server/ca'))).expanduser().resolve()
    ota_base = args.ota_url or ask('OTA manufacturing HTTPS URL', 'https://192.168.2.120:8451')
    output_dir = args.out.expanduser().resolve()

    ca_cert, ca_key = load_ca(ca_dir)
    device_key, cert = make_device_certificate(
        ca_cert, ca_key, ecosystem, device_id, compact_id, device_group, device_model,
        product_role, hardware_revision, chip_family, flash_size, args.days,
    )

    output_dir.mkdir(parents=True, exist_ok=True)
    cert_path = output_dir / DEVICE_CERT_NAME
    key_path = output_dir / DEVICE_KEY_NAME
    root_ca_path = output_dir / ROOT_CA_CERT_NAME
    cert_pem = cert.public_bytes(serialization.Encoding.PEM)
    cert_path.write_bytes(cert_pem)
    key_path.write_bytes(device_key.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption(),
    ))
    root_ca_path.write_bytes(ca_cert.public_bytes(serialization.Encoding.PEM))
    try:
        key_path.chmod(0o600)
    except OSError:
        pass

    registration = register_with_ota(ota_base, root_ca_path, cert_pem)
    if registration.get('status') != 'REGISTERED':
        raise RuntimeError(f'OTA registration failed: {registration}')

    (output_dir / OTA_CERT_NAME).write_bytes(
        ota_get(ota_base, '/api/manufacturing/ota-server.pem', root_ca_path)
    )
    (output_dir / OTA_PUBLIC_NAME).write_bytes(
        ota_get(ota_base, '/api/manufacturing/ota-public.pem', root_ca_path)
    )

    print('\nDevice credential provisioning complete:')
    print(f'  device_id:          {device_id}')
    print(f'  group:              {device_group}')
    print(f'  model:              {device_model}')
    print(f'  product_role:       {product_role}')
    print(f'  hardware_revision:  {hardware_revision}')
    print(f'  certificate SHA256: {cert.fingerprint(hashes.SHA256()).hex()}')
    print(f'  OTA registry:       {registration.get("status")}')
    print(f'  build directory:    {output_dir}')
    print(f'  PRIVATE until flash: {key_path}')
    print('After a successful flash, delete device_private.pem from the workstation; OTA keeps only the public certificate.')


if __name__ == '__main__':
    main()
