"""The host bootstrap generator must match the fixed HIPC request ABI."""
import struct
import unittest

from tools.provisioner.create_provisioning import PAYLOAD_SIZE, build_payload


class ProvisioningPayloadTests(unittest.TestCase):
    def test_binary_layout_matches_sysmodule_struct(self):
        payload = build_payload(
            "portal.example.net",
            "switch-livingroom",
            "AbCdEf0123456789_-abcdefgh",
        )
        self.assertEqual(len(payload), 448)
        host, device_id, code = struct.unpack("<254s65s129s", payload)
        self.assertEqual(host.split(b"\0", 1)[0], b"portal.example.net")
        self.assertEqual(device_id.split(b"\0", 1)[0], b"switch-livingroom")
        self.assertEqual(code.split(b"\0", 1)[0], b"AbCdEf0123456789_-abcdefgh")
        self.assertEqual(PAYLOAD_SIZE, 448)

    def test_rejects_scheme_port_path_and_invalid_identifiers(self):
        invalid = [
            ("https://portal.example.net", "switch-01", "a" * 24),
            ("portal.example.net:8080", "switch-01", "a" * 24),
            ("portal.example.net/path", "switch-01", "a" * 24),
            ("portal.example.net", "../switch", "a" * 24),
            ("portal.example.net", "switch-01", "short"),
        ]
        for args in invalid:
            with self.subTest(args=args), self.assertRaises(ValueError):
                build_payload(*args)

    def test_rejects_non_ascii(self):
        with self.assertRaises(ValueError):
            build_payload("portal.example.net", "switch-01", "ä" * 24)


if __name__ == "__main__":
    unittest.main()
