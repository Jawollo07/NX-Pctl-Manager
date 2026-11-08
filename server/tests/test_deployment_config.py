"""Deployment configuration must fail closed when secrets are missing."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class DeploymentConfigTests(unittest.TestCase):
    def test_compose_requires_external_secrets_and_loopback_binding(self):
        compose = (ROOT / "server" / "compose.yaml").read_text(encoding="utf-8")
        self.assertIn("env_file:", compose)
        self.assertIn("- .env", compose)
        self.assertIn('127.0.0.1:8080:8080', compose)
        self.assertIn("read_only: true", compose)
        self.assertIn("no-new-privileges:true", compose)

    def test_secret_template_is_non_secret_and_available(self):
        template = ROOT / "server" / ".env.example"
        self.assertTrue(template.is_file())
        content = template.read_text(encoding="utf-8")
        self.assertIn("NXPCTL_ADMIN_TOKEN=replace-", content)
        self.assertIn("NXPCTL_PARENT_AUTH_KEY=replace-", content)
        self.assertNotIn("NXPCTL_DATABASE=", content)

    def test_build_script_never_updates_the_callers_checkout(self):
        script = (ROOT / "scripts" / "build-sysmodule-local.sh").read_text(encoding="utf-8")
        self.assertIn('ATMOSPHERE_ROOT:-', script)
        self.assertNotIn("git pull", script)
        self.assertNotIn("git reset", script)
        self.assertIn("Expected non-empty NSP", script)


if __name__ == "__main__":
    unittest.main()
