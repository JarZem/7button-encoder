#!/usr/bin/env python3
"""
Install/update the ESP OTA Home Assistant add-on over SSH.

The script intentionally does not contain site secrets. Pass runtime options
with --options-json or set them later in the Home Assistant add-on UI.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import paramiko


ADDON_FILES = (
    "server.py",
    "config.yaml",
    "Dockerfile",
    "run.sh",
    "reinstall.sh",
)


def ssh_client(host: str, user: str, key: Path) -> paramiko.SSHClient:
    ssh_key = paramiko.Ed25519Key.from_private_key_file(str(key))
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(hostname=host, username=user, pkey=ssh_key, timeout=15)
    return client


def run(client: paramiko.SSHClient, command: str, timeout: int = 180) -> str:
    _stdin, stdout, stderr = client.exec_command(command, timeout=timeout)
    out = stdout.read().decode("utf-8", "replace")
    err = stderr.read().decode("utf-8", "replace")
    rc = stdout.channel.recv_exit_status()
    if rc != 0:
        raise RuntimeError(f"Command failed ({rc}): {command}\n{out}\n{err}")
    return out


def supervisor_set_options(client: paramiko.SSHClient, options: dict) -> None:
    payload = json.dumps({"options": options}, separators=(",", ":"))
    remote = (
        "python3 - <<'PY'\n"
        "import json, os, urllib.request\n"
        "payload = '''" + payload.replace("\\", "\\\\").replace("'", "\\'") + "'''\n"
        "req = urllib.request.Request(\n"
        "    'http://supervisor/addons/local_esp_ota/options',\n"
        "    data=payload.encode(),\n"
        "    method='POST',\n"
        "    headers={\n"
        "        'Authorization': 'Bearer ' + os.environ['SUPERVISOR_TOKEN'],\n"
        "        'Content-Type': 'application/json',\n"
        "    },\n"
        ")\n"
        "with urllib.request.urlopen(req, timeout=10) as r:\n"
        "    r.read()\n"
        "PY"
    )
    run(client, remote)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True, help="Home Assistant SSH host")
    parser.add_argument("--user", default="root")
    parser.add_argument(
        "--key",
        default=str(Path.home() / ".ssh" / "id_ed25519"),
        help="ED25519 private key for Home Assistant SSH",
    )
    parser.add_argument(
        "--source",
        required=True,
        help="Local esp_ota add-on source directory",
    )
    parser.add_argument(
        "--options-json",
        help="JSON file with add-on options. Omit to keep current/default options.",
    )
    args = parser.parse_args()

    source = Path(args.source)
    if not source.is_dir():
        raise SystemExit(f"Source directory not found: {source}")

    missing = [name for name in ADDON_FILES if not (source / name).is_file()]
    if missing:
        raise SystemExit(f"Missing add-on files: {', '.join(missing)}")

    client = ssh_client(args.host, args.user, Path(args.key))
    try:
        run(client, "mkdir -p /addons/esp_ota /share/esp_ota/firmware /share/esp_ota/cert")

        sftp = client.open_sftp()
        try:
            for name in ADDON_FILES:
                sftp.put(str(source / name), f"/addons/esp_ota/{name}")
        finally:
            sftp.close()

        run(client, "ha store reload", timeout=180)
        info_text = run(client, "ha apps info local_esp_ota --raw-json 2>/dev/null || true")
        if '"slug":"local_esp_ota"' in info_text and '"state":"unknown"' not in info_text:
            try:
                info = json.loads(info_text).get("data", {})
            except json.JSONDecodeError:
                info = {}
            if info.get("update_available"):
                run(client, "ha apps update local_esp_ota", timeout=300)
            else:
                run(client, "ha apps rebuild local_esp_ota", timeout=300)
        else:
            run(client, "ha apps install local_esp_ota", timeout=300)

        if args.options_json:
            with open(args.options_json, "r", encoding="utf-8") as f:
                supervisor_set_options(client, json.load(f))

        run(client, "ha apps restart local_esp_ota", timeout=120)
        print("ESP OTA add-on deployed and restarted.")
    finally:
        client.close()


if __name__ == "__main__":
    main()
