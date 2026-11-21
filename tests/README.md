# OTA Integration Tests

These tests verify the OTA firmware download API functionality in a real environment against a running OTA server.

## Architecture

- **No mocking** - Tests use real HTTPS communication
- **No test endpoints** - Uses production API only
- **Container-native** - Designed to run inside OTA add-on where `/data/ota_token_secret.bin` is available
- **Shared token logic** - Uses `ota_helper.py` (same as server.py)

## Running Tests in OTA Add-on

### Integration with Home Assistant OTA Add-on

To add tests to the OTA add-on, copy `tests/` to the OTA add-on repository:

```bash
# In OTA add-on repo (e.g., ha-addon-esp-ota)
cp -r tests/ /path/to/ha-addon-esp-ota/
cp ota_helper.py /path/to/ha-addon-esp-ota/
```

Then run inside the container:

```bash
# From within running OTA add-on container
python3 /share/esp_ota/tests/run_all.py
```

Or add to container startup script:

```bash
#!/bin/bash
mkdir -p /share/esp_ota/firmware
python3 /server.py &
python3 /share/esp_ota/tests/run_all.py
```

### Local Development (Without Container)

If testing locally outside the container, copy token secret for testing:

```bash
# From OTA container or volume mount
docker cp ha-addon-container:/data/ota_token_secret.bin ./ota_token_secret.bin

# Set env var and run test
export OTA_TOKEN_SECRET_PATH=./ota_token_secret.bin
export OTA_SERVER_HOST=localhost
export OTA_SERVER_PORT=8443
python3 tests/run_all.py
```

## Test Cases

### OTA_FIRMWARE_DOWNLOAD
- Finds firmware BIN locally
- Loads release metadata (SHA256, size)
- Discovers firmware code by probing server
- Creates valid download token
- Downloads firmware via HTTPS
- Verifies HTTP 200, size, SHA256
- Checks `X-Firmware-SHA256` response header

### OTA_INVALID_TOKEN_REJECTED
- Attempts download with invalid token
- Verifies HTTP 401 rejection

### OTA_MISSING_DEVICE_ID_REJECTED
- Attempts download without required `X-Device-ID` header
- Verifies HTTP 401 rejection

## Configuration

The test auto-detects the environment:

### Container Mode (Default)
- Uses `/share/esp_ota/firmware/remotecontrol7andEncoder.bin`
- Uses `/data/ota_token_secret.bin` (shared with server)
- Connects to `localhost:8443`

### Local Development Mode
Override with environment variables:

```bash
export OTA_SERVER_HOST=192.168.1.100
export OTA_SERVER_PORT=8443
export OTA_PROJECT_DIR=/path/to/remotecontrol7andEncoder
export OTA_TOKEN_SECRET_PATH=/path/to/ota_token_secret.bin
python3 tests/run_all.py
```

## Expected Output

```
======================================================================
OTA Integration Tests
======================================================================

OTA Test Config (CONTAINER):
  Server: https://localhost:8443
  Firmware: /share/esp_ota/firmware/remotecontrol7andEncoder.bin
  Metadata: /share/esp_ota/firmware/remotecontrol7andEncoder.release.json
  CA Cert: /share/esp_ota/cert/ota-ca.crt
  Token Secret: /data/ota_token_secret.bin

✓ All prerequisites met

[PASS] OTA_FIRMWARE_DOWNLOAD
    code: ABC
    size: 1266496
    sha256: 0d567d08fa...
[PASS] OTA_INVALID_TOKEN_REJECTED
[PASS] OTA_MISSING_DEVICE_ID_REJECTED

Tests: 3 passed, 0 failed
```

## Test Files

- `run_all.py` - Main test runner
- `.env.example` - Configuration template (for local dev)
- `ota_helper.py` (parent dir) - Shared token/firmware logic

## Notes

- Tests use `ota_helper.py` - the same token logic as production server
- No secrets are hard-coded or logged
- Token validation uses current + previous time bucket (production behavior)
- Device ID is hardcoded test value (`aa:bb:cc:dd:ee:ff:00:11`)
- SSL certificate verification is disabled for localhost testing
- Tests do not modify database or firmware registry

