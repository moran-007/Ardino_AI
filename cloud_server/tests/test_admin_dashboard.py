from __future__ import annotations

import re
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from cryptography.fernet import Fernet
from fastapi.testclient import TestClient

from cloud_server.admin import hash_admin_password
from cloud_server.app import create_app
from cloud_server.config import Settings


class AdminDashboardTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.settings = Settings.for_test(
            Path(self.temp.name),
            admin_password_hash=hash_admin_password("dashboard-test-password"),
            config_encryption_key=Fernet.generate_key().decode("ascii"),
            deepseek_api_key="test-environment-key",
        )
        self.client = TestClient(create_app(self.settings))
        unlocked = self.client.post(
            "/admin/unlock", data={"password": "dashboard-test-password"}
        )
        self.csrf = re.search(r'name="csrf" value="([^"]+)"', unlocked.text).group(1)

    def tearDown(self):
        self.client.close()
        self.temp.cleanup()

    def api_form(self, **overrides):
        return {
            "csrf": self.csrf,
            "primary_provider": "qwen",
            "secondary_provider": "deepseek",
            "tertiary_provider": "",
            **{
                field: getattr(self.settings, field)
                for name in ("deepseek", "qwen", "glm")
                for field in (f"{name}_base_url", f"{name}_model")
            },
            **overrides,
        }

    def test_separate_pages_and_protected_routes(self):
        home = self.client.get("/admin/")
        self.assertIn("Token 用量", home.text)
        self.assertNotIn('name="deepseek_api_key"', home.text)
        for path, marker in (
            ("/admin/devices", "创建设备凭证"),
            ("/admin/api-keys", 'name="primary_provider"'),
            ("/admin/settings", "保存并重载"),
        ):
            response = self.client.get(path)
            self.assertEqual(response.status_code, 200)
            self.assertIn(marker, response.text)
            self.assertNotIn("test-environment-key", response.text)
            self.assertEqual(response.headers["cache-control"], "no-store")
        self.client.cookies.clear()
        for path in ("/admin/", "/admin/devices", "/admin/api-keys", "/admin/settings"):
            self.assertIn("管理员密码", self.client.get(path).text)

    def test_api_order_persists_and_service_save_does_not_clear_keys(self):
        response = self.client.post(
            "/admin/api-keys", data=self.api_form(qwen_api_key="test-qwen-secret")
        )
        self.assertEqual(response.status_code, 200, response.text)
        current = self.client.app.state.effective_settings
        self.assertEqual(current.llm_provider_order, ("qwen", "deepseek"))
        self.assertEqual(current.qwen_api_key, "test-qwen-secret")
        saved = self.client.post(
            "/admin/config",
            data={
                "csrf": self.csrf, "section": "service", "voice_api_enabled": "on",
                "translation_api_enabled": "on", "asr_provider": "mock", "tts_provider": "mock",
            },
        )
        self.assertEqual(saved.status_code, 200, saved.text)
        self.assertEqual(self.client.app.state.effective_settings.llm_provider_order, ("qwen", "deepseek"))
        self.assertEqual(self.client.app.state.effective_settings.qwen_api_key, "test-qwen-secret")
        with TestClient(create_app(self.settings)) as restarted:
            self.assertEqual(restarted.app.state.effective_settings.llm_provider_order, ("qwen", "deepseek"))

    def test_key_clear_overrides_environment_and_blank_preserves_value(self):
        saved = self.client.post("/admin/api-keys", data=self.api_form())
        self.assertEqual(saved.status_code, 200)
        self.assertEqual(self.client.app.state.effective_settings.deepseek_api_key, "test-environment-key")
        self.client.post("/admin/api-keys", data=self.api_form(clear_deepseek_api_key="on"))
        self.assertEqual(self.client.app.state.effective_settings.deepseek_api_key, "")
        self.assertNotIn("test-environment-key", (Path(self.temp.name) / "admin_config.enc").read_text())

    def test_invalid_order_and_csrf_leave_config_unchanged(self):
        before = dict(self.client.app.state.runtime_config)
        for data, status in (
            (self.api_form(secondary_provider="qwen"), 422),
            (self.api_form(primary_provider="unknown"), 422),
            (self.api_form(csrf="bad"), 403),
            (self.api_form(deepseek_base_url="https://user:password@api.deepseek.com"), 422),
        ):
            response = self.client.post("/admin/api-keys", data=data)
            self.assertEqual(response.status_code, status, response.text)
            self.assertEqual(self.client.app.state.runtime_config, before)

    def test_fetch_models_is_authenticated_and_does_not_change_settings(self):
        before = dict(self.client.app.state.runtime_config)
        with patch("cloud_server.admin.fetch_models", return_value=["deepseek-live-model"]) as fetch:
            denied = self.client.post("/admin/api-keys/deepseek/models", data={"csrf": "bad"})
            self.assertEqual(denied.status_code, 403)
            fetch.assert_not_called()
            response = self.client.post("/admin/api-keys/deepseek/models", data={"csrf": self.csrf})
            self.assertEqual(response.status_code, 200, response.text)
            self.assertIn("deepseek-live-model", response.text)
            self.assertNotIn("test-environment-key", response.text)
            self.assertEqual(self.client.app.state.runtime_config, before)

    def test_empty_model_catalog_is_not_misrepresented_as_suggestions(self):
        with patch("cloud_server.admin.fetch_models", return_value=[]):
            response = self.client.post("/admin/api-keys/deepseek/models", data={"csrf": self.csrf})
        self.assertIn("接口返回 0 个模型", response.text)
        self.assertNotIn('<option value="deepseek-v4-pro">', response.text)

    def test_reset_clears_custom_model_list_addresses(self):
        response = self.client.post("/admin/api-keys", data=self.api_form(
            deepseek_base_url="https://gateway.example/v1",
            deepseek_models_url="https://gateway.example/v1/models",
        ))
        self.assertEqual(response.status_code, 200)
        self.client.post("/admin/reset-recommended", data={"csrf": self.csrf, "confirm_reset": "on"})
        self.assertEqual(self.client.app.state.runtime_config["deepseek_models_url"], "")
        response = self.client.post("/admin/api-keys", data=self.api_form())
        self.assertEqual(response.status_code, 200)

    def test_model_failure_displays_error_and_keeps_configuration(self):
        from cloud_server.model_catalog import ModelCatalogError
        before = dict(self.client.app.state.runtime_config)
        with patch("cloud_server.admin.fetch_models", side_effect=ModelCatalogError("模型列表鉴权失败")):
            response = self.client.post("/admin/api-keys/deepseek/models", data={"csrf": self.csrf})
        self.assertIn("模型列表鉴权失败", response.text)
        self.assertEqual(self.client.app.state.runtime_config, before)

    def test_device_cards_escape_names_and_display_individual_statistics(self):
        store = self.client.app.state.store
        store.register_device("device-a", '<script>alert("test")</script>', "device-secret")
        store.record_usage(device_id="device-a", provider="deepseek", key_id="fake-key-id", model="test-model",
                           operation="dialogue", success=True, input_tokens=30, output_tokens=12, total_tokens=42)
        response = self.client.get("/admin/devices")
        self.assertIn('&lt;script&gt;', response.text)
        self.assertNotIn('<script>', response.text)
        self.assertIn('42<small>TOKENS', response.text)
        self.assertNotIn('device-secret', response.text)


if __name__ == "__main__":
    unittest.main()
