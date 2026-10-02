from __future__ import annotations

import unittest
from unittest.mock import patch

import httpx

from cloud_server.model_catalog import ModelCatalogError, PROVIDERS, fetch_models


TEST_KEY = "test-catalog-key-never-real"


def response(payload=None, status=200, *, content=None, headers=None):
    return httpx.Response(
        status,
        json=payload if content is None else None,
        content=content,
        headers=headers,
        request=httpx.Request("GET", "https://catalog.example/models"),
    )


class ModelCatalogTest(unittest.TestCase):
    def test_provider_metadata_is_documented_and_glm_has_no_guessed_catalog(self):
        self.assertEqual(set(PROVIDERS), {"deepseek", "qwen", "glm", "custom", "local_llm"})
        for provider in PROVIDERS.values():
            for field in ("label", "base_url", "models_url", "protocol_label", "docs_url"):
                self.assertIsInstance(provider[field], str)
            self.assertIsInstance(provider["suggested_models"], tuple)
            self.assertTrue(provider["suggested_models"])
            self.assertTrue(provider["docs_url"].startswith("https://"))
        self.assertEqual(PROVIDERS["glm"]["models_url"], "")
        self.assertTrue(PROVIDERS["qwen"]["base_url"].endswith("/services/aigc/text-generation/generation"))

    @patch("cloud_server.model_catalog.httpx.get")
    def test_deepseek_gets_real_ids_only_and_preserves_order(self, get):
        get.return_value = response({"object": "list", "data": [
            {"id": "deepseek-flash"}, {"id": "deepseek-v4-pro"}, {"id": "deepseek-flash"},
            {"id": "<script>alert(1)</script>"}, {"id": "bad\nmodel"}, {"id": ""},
            {"id": 123}, {"id": "x" * 300}, "unexpected", {},
        ]})
        self.assertEqual(fetch_models("deepseek", TEST_KEY, "https://api.deepseek.com"), ["deepseek-flash", "deepseek-v4-pro"])
        args, kwargs = get.call_args
        self.assertEqual(args[0], "https://api.deepseek.com/models")
        self.assertEqual(kwargs["headers"]["Authorization"], f"Bearer {TEST_KEY}")
        self.assertFalse(kwargs["follow_redirects"])
        self.assertGreaterEqual(kwargs["timeout"], 10)
        self.assertLessEqual(kwargs["timeout"], 15)

    @patch("cloud_server.model_catalog.httpx.get")
    def test_deepseek_keeps_custom_origin_and_prefix(self, get):
        get.return_value = response({"data": [{"id": "custom/model-v1"}]})
        for base in ("http://127.0.0.1:8080/proxy/v1/", "http://127.0.0.1:8080/proxy/v1/chat/completions"):
            with self.subTest(base=base):
                self.assertEqual(fetch_models("deepseek", TEST_KEY, base), ["custom/model-v1"])
                self.assertEqual(get.call_args.args[0], "http://127.0.0.1:8080/proxy/v1/models")

    @patch("cloud_server.model_catalog.httpx.get")
    def test_qwen_uses_native_pagination_and_accepts_legacy_id_shape(self, get):
        get.side_effect = [
            response({"success": True, "code": None, "output": {"total": 3, "page_no": 1, "models": [{"model": "qwen-plus"}, {"model_id": "qwen-turbo"}]}}),
            response({"success": True, "output": {"total": 3, "page_no": 2, "models": [{"model": "qwen3-max"}]}}),
        ]
        base = "https://workspace123.cn-beijing.maas.aliyuncs.com/api/v1/services/aigc/text-generation/generation"
        self.assertEqual(fetch_models("qwen", TEST_KEY, base), ["qwen-plus", "qwen-turbo", "qwen3-max"])
        self.assertEqual(get.call_count, 2)
        for number, call in enumerate(get.call_args_list, 1):
            self.assertEqual(call.args[0], "https://workspace123.cn-beijing.maas.aliyuncs.com/api/v1/models")
            self.assertEqual(call.kwargs["params"]["capabilities"], "TG")
            self.assertEqual(call.kwargs["params"]["page_no"], number)
            self.assertEqual(call.kwargs["params"]["page_size"], 100)
            self.assertFalse(call.kwargs["follow_redirects"])

    @patch("cloud_server.model_catalog.httpx.get")
    def test_qwen_legacy_and_compatible_hosts_stay_on_the_configured_origin(self, get):
        get.return_value = response({"output": {"total": 1, "models": [{"model_id": "qwen-plus"}]}})
        cases = (
            ("https://dashscope.aliyuncs.com/api/v1/services/aigc/text-generation/generation", "https://dashscope.aliyuncs.com/api/v1/models"),
            ("https://dashscope.aliyuncs.com/compatible-mode/v1", "https://dashscope.aliyuncs.com/api/v1/models"),
            ("https://dashscope-intl.aliyuncs.com/compatible-mode/v1/", "https://dashscope-intl.aliyuncs.com/api/v1/models"),
            ("https://gateway.example/proxy/api/v1/services/aigc/text-generation/generation", "https://gateway.example/proxy/api/v1/models"),
        )
        for base, expected in cases:
            with self.subTest(base=base):
                self.assertEqual(fetch_models("qwen", TEST_KEY, base), ["qwen-plus"])
                self.assertEqual(get.call_args.args[0], expected)

    @patch("cloud_server.model_catalog.httpx.get")
    def test_glm_requires_an_explicit_supported_catalog_and_never_returns_candidates_as_live(self, get):
        with self.assertRaisesRegex(ModelCatalogError, "未.*模型列表"):
            fetch_models("glm", TEST_KEY, PROVIDERS["glm"]["base_url"])
        get.assert_not_called()

    @patch("cloud_server.model_catalog.httpx.get")
    def test_explicit_same_origin_catalog_supports_a_glm_gateway(self, get):
        get.return_value = response({"data": [{"id": "glm-4.7-flash"}]})
        models = fetch_models("glm", TEST_KEY, "https://gateway.example/v4", "https://gateway.example/private/models")
        self.assertEqual(models, ["glm-4.7-flash"])
        self.assertEqual(get.call_args.args[0], "https://gateway.example/private/models")

    @patch("cloud_server.model_catalog.httpx.get")
    def test_same_origin_comparison_normalizes_case_and_default_port(self, get):
        get.return_value = response({"data": []})
        self.assertEqual(fetch_models("deepseek", TEST_KEY, "https://API.DEEPSEEK.COM:443/v1", "https://api.deepseek.com/models"), [])
        self.assertEqual(get.call_count, 1)

    @patch("cloud_server.model_catalog.httpx.get")
    def test_different_origin_never_receives_key(self, get):
        for url in ("https://elsewhere.example/models", "http://api.deepseek.com/models", "https://api.deepseek.com:8443/models", "https://api.deepseek.com:0/models"):
            with self.subTest(url=url), self.assertRaisesRegex(ModelCatalogError, "同源"):
                fetch_models("deepseek", TEST_KEY, "https://api.deepseek.com", url)
        get.assert_not_called()

    @patch("cloud_server.model_catalog.httpx.get")
    def test_invalid_urls_never_send_network_requests(self, get):
        urls = (
            "", "api.deepseek.com", "file:///tmp/models", "ftp://api.deepseek.com/models",
            "https://", "https://user:secret@api.deepseek.com", "https://api.deepseek.com?key=secret",
            "https://api.deepseek.com?", "https://api.deepseek.com#fragment", "https://api.deepseek.com#",
            "https://api.deepseek.com\n.evil.example", "https://api.deepseek.com\\@evil.example",
            "https://api.deepseek.com:bad", "https://api.deepseek.com:99999", "https://{WorkspaceId}.cn-beijing.maas.aliyuncs.com",
        )
        for url in urls:
            with self.subTest(url=url), self.assertRaises(ModelCatalogError):
                fetch_models("deepseek", TEST_KEY, url)
        get.assert_not_called()

    @patch("cloud_server.model_catalog.httpx.get")
    def test_invalid_explicit_catalog_url_is_rejected_before_request(self, get):
        for url in ("https://user:secret@api.deepseek.com/models", "https://api.deepseek.com/models?access_token=secret", "https://api.deepseek.com/models#id"):
            with self.subTest(url=url), self.assertRaises(ModelCatalogError):
                fetch_models("deepseek", TEST_KEY, "https://api.deepseek.com", url)
        get.assert_not_called()

    @patch("cloud_server.model_catalog.httpx.get")
    def test_missing_or_malformed_key_and_unknown_provider_do_not_request(self, get):
        for key in ("", "  ", "secret\r\nX-Injected: true", "not a key"):
            with self.subTest(key=key), self.assertRaises(ModelCatalogError):
                fetch_models("deepseek", key, "https://api.deepseek.com")
        with self.assertRaises(ModelCatalogError):
            fetch_models("unknown", TEST_KEY, "https://api.deepseek.com")
        get.assert_not_called()

    @patch("cloud_server.model_catalog.httpx.get")
    def test_http_failures_and_redirects_are_safe_errors_without_body(self, get):
        for status in (301, 302, 307, 308, 401, 403, 404, 429, 500):
            get.return_value = response(status=status, content=f"secret body {TEST_KEY}".encode(), headers={"Location": "https://elsewhere.example/models"})
            with self.subTest(status=status), self.assertRaises(ModelCatalogError) as raised:
                fetch_models("deepseek", TEST_KEY, "https://api.deepseek.com")
            self.assertNotIn(TEST_KEY, str(raised.exception))
            self.assertNotIn("secret body", str(raised.exception))
            self.assertFalse(get.call_args.kwargs["follow_redirects"])

    @patch("cloud_server.model_catalog.httpx.get")
    def test_transport_errors_and_invalid_json_never_expose_upstream_details(self, get):
        for error in (httpx.ReadTimeout(TEST_KEY), httpx.ConnectError(TEST_KEY), httpx.InvalidURL(TEST_KEY)):
            get.side_effect = error
            with self.subTest(error=type(error).__name__), self.assertRaises(ModelCatalogError) as raised:
                fetch_models("deepseek", TEST_KEY, "https://api.deepseek.com")
            self.assertNotIn(TEST_KEY, str(raised.exception))
            self.assertTrue(raised.exception.__suppress_context__)
        get.side_effect = None
        get.return_value = response(content=f"not-json {TEST_KEY}".encode())
        with self.assertRaises(ModelCatalogError) as raised:
            fetch_models("deepseek", TEST_KEY, "https://api.deepseek.com")
        self.assertNotIn(TEST_KEY, str(raised.exception))

    @patch("cloud_server.model_catalog.httpx.get")
    def test_business_failures_are_not_replaced_with_suggested_models(self, get):
        for payload in (
            {"error": {"message": TEST_KEY}},
            {"success": False, "message": TEST_KEY, "output": {"models": []}},
            {"code": "InvalidApiKey", "message": TEST_KEY},
        ):
            get.return_value = response(payload)
            with self.subTest(payload=payload), self.assertRaises(ModelCatalogError) as raised:
                fetch_models("qwen", TEST_KEY, PROVIDERS["qwen"]["base_url"])
            self.assertNotIn(TEST_KEY, str(raised.exception))

    @patch("cloud_server.model_catalog.httpx.get")
    def test_malformed_catalog_envelopes_are_rejected(self, get):
        for payload in ([], {}, {"data": None}, {"data": {}}, {"output": []}, {"output": {"models": "qwen-plus"}}):
            get.return_value = response(payload)
            with self.subTest(payload=payload), self.assertRaises(ModelCatalogError):
                fetch_models("qwen", TEST_KEY, PROVIDERS["qwen"]["base_url"])

    @patch("cloud_server.model_catalog.httpx.get")
    def test_qwen_incomplete_pagination_raises_without_returning_partial_results(self, get):
        get.side_effect = [
            response({"output": {"total": 2, "models": [{"model": "qwen-plus"}]}}),
            response({"output": {"total": 2, "models": []}}),
        ]
        with self.assertRaisesRegex(ModelCatalogError, "分页"):
            fetch_models("qwen", TEST_KEY, PROVIDERS["qwen"]["base_url"])
        self.assertEqual(get.call_count, 2)

    @patch("cloud_server.model_catalog.httpx.get")
    def test_qwen_invalid_pagination_metadata_is_rejected(self, get):
        for metadata in ({"total": -1}, {"total": "2"}, {"total": True}, {"total": 2, "page_no": 2}):
            get.return_value = response({"output": {"models": [{"model": "qwen-plus"}], **metadata}})
            with self.subTest(metadata=metadata), self.assertRaisesRegex(ModelCatalogError, "分页"):
                fetch_models("qwen", TEST_KEY, PROVIDERS["qwen"]["base_url"])

    @patch("cloud_server.model_catalog.httpx.get")
    def test_qwen_legacy_response_without_total_stops_on_short_page(self, get):
        get.return_value = response({"output": {"models": [{"model_id": "qwen-plus"}]}})
        self.assertEqual(fetch_models("qwen", TEST_KEY, PROVIDERS["qwen"]["base_url"]), ["qwen-plus"])
        self.assertEqual(get.call_count, 1)

    @patch("cloud_server.model_catalog.httpx.get")
    def test_runaway_qwen_catalog_cannot_loop_forever_or_return_partial_results(self, get):
        get.return_value = response({"output": {"total": 10_000, "models": [{"model": "qwen-plus"}]}})
        with self.assertRaisesRegex(ModelCatalogError, "分页"):
            fetch_models("qwen", TEST_KEY, PROVIDERS["qwen"]["base_url"])
        self.assertLessEqual(get.call_count, 100)

    @patch("cloud_server.model_catalog.httpx.get")
    def test_qwen_explicit_compatible_catalog_can_return_data_ids(self, get):
        get.return_value = response({"data": [{"id": "qwen-plus"}]})
        self.assertEqual(fetch_models("qwen", TEST_KEY, "https://dashscope.aliyuncs.com/api/v1", "https://dashscope.aliyuncs.com/compatible-mode/v1/models"), ["qwen-plus"])


if __name__ == "__main__":
    unittest.main()
