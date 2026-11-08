"""The config generator emits the exact text format consumed by the bootstrap."""
import unittest

from tools.provisioner.create_provisioning import build_config


class ProvisioningConfigTests(unittest.TestCase):
    def test_generates_readable_config(self):
        config = build_config(
            "portal.example.net",
            "switch-livingroom",
            "AbCdEf0123456789_-abcdefgh",
        )
        self.assertEqual(
            config,
            "# NX-Pctl-Manager one-time device enrollment\n"
            "api_host=portal.example.net\n"
            "device_id=switch-livingroom\n"
            "enrollment_code=AbCdEf0123456789_-abcdefgh\n",
        )

    def test_rejects_scheme_port_path_and_invalid_identifiers(self):
        invalid = [
            ("https://portal.example.net", "switch-01", "a" * 24),
            ("portal.example.net:8080", "switch-01", "a" * 24),
            ("portal.example.net/path", "switch-01", "a" * 24),
            ("bad..host", "switch-01", "a" * 24),
            ("-bad.host", "switch-01", "a" * 24),
            ("bad-.host", "switch-01", "a" * 24),
            ("bad.-host", "switch-01", "a" * 24),
            ("portal.example.net", "../switch", "a" * 24),
            ("portal.example.net", "switch-01", "short"),
        ]
        for args in invalid:
            with self.subTest(args=args), self.assertRaises(ValueError):
                build_config(*args)

    def test_rejects_non_ascii(self):
        with self.assertRaises(ValueError):
            build_config("portal.example.net", "switch-01", "ä" * 24)


if __name__ == "__main__":
    unittest.main()
