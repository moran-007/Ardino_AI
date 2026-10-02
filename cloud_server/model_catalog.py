"""Read-only model discovery for the configured Chinese LLM providers.

Suggested models are documentation examples, never a substitute for a successful
live catalog request. A listed model may still require account permissions.
Importing this module performs no network I/O.
"""

from __future__ import annotations

import re
from urllib.parse import SplitResult, urlsplit, urlunsplit

import httpx


class ModelCatalogError(RuntimeError):
    """A user-facing catalog error with no credentials or upstream body."""


# Official documentation checked on 2026-09-30. Beijing's new DashScope hostname
# includes the user's WorkspaceId; retain the working legacy native default and
# derive model URLs from the actual configured origin, including workspace hosts.
PROVIDERS = {
    "deepseek": {
        "label": "DeepSeek",
        "base_url": "https://api.deepseek.com",
        "models_url": "https://api.deepseek.com/models",
        "protocol_label": "DeepSeek 官方 Chat Completions",
        "suggested_models": ("deepseek-flash", "deepseek-v4-pro"),
        "docs_url": "https://api-docs.deepseek.com/zh-cn/api/list-models/",
    },
    "qwen": {
        "label": "通义千问",
        "base_url": "https://dashscope.aliyuncs.com/api/v1/services/aigc/text-generation/generation",
        "models_url": "https://dashscope.aliyuncs.com/api/v1/models",
        "protocol_label": "阿里云 DashScope 原生 HTTP / SSE",
        "suggested_models": ("qwen-plus", "qwen3-max"),
        "docs_url": "https://help.aliyun.com/zh/model-studio/list-models",
    },
    "glm": {
        "label": "智谱 GLM",
        "base_url": "https://open.bigmodel.cn/api/paas/v4",
        # The official API index does not document a generic list-models API.
        "models_url": "",
        "protocol_label": "智谱官方 HTTP / SSE",
        "suggested_models": (
            "glm-4.7-flash", "glm-5.3", "glm-5.3-flash", "glm-5.2", "glm-4.7", "glm-4.5-flash",
        ),
        "docs_url": "https://docs.bigmodel.cn/cn/guide/start/model-overview",
    },
    'custom': {
        'label': '自定义外部模型 API', 'base_url': 'https://api.example.com/v1',
        'models_url': '', 'protocol_label': '自定义 HTTP 接口',
        'suggested_models': ('custom-model',),
        'docs_url': 'https://docs.ollama.com/api/openai-compatibility',
    },
    'local_llm': {
        'label': '服务器本地语言模型', 'base_url': 'http://127.0.0.1:11434',
        'models_url': '', 'protocol_label': '本地推理 HTTP 接口',
        'suggested_models': ('qwen3:8b',), 'docs_url': 'https://docs.ollama.com/api/chat',
    },
}

_MODEL_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9._:/@+\-]{0,199}\Z")
_PAGE_SIZE = 100
_MAX_PAGES = 50
_TIMEOUT_SECONDS = 12.0


def _validated_url(value: str, label: str) -> SplitResult:
    if (
        not isinstance(value, str)
        or not value
        or any(character.isspace() or ord(character) < 32 or ord(character) == 127 for character in value)
        or any(character in value for character in ("\\", "?", "#"))
    ):
        raise ModelCatalogError(f"{label}必须是完整 HTTP/HTTPS 地址，且不能包含凭据、查询参数或片段")
    try:
        parts = urlsplit(value)
        if (
            parts.scheme.lower() not in {"http", "https"}
            or not parts.hostname
            or parts.username is not None
            or parts.password is not None
            or parts.query
            or parts.fragment
            or any(character in parts.hostname for character in ("%", "{", "}", "<", ">"))
        ):
            raise ValueError
        # Accessing .port validates its type/range; httpx validates host syntax.
        parts.port
        httpx.URL(value)
    except (ValueError, httpx.InvalidURL):
        raise ModelCatalogError(f"{label}必须是完整 HTTP/HTTPS 地址，且不能包含凭据、查询参数或片段") from None
    return parts._replace(scheme=parts.scheme.lower())


def _origin(parts: SplitResult) -> tuple[str, str, int]:
    port = parts.port if parts.port is not None else (443 if parts.scheme == "https" else 80)
    return parts.scheme, (parts.hostname or "").lower(), port


def _catalog_url(provider: str, base: SplitResult, models_url: str) -> str:
    if models_url:
        target = _validated_url(models_url, "模型列表地址")
        if _origin(target) != _origin(base):
            raise ModelCatalogError("模型列表地址必须与 API 地址同源（协议、域名和端口一致）")
        return urlunsplit(target)

    if provider == "glm":
        raise ModelCatalogError("智谱官方文档未提供通用模型列表接口；请参考文档候选模型，或填写已确认的同源模型列表地址")

    path = base.path.rstrip("/")
    if path.endswith("/chat/completions"):
        path = path[: -len("/chat/completions")]
    if provider == "qwen":
        for suffix in (
            "/api/v1/services/aigc/text-generation/generation",
            "/api/v1/services/aigc/multimodal-generation/generation",
            "/compatible-mode/v1",
        ):
            if path.endswith(suffix):
                path = path[: -len(suffix)] + "/api/v1/models"
                break
        else:
            if not path:
                path = "/api/v1/models"
            elif path.endswith("/api/v1"):
                path += "/models"
            elif not path.endswith("/api/v1/models"):
                raise ModelCatalogError("无法从千问 API 地址确定模型列表接口，请填写已确认的同源模型列表地址")
    elif not path.endswith("/models"):
        path += "/models"
    return urlunsplit(base._replace(path=path))


