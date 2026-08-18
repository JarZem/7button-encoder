This directory is a LOCAL BUILD WORKSPACE for one ESP device.

Generate it with:
  python tools/create_device_credentials.py

Expected generated files:
  device_private.pem      PRIVATE; embed into the target ESP flash, never commit or upload to OTA.
  device_cert.pem         PUBLIC CA-signed identity of this ESP; the generator registers it with OTA.
  root_ca_cert.pem        PUBLIC offline Root CA certificate.
  ota_server_cert.pem     PUBLIC OTA server certificate fetched from OTA manufacturing HTTPS API.
  ota_server_public.pem   PUBLIC OTA signing/TLS public key fetched from OTA.

CMake requires the first four PEM files and embeds them into the firmware image.
After the device has been flashed successfully, delete device_private.pem from this workstation or run:
  python tools/cleanup_device_credentials.py

All generated files in this directory except this README are ignored by Git.
