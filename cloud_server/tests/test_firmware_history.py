from __future__ import annotations

import io
import json
import unittest
import zipfile

from cryptography.fernet import Fernet
from fastapi.testclient import TestClient

from cloud_server.admin import hash_admin_password
from cloud_server.app import create_app
from cloud_server.config import Settings
from cloud_server.tests.test_firmware_update import FirmwareUpdateTest, HARDWARE_ID


class FirmwareHistoryTest(unittest.TestCase):
    setUp = FirmwareUpdateTest.setUp
    tearDown = FirmwareUpdateTest.tearDown
    package = FirmwareUpdateTest.package
    repository = FirmwareUpdateTest.repository

    def test_old_versions_and_original_packages_are_retained(self):
        repository = self.repository()
        older = self.package("1.2.9")
        repository.publish_package(older)
        repository.publish_package(self.package("1.2.10"))
        self.assertEqual([r.version for r in repository.list_releases()], ["1.2.10", "1.2.9"])
        self.assertEqual(repository.package_bytes("1.2.9"), older)
        self.assertEqual(repository.current().version, "1.2.10")
        self.assertIsNone(repository.package_bytes("../current"))

    def test_legacy_packages_can_be_downloaded_without_changing_current(self):
        repository = self.repository()
        repository.publish_package(self.package("1.2.9"))
        (repository.root / "releases/1.2.9/package.zip").unlink(missing_ok=True)
        repository.publish_package(self.package("1.2.10"))
        with zipfile.ZipFile(io.BytesIO(repository.package_bytes("1.2.9"))) as archive:
            self.assertEqual(set(archive.namelist()), {"manifest.json", "firmware.bin"})
            self.assertEqual(json.loads(archive.read("manifest.json"))["version"], "1.2.9")
        self.assertEqual(repository.current().version, "1.2.10")

    def test_history_requires_login_and_lists_notes_and_downloads(self):
        settings = Settings.for_test(
            self.root, firmware_public_key_path=self.public_key_path,
            firmware_public_base_url=self.base_url, firmware_hardware_id=HARDWARE_ID,
            admin_password_hash=hash_admin_password("test-admin-password"),
            config_encryption_key=Fernet.generate_key().decode("ascii"),
        )
        with TestClient(create_app(settings)) as client:
            self.assertNotEqual(client.get("/admin/firmware/package/1.2.9").status_code, 200)
            unlocked = client.post("/admin/unlock", data={"password": "test-admin-password"})
            home = client.get("/admin/")
            csrf = home.text.split('name="csrf" value="', 1)[1].split('"', 1)[0]
            for version in ("1.2.9", "1.2.10"):
                result = client.post("/admin/firmware/upload", data={"csrf": csrf, "notes": '<script>alert(1)</script> Noise fix'},
                                     files={"package": ("ota.zip", self.package(version), "application/zip")})
                self.assertEqual(result.status_code, 200, result.text)
            page = client.get("/admin/firmware")
            self.assertEqual(page.status_code, 200)
            self.assertIn("1.2.9", page.text)
            self.assertIn("1.2.10", page.text)
            self.assertIn("Noise fix", page.text)
            self.assertNotIn("<script>alert(1)</script>", page.text)
            self.assertEqual(client.get("/admin/firmware/package/1.2.9").status_code, 200)
            self.assertEqual(client.app.state.firmware_repository.current().version, "1.2.10")

    def test_partition_compatibility_and_archive_do_not_offer_wrong_upgrade(self):
        repository = self.repository()
        repository.publish_package(self.package("1.4.2"), minimum_version="1.4.1")
        repository.publish_package(self.package("1.2.9"), activate=False)
        self.assertEqual(repository.current().version, "1.4.2")
        self.assertIsNone(repository.manifest_for("1.2.9", HARDWARE_ID))
        self.assertIsNotNone(repository.manifest_for("1.4.1", HARDWARE_ID))
        record = repository.root / "releases/1.2.9/release.json"
        values = json.loads(record.read_text())
        values["withdrawn"] = True
        record.write_text(json.dumps(values))
        self.assertEqual(len(repository.list_releases()), 2)
        self.assertIsNone(repository.package_bytes("1.2.9"))


if __name__ == "__main__":
    unittest.main()
