from __future__ import annotations

import base64
import hashlib
import io
import json
import tempfile
import unittest
import zipfile
from pathlib import Path

from cryptography.fernet import Fernet
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa
from fastapi.testclient import TestClient

from cloud_server.admin import hash_admin_password
from cloud_server.app import create_app
from cloud_server.config import Settings
from cloud_server.firmware import FirmwarePackageError, FirmwareRepository
from cloud_server.store import Store


HARDWARE_ID = "esp32s3-cloud-voice-v1"


class FirmwareUpdateTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.private_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        self.public_key_path = self.root / "firmware_public_key.pem"
        self.public_key_path.write_bytes(
            self.private_key.public_key().public_bytes(
                serialization.Encoding.PEM,
                serialization.PublicFormat.SubjectPublicKeyInfo,
            )
        )
        self.base_url = "https://voice.example.test"

    def tearDown(self) -> None:
        self.temp.cleanup()

    def package(
        self, version: str = "1.2.3", *, firmware: bytes | None = None, force: bool = False,
        hardware: str = HARDWARE_ID
    ) -> bytes:
        firmware = firmware or (b"\xe9" + b"A" * (65_536 - 1))
        url = f"{self.base_url}/voice-api/v1/firmware/download/{version}"
        digest = hashlib.sha256(firmware).hexdigest()
        force_text = "true" if force else "false"
        canonical = f"{version}\n{hardware}\n{len(firmware)}\n{digest}\n{url}\n{force_text}"
        signature = self.private_key.sign(
            canonical.encode("utf-8"), padding.PKCS1v15(), hashes.SHA256()
        )
        manifest = {
            "version": version,
            "hardware": hardware,
            "size": len(firmware),
            "sha256": digest,
            "url": url,
            "signature": base64.b64encode(signature).decode("ascii"),
            "force": force,
        }
        output = io.BytesIO()
        with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as package:
            package.writestr("manifest.json", json.dumps(manifest))
            package.writestr("firmware.bin", firmware)
        return output.getvalue()

    def repository(self) -> FirmwareRepository:
        return FirmwareRepository(
            self.root / "firmware",
            self.public_key_path,
            self.base_url,
            HARDWARE_ID,
        )

    def test_publish_valid_signed_package_atomically(self) -> None:
        release = self.repository().publish_package(self.package())
        self.assertEqual(release.version, "1.2.3")
        self.assertEqual(release.size, 65_536)
        self.assertEqual(self.repository().current().version, "1.2.3")
        self.assertEqual(self.repository().firmware_path("1.2.3").read_bytes()[0], 0xE9)

    def test_rejects_tampered_or_structurally_unsafe_packages(self) -> None:
        payload = self.package(firmware=b"\xe9" + b"B" * (65_536 - 1))
        with zipfile.ZipFile(io.BytesIO(payload)) as source:
            manifest = source.read("manifest.json")
        output = io.BytesIO()
        with zipfile.ZipFile(output, "w") as package:
            package.writestr("manifest.json", manifest)
            package.writestr("firmware.bin", b"\xe9" + b"C" * (65_536 - 1))
        with self.assertRaises(FirmwarePackageError):
            self.repository().publish_package(output.getvalue())

        output = io.BytesIO()
        with zipfile.ZipFile(output, "w") as package:
            package.writestr("../manifest.json", manifest)
            package.writestr("firmware.bin", b"x" * 65_536)
        with self.assertRaises(FirmwarePackageError):
            self.repository().publish_package(output.getvalue())

    def test_rejects_downgrade_and_force_does_not_repeat_current_version(self) -> None:
        repository = self.repository()
        repository.publish_package(self.package("1.2.3", force=True))
        self.assertIsNone(repository.manifest_for("1.2.3", HARDWARE_ID))
        self.assertIsNone(repository.manifest_for("1.2.4", HARDWARE_ID))
        self.assertEqual(repository.manifest_for("1.2.2", HARDWARE_ID)["version"], "1.2.3")
        with self.assertRaises(FirmwarePackageError):
            repository.publish_package(self.package("1.2.2"))

    def test_device_manifest_and_download_require_auth_and_filter_versions(self) -> None:
        settings = Settings.for_test(
            self.root,
            firmware_public_key_path=self.public_key_path,
            firmware_public_base_url=self.base_url,
            firmware_hardware_id=HARDWARE_ID,
        )
        repository = FirmwareRepository(
            self.root / "firmware", self.public_key_path, self.base_url, HARDWARE_ID
        )
        repository.publish_package(self.package())
        store = Store(settings.database_path, settings.auth_pepper)
        token = store.register_device("esp32-test-01", "test", "ota-token")
        with TestClient(create_app(settings)) as client:
            self.assertEqual(client.get("/voice-api/v1/firmware/manifest").status_code, 401)
            headers = {
                "X-Device-ID": "esp32-test-01",
                "Authorization": f"Bearer {token}",
                "X-Firmware-Version": "1.0.0",
                "X-Hardware-ID": HARDWARE_ID,
            }
            manifest = client.get("/voice-api/v1/firmware/manifest", headers=headers)
            self.assertEqual(manifest.status_code, 200, manifest.text)
            self.assertEqual(manifest.json()["version"], "1.2.3")
            current = client.get(
                "/voice-api/v1/firmware/manifest",
                headers={**headers, "X-Firmware-Version": "1.2.3"},
            )
            self.assertEqual(current.status_code, 204)
            download = client.get("/voice-api/v1/firmware/download/1.2.3", headers=headers)
            self.assertEqual(download.status_code, 200)
            self.assertEqual(len(download.content), 65_536)
            self.assertEqual(download.headers["cache-control"], "private, no-store")

    def test_two_hardware_channels_do_not_mix_same_version_images(self):
        multi = "esp32s3-cloud-voice-multimic-v1"
        settings = Settings.for_test(self.root, firmware_public_key_path=self.public_key_path,
            firmware_public_base_url=self.base_url, firmware_hardware_id=HARDWARE_ID,
            admin_password_hash=hash_admin_password("test-admin-password"),
            config_encryption_key=Fernet.generate_key().decode("ascii"))
        with TestClient(create_app(settings)) as client:
            client.app.state.firmware_repository.publish_package(self.package("2.0.0"))
            multi_image = b"\xe9" + b"M" * 65535
            multi_package = self.package("2.0.0", hardware=multi, firmware=multi_image)
            token = client.app.state.store.register_device("multi-device", "multi", "multi-token")
            unlocked = client.post('/admin/unlock', data={'password':'test-admin-password'}, follow_redirects=True)
            csrf = unlocked.text.split('name="csrf" value="',1)[1].split('"',1)[0]
            response = client.post('/admin/firmware/upload', data={'csrf':csrf,'channel':'multi'},
                files={'package':('multi.zip',multi_package,'application/zip')})
            self.assertEqual(response.status_code,200,response.text)
            headers = {'X-Device-ID':'multi-device','Authorization':f'Bearer {token}',
                'X-Hardware-ID':multi,'X-Firmware-Version':'1.4.3'}
            self.assertEqual(client.get('/voice-api/v1/firmware/manifest',headers=headers).json()['hardware'],multi)
            self.assertEqual(client.get('/voice-api/v1/firmware/download/2.0.0',headers=headers).content,multi_image)
            single = client.get('/voice-api/v1/firmware/download/2.0.0',headers={**headers,'X-Hardware-ID':HARDWARE_ID})
            self.assertEqual(single.content,b"\xe9" + b"A" * 65535)
            self.assertEqual(client.get('/admin/firmware/package/2.0.0?channel=multi').content,multi_package)
            page = client.get('/admin/firmware').text
            self.assertIn('name="channel" value="multi"',page)
            self.assertIn('?channel=multi',page)

    def test_admin_can_upload_signed_package(self) -> None:
        settings = Settings.for_test(
            self.root,
            firmware_public_key_path=self.public_key_path,
            firmware_public_base_url=self.base_url,
            firmware_hardware_id=HARDWARE_ID,
            admin_password_hash=hash_admin_password("test-admin-password"),
            config_encryption_key=Fernet.generate_key().decode("ascii"),
        )
        with TestClient(create_app(settings)) as client:
            unlocked = client.post(
                "/admin/unlock",
                data={"password": "test-admin-password"},
                follow_redirects=True,
            )
            csrf = unlocked.text.split('name="csrf" value="', 1)[1].split('"', 1)[0]
            uploaded = client.post(
                "/admin/firmware/upload",
                data={"csrf": csrf},
                files={"package": ("ota-1.2.3.zip", self.package(), "application/zip")},
            )
            self.assertEqual(uploaded.status_code, 200, uploaded.text)
            self.assertIn("1.2.3", uploaded.text)
            self.assertEqual(client.app.state.firmware_repository.current().version, "1.2.3")


if __name__ == "__main__":
    unittest.main()
