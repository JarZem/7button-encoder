import argparse
import hashlib
import json
import os


REQUIRED_FIELDS = (
    "ota_ecosystem",
    "device_model",
    "product_role",
    "firmware_product",
    "hardware_revision",
    "chip_family",
    "flash_size",
    "firmware_version",
    "firmware_channel",
)


def sha256_file(path):
    sha = hashlib.sha256()
    with open(path, "rb") as handle:
        while True:
            chunk = handle.read(1024 * 1024)
            if not chunk:
                break
            sha.update(chunk)
    return sha.hexdigest()


def main():
    parser = argparse.ArgumentParser(
        description="Create OTA release metadata next to an ESP-IDF .bin image."
    )
    parser.add_argument("--project", default="device.project.json")
    parser.add_argument("--bin", required=True, dest="bin_path")
    parser.add_argument("--output")
    args = parser.parse_args()

    with open(args.project, "r", encoding="utf-8") as handle:
        metadata = json.load(handle)

    missing = [field for field in REQUIRED_FIELDS if not metadata.get(field)]
    if missing:
        raise SystemExit("Missing release metadata fields: " + ", ".join(missing))

    bin_path = os.path.abspath(args.bin_path)
    if not os.path.isfile(bin_path):
        raise SystemExit("BIN file not found: " + bin_path)

    release = dict(metadata)
    release["filename"] = os.path.basename(bin_path)
    release["size"] = os.path.getsize(bin_path)
    release["sha256"] = sha256_file(bin_path)

    output = args.output
    if not output:
        base, _ = os.path.splitext(bin_path)
        output = base + ".release.json"

    with open(output, "w", encoding="utf-8") as handle:
        json.dump(release, handle, indent=2, sort_keys=True)
        handle.write("\n")

    print(output)


if __name__ == "__main__":
    main()
