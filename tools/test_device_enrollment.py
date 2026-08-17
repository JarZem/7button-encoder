from __future__ import annotations

import hashlib
import hmac
import time
import unittest

from device_enrollment import (
    ChallengeStore,
    canonical_enrollment,
    derive_device_auth_secret,
    enrollment_hmac,
    fixed_test_vector,
    normalize_device_id,
)


class DeviceEnrollmentTests(unittest.TestCase):
    def test_fixed_vector(self) -> None:
        master, fields, secret_hex, canonical_sha, auth_hmac = fixed_test_vector()
        self.assertEqual(secret_hex, "431edec87e617661b100ab27c0799b9dd63e6489e24b0061702c81dac0f0edb7")
        self.assertEqual(canonical_sha, "112c72bf9dfc5258f8de3382afc2c4e283e2c3d43993ac2eebc66e390b135423")
        self.assertEqual(auth_hmac, "d483b2e15530f666e492bd10679ac1120ef45bb17f43f51ec46dfcb058aeb9ca")

        secret = derive_device_auth_secret(master, fields["device_id"])
        self.assertTrue(hmac.compare_digest(bytes.fromhex(auth_hmac), enrollment_hmac(secret, fields)))
        self.assertEqual(hashlib.sha256(canonical_enrollment(fields)).hexdigest(), canonical_sha)

    def test_modified_payload_rejects(self) -> None:
        master, fields, _, _, auth_hmac = fixed_test_vector()
        modified = dict(fields)
        modified["firmware_version"] = "2"
        secret = derive_device_auth_secret(master, fields["device_id"])
        self.assertFalse(hmac.compare_digest(bytes.fromhex(auth_hmac), enrollment_hmac(secret, modified)))

    def test_two_identities_have_different_auth_secrets(self) -> None:
        master = bytes.fromhex("10" * 32)
        a = derive_device_auth_secret(master, "20:6e:f1:ff:fe:0d:45:94")
        b = derive_device_auth_secret(master, "20:6e:f1:ff:fe:0d:45:95")
        self.assertNotEqual(a, b)

    def test_challenge_is_single_use_and_bound(self) -> None:
        store = ChallengeStore(ttl_seconds=60)
        challenge = store.create("0x206ef1fffe0d4594")
        self.assertEqual(challenge.device_id, "20:6e:f1:ff:fe:0d:45:94")
        self.assertIsNone(store.consume("20:6e:f1:ff:fe:0d:45:95", challenge.message_id, challenge.challenge.hex()))
        self.assertIsNone(store.consume("20:6e:f1:ff:fe:0d:45:94", challenge.message_id, challenge.challenge.hex()))

        challenge = store.create("20:6e:f1:ff:fe:0d:45:94")
        self.assertIsNotNone(store.consume("20:6e:f1:ff:fe:0d:45:94", challenge.message_id, challenge.challenge.hex()))
        self.assertIsNone(store.consume("20:6e:f1:ff:fe:0d:45:94", challenge.message_id, challenge.challenge.hex()))

    def test_expired_challenge_rejects(self) -> None:
        store = ChallengeStore(ttl_seconds=1)
        challenge = store.create("20:6e:f1:ff:fe:0d:45:94")
        challenge.expires_at = time.time() - 1
        self.assertIsNone(store.consume("20:6e:f1:ff:fe:0d:45:94", challenge.message_id, challenge.challenge.hex()))

    def test_device_id_normalization(self) -> None:
        self.assertEqual(normalize_device_id("0x206EF1FFFE0D4594"), "20:6e:f1:ff:fe:0d:45:94")


if __name__ == "__main__":
    unittest.main()
