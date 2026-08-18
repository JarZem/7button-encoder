#!/usr/bin/env python3

from __future__ import annotations

import argparse
import getpass
from datetime import datetime, timedelta, timezone
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID


def normalize_device_id(value: str) -> str:
    compact = value.strip().lower().replace("0x", "").replace(":", "")
    if len(compact) != 16 or any(ch not in "0123456789abcdef" for ch in compact):
        raise ValueError("device id must be an 8-byte IEEE address")
    return compact


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Create one CA-signed ESP device certificate and matching P-256 private key for embedding into flash."
    )
    parser.add_argument("--device-id", required=True, help="Zigbee IEEE address, e.g. 20:6e:f1:ff:fe:0d:45:94")
    parser.add_argument("--ca-cert", required=True, type=Path)
    parser.add_argument("--ca-key", required=True, type=Path)
    parser.add_argument("--ecosystem", default="JaroslavZemanESP")
    parser.add_argument("--out", type=Path, default=Path("device_credentials"))
    parser.add_argument("--days", type=int, default=3650)
    args = parser.parse_args()

    compact = normalize_device_id(args.device_id)
    ca_cert = x509.load_pem_x509_certificate(args.ca_cert.read_bytes())

    password_text = getpass.getpass("CA private key password (leave empty if unencrypted): ")
    password = password_text.encode("utf-8") if password_text else None
    ca_key = serialization.load_pem_private_key(args.ca_key.read_bytes(), password=password)

    device_key = ec.generate_private_key(ec.SECP256R1())
    subject = x509.Name([
        x509.NameAttribute(NameOID.COMMON_NAME, f"ESP-{compact}"),
        x509.NameAttribute(NameOID.ORGANIZATION_NAME, args.ecosystem),
    ])
    now = datetime.now(timezone.utc)
    san_uri = f"urn:{args.ecosystem.lower()}:esp:pki:device:{compact}"

    cert = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(ca_cert.subject)
        .public_key(device_key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - timedelta(minutes=5))
        .not_valid_after(now + timedelta(days=args.days))
        .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
        .add_extension(x509.KeyUsage(
            digital_signature=True,
            content_commitment=False,
            key_encipherment=False,
            data_encipherment=False,
            key_agreement=False,
            key_cert_sign=False,
            crl_sign=False,
            encipher_only=False,
            decipher_only=False,
        ), critical=True)
        .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.CLIENT_AUTH]), critical=False)
        .add_extension(x509.SubjectAlternativeName([x509.UniformResourceIdentifier(san_uri)]), critical=False)
        .sign(ca_key, hashes.SHA256())
    )

    args.out.mkdir(parents=True, exist_ok=True)
    cert_path = args.out / "device_cert.pem"
    key_path = args.out / "device_private.pem"

    cert_path.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    key_path.write_bytes(device_key.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption(),
    ))

    print(f"Created {cert_path}")
    print(f"Created {key_path}")
    print(f"Device SAN: {san_uri}")
    print(f"Certificate SHA256: {cert.fingerprint(hashes.SHA256()).hex()}")


if __name__ == "__main__":
    main()