def _get_catalog(url: str, api_key: str, params: dict | None) -> dict:
    try:
        response = httpx.get(
            url,
            headers={**({"Authorization": f"Bearer {api_key}"} if api_key else {}), "Accept": "application/json"},
            params=params,
            timeout=_TIMEOUT_SECONDS,
            follow_redirects=False,
        )
    except httpx.TimeoutException:
        raise ModelCatalogError("获取模型列表超时，请稍后重试") from None
    except (httpx.HTTPError, httpx.InvalidURL, ValueError):
        raise ModelCatalogError("无法连接模型列表接口，请检查 API 地址和网络") from None

    status = response.status_code
    if 300 <= status < 400:
        raise ModelCatalogError("模型列表接口返回重定向，请直接配置最终的同源地址")
    if status in {401, 403}:
        raise ModelCatalogError("模型列表鉴权失败，请检查 API Key 和访问权限")
    if status == 429:
        raise ModelCatalogError("模型列表查询频率受限，请稍后重试")
    if not 200 <= status < 300:
        raise ModelCatalogError(f"获取模型列表失败（HTTP {status}），请检查接口地址和服务状态")

    try:
        payload = response.json()
    except ValueError:
        raise ModelCatalogError("模型列表接口返回了无效 JSON") from None
    if not isinstance(payload, dict):
        raise ModelCatalogError("模型列表响应格式不正确")
    if (
        payload.get("error")
        or payload.get("success") is False
        or payload.get("code") not in (None, "", 0, "0", 200, "200", "OK", "Success", "success")
    ):
        raise ModelCatalogError("模型列表接口返回业务错误，请检查 API Key、权限和接口地址")
    return payload


def _append_models(entries: list, fields: tuple[str, ...], api_key: str, models: list[str], seen: set[str]) -> None:
    for entry in entries:
        if not isinstance(entry, dict):
            continue
        for field in fields:
            model = entry.get(field)
            if isinstance(model, str) and _MODEL_ID.fullmatch(model) and model != api_key:
                if model not in seen:
                    models.append(model)
                    seen.add(model)
                break


def fetch_models(provider: str, api_key: str, base_url: str, models_url: str = "", protocol: str = 'openai') -> list[str]:
    """Fetch model IDs using GET only, without generation or credential redirects.

    Explicit model endpoints must share the configured API origin. DeepSeek and
    explicitly configured compatible endpoints use ``data[].id``. DashScope's
    native endpoint uses ``output.models[].model`` (legacy ``model_id`` accepted)
    and is paged until ``output.total`` records have been read. IDs are validated,
    deduplicated, and returned in the provider's order. No static fallback occurs.
    """
    if not isinstance(provider, str) or provider not in PROVIDERS:
        raise ModelCatalogError("不支持此模型服务商")
    if not isinstance(api_key, str) or (not api_key and provider not in {'custom', 'local_llm'}) or not api_key.isascii() or any(
        character.isspace() or ord(character) < 32 or ord(character) == 127 for character in api_key
    ):
        raise ModelCatalogError("请先配置有效的 API Key")
    base = _validated_url(base_url, "API 地址")
    if provider in {'custom', 'local_llm'} and protocol == 'ollama':
        path = base.path.rstrip('/')
        if path.endswith('/api/chat'):
            path = path[:-5]
        if not path.endswith('/api'):
            path += '/api'
        url = _catalog_url(provider, base, models_url or urlunsplit(base._replace(path=path + '/tags')))
        payload = _get_catalog(url, api_key, None)
        if not isinstance(payload.get('models'), list):
            raise ModelCatalogError('Ollama 模型列表响应格式不正确')
        models = []
        _append_models(payload['models'], ('name', 'model'), api_key, models, set())
        return models
    url = _catalog_url(provider, base, models_url)
    models: list[str] = []
    seen: set[str] = set()
    received = 0

    for page_no in range(1, _MAX_PAGES + 1):
        params = {"capabilities": "TG", "page_no": page_no, "page_size": _PAGE_SIZE} if provider == "qwen" else None
        payload = _get_catalog(url, api_key, params)
        if provider == "qwen" and "output" in payload:
            output = payload["output"]
            if not isinstance(output, dict) or not isinstance(output.get("models"), list):
                raise ModelCatalogError("千问模型列表响应格式不正确")
            entries = output["models"]
            total = output.get("total")
            returned_page = output.get("page_no")
            if (
                total is not None and (type(total) is not int or total < 0)
                or returned_page is not None and (type(returned_page) is not int or returned_page != page_no)
            ):
                raise ModelCatalogError("千问模型列表分页信息不正确")
            received += len(entries)
            _append_models(entries, ("model", "model_id", "id"), api_key, models, seen)
            if total is not None and received >= total:
                return models
            if total is None and len(entries) < _PAGE_SIZE:
                return models
            if not entries:
                raise ModelCatalogError("千问模型列表分页响应不完整，请稍后重试")
        else:
            entries = payload.get("data")
            if not isinstance(entries, list):
                raise ModelCatalogError("模型列表响应格式不正确")
            _append_models(entries, ("id",), api_key, models, seen)
            return models

    raise ModelCatalogError("模型列表分页超出查询上限，请检查模型列表接口")
