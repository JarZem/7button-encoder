#!/usr/bin/env python3
"""Generate secure MQTT->Zigbee test vectors without running the OTA server.

The script uses the same protocol as ESP firmware:
  A1|counter|random22|crc32|auth43
  R1|counter|auth43
  P1|base64url(AES-GCM(ciphertext||tag16))

It does NOT publish MQTT. It prints JSON payloads to paste into:
  zigbee2mqtt/<ieee>/set
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import hmac
import ipaddress
import json
import os
import struct
import zlib

from cryptography import x509
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

SESSION_DOMAIN = b"JaroslavZemanESP-SESSION-v1"
CHALLENGE_DOMAIN = b"JaroslavZemanESP-CHALLENGE-v1"
RESPONSE_DOMAIN = b"JaroslavZemanESP-CHALLENGE-OK-v1"
PROVISION_KEY_DOMAIN = b"JaroslavZemanESP-PROVISION-KEY-v1"
PROVISION_NONCE_DOMAIN = b"JaroslavZemanESP-PROVISION-NONCE-v1"
PROVISION_AAD_DOMAIN = b"JaroslavZemanESP-PROVISION-AAD-v1"

SECURITY = {
    "open": 0,
    "wpa2": 1,
    "wpa3": 2,
    "wpa2_wpa3": 3,
}


def b64u(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def load_private_key(path: str):
    with open(path, "rb") as f:
        return serialization.load_pem_private_key(f.read(), password=None)


def load_cert(path: str) -> x509.Certificate:
    with open(path, "rb") as f:
        return x509.load_pem_x509_certificate(f.read())


def public_numbers_equal(private_key, cert: x509.Certificate) -> bool:
    return private_key.public_key().public_numbers() == cert.public_key().public_numbers()


def session_key(server_private, device_cert: x509.Certificate, device_id: str,
                counter: int, random16: bytes) -> bytes:
    device_public = device_cert.public_key()
    if not isinstance(server_private, ec.EllipticCurvePrivateKey) or not isinstance(device_public, ec.EllipticCurvePublicKey):
        raise SystemExit("Both OTA server key and device certificate must use EC keys")
    if not isinstance(server_private.curve, ec.SECP256R1) or not isinstance(device_public.curve, ec.SECP256R1):
        raise SystemExit("Both keys must use P-256/secp256r1")
    shared = server_private.exchange(ec.ECDH(), device_public)
    material = SESSION_DOMAIN + device_id.encode("ascii") + struct.pack(">Q", counter) + random16
    return hmac.new(shared, material, hashlib.sha256).digest()


def auth_material(domain: bytes, device_id: str, counter: int, random16: bytes, crc: int) -> bytes:
    return domain + device_id.encode("ascii") + struct.pack(">Q", counter) + random16 + struct.pack(">I", crc)


def make_challenge(server_private, device_cert, device_id: str, counter: int, random16: bytes):
    crc_input = struct.pack(">Q", counter) + random16
    crc = zlib.crc32(crc_input) & 0xFFFFFFFF
    sk = session_key(server_private, device_cert, device_id, counter, random16)
    mac = hmac.new(sk, auth_material(CHALLENGE_DOMAIN, device_id, counter, random16, crc), hashlib.sha256).digest()
    ack_mac = hmac.new(sk, auth_material(RESPONSE_DOMAIN, device_id, counter, random16, crc), hashlib.sha256).digest()
    challenge = f"A1|{counter}|{b64u(random16)}|{crc:08x}|{b64u(mac)}"
    expected_ack = f"R1|{counter}|{b64u(ack_mac)}"
    if len(challenge.encode()) > 100:
        raise SystemExit(f"Challenge wire payload is too long: {len(challenge.encode())} bytes")
    return challenge, expected_ack, sk, crc


def encode_provision_plain(ssid: str, password: str, host: str, port: int,
                           security: int, channel: int) -> bytes:
    ssid_b = ssid.encode("utf-8")
    password_b = password.encode("utf-8")
    if not 1 <= len(ssid_b) <= 32:
        raise SystemExit("SSID must encode to 1..32 bytes")
    if len(password_b) > 64:
        raise SystemExit("Password must encode to at most 64 bytes")
    if not 0 <= channel <= 14:
        raise SystemExit("WiFi channel must be 0..14")
    if not 1 <= port <= 65535:
        raise SystemExit("OTA port must be 1..65535")

    try:
        ip = ipaddress.ip_address(host)
        if ip.version != 4:
            raise ValueError
        host_type = 1
        host_b = ip.packed
    except ValueError:
        host_type = 0
        host_b = host.encode("utf-8")
        if not 1 <= len(host_b) <= 64:
            raise SystemExit("OTA host must encode to 1..64 bytes")

    return bytes([
        1,
        security,
        channel,
        len(ssid_b),
        len(password_b),
        host_type,
        len(host_b),
    ]) + ssid_b + password_b + host_b + struct.pack(">H", port)


def make_provision(sk: bytes, device_id: str, counter: int, random16: bytes, crc: int,
                   ssid: str, password: str, host: str, port: int,
                   security: int, channel: int) -> str:
    key = hmac.new(sk, PROVISION_KEY_DOMAIN, hashlib.sha256).digest()
    nonce_material = PROVISION_NONCE_DOMAIN + struct.pack(">Q", counter) + random16
    nonce = hmac.new(sk, nonce_material, hashlib.sha256).digest()[:12]
    aad = PROVISION_AAD_DOMAIN + device_id.encode("ascii") + struct.pack(">Q", counter) + random16 + struct.pack(">I", crc)
    plain = encode_provision_plain(ssid, password, host, port, security, channel)
    encrypted = AESGCM(key).encrypt(nonce, plain, aad)  # ciphertext || 16-byte tag
    wire = "P1|" + b64u(encrypted)
    if len(wire.encode()) > 100:
        raise SystemExit(
            f"Provisioning payload is {len(wire.encode())} bytes, above the proven 100-byte Zigbee limit. "
            "Use shorter SSID/password/hostname for this single-frame test."
        )
    return wire


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--ota-key", required=True, help="PEM OTA server private P-256 key")
    p.add_argument("--ota-cert", required=True, help="PEM OTA server certificate embedded in ESP")
    p.add_argument("--device-cert", required=True, help="PEM certificate of the target ESP")
    p.add_argument("--device-id", required=True, help="e.g. 20:6e:f1:ff:fe:0d:45:94")
    p.add_argument("--counter", required=True, type=int, help="Counter from the latest ESP HELLO")
    p.add_argument("--random-hex", help="Optional fixed 16-byte random as 32 hex chars; default=random")
    p.add_argument("--ssid", required=True)
    p.add_argument("--password", required=True)
    p.add_argument("--host", default="192.168.2.120")
    p.add_argument("--port", type=int, default=8443)
    p.add_argument("--security", choices=SECURITY, default="wpa2")
    p.add_argument("--channel", type=int, default=0)
    args = p.parse_args()

    server_private = load_private_key(args.ota_key)
    server_cert = load_cert(args.ota_cert)
    device_cert = load_cert(args.device_cert)
    if not public_numbers_equal(server_private, server_cert):
        raise SystemExit("OTA private key does not match OTA certificate embedded in ESP")

    random16 = bytes.fromhex(args.random_hex) if args.random_hex else os.urandom(16)
    if len(random16) != 16:
        raise SystemExit("--random-hex must contain exactly 16 bytes")

    challenge, expected_ack, sk, crc = make_challenge(
        server_private, device_cert, args.device_id, args.counter, random16
    )
    provisioning = make_provision(
        sk, args.device_id, args.counter, random16, crc,
        args.ssid, args.password, args.host, args.port,
        SECURITY[args.security], args.channel,
    )

    topic = "zigbee2mqtt/0x" + args.device_id.replace(":", "") + "/set"
    print("TOPIC:")
    print(topic)
    print("\n1) SEND CHALLENGE:")
    print(json.dumps({"ota_command": challenge}, separators=(",", ":")))
    print("\n2) ESP MUST RETURN EXACTLY:")
    print(expected_ack)
    print("\n3) ONLY AFTER THAT SEND PROVISIONING:")
    print(json.dumps({"ota_command": provisioning}, separators=(",", ":")))
    print(f"\nchallenge_bytes={len(challenge.encode())} provisioning_bytes={len(provisioning.encode())}")


if __name__ == "__main__":
    main()
