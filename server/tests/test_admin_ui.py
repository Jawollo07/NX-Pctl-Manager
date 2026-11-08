"""Regression tests for the same-origin administration dashboard."""
import unittest
from pathlib import Path

from fastapi import HTTPException

from server import admin_ui


class AdminDashboardTests(unittest.TestCase):
    def test_dashboard_is_no_store_and_has_restrictive_csp(self):
        response = admin_ui.admin_dashboard()
        self.assertTrue(Path(response.path).is_file())
        self.assertEqual(response.headers["cache-control"], "no-store")
        policy = response.headers["content-security-policy"]
        self.assertIn("default-src 'self'", policy)
        self.assertIn("frame-ancestors 'none'", policy)
        self.assertNotIn("'unsafe-inline'", policy)

    def test_only_known_static_assets_are_served(self):
        for asset in ("admin.css", "admin.js"):
            response = admin_ui.admin_asset(asset)
            self.assertTrue(Path(response.path).is_file())
            self.assertEqual(response.headers["cache-control"], "no-store")
            self.assertEqual(response.headers["x-content-type-options"], "nosniff")
        with self.assertRaises(HTTPException) as error:
            admin_ui.admin_asset("../app.py")
        self.assertEqual(error.exception.status_code, 404)


if __name__ == "__main__":
    unittest.main()
