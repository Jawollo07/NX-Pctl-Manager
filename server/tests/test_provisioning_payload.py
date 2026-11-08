"""The config generator emits the exact text format consumed by the bootstrap."""
import unittest

from tools.provisioner.create_provisioning import build_config


class ProvisioningConfigTests(unittest.TestCase):
    def test_generates_config_with_full_ipv4_url_and_port(self):
        config = build_config(
            "http://192.168.178.10:1212",
            "switch-livingroom",
            "AbCdEf0123456789_-abcdefgh",
        )
        self.assertEqual(
            config,
            "# NX-Pctl-Manager one-time device enrollment\n"
            "api_url=http://192.168.178.10:1212\n"
            "device_id=switch-livingroom\n"
            "enrollment_code=AbCdEf0123456789_-abcdefgh\n",
        )

    def test_accepts_hostname_and_default_http_port(self):
        config = build_config("http://portal.example.net", "switch-01", "a" * 24)
        self.assertIn("api_url=http://portal.example.net\n", config)

    def test_rejects_unsupported_or_non_base_urls(self):
        invalid = [
            "portal.example.net",
            "https://portal.example.net",
            "ftp://portal.example.net",
            "http://user:pass@portal.example.net",
            "http://portal.example.net:0",
            "http://portal.example.net:65536",
            "http://portal.example.net/path",
            "http://portal.example.net?x=1",
            "http://portal.example.net#frag",
            "http://",
        ]
        for url in invalid:
            with self.subTest(url=url), self.assertRaises(ValueError):
                build_config(url, "switch-01", "a" * 24)

    def test_rejects_invalid_identifiers_and_non_ascii(self):
        for args in [
            ("http://portal.example.net", "../switch", "a" * 24),
            ("http://portal.example.net", "switch-01", "short"),
            ("http://portal.example.net", "switch-01", "ä" * 24),
        ]:
            with self.subTest(args=args), self.assertRaises(ValueError):
                build_config(*args)


if __name__ == "__main__":
    unittest.main()
