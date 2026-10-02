from __future__ import annotations

import base64
import hashlib
import io
import json
import os
import re
import shutil
import tempfile
import threading
import zipfile
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import urlsplit

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa


VERSION_PATTERN = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$")
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
PACKAGE_FILES = {"manifest.json", "firmware.bin"}
MIN_FIRMWARE_BYTES = 65_536
MAX_FIRMWARE_BYTES = 0x300000
MAX_MANIFEST_BYTES = 4096
MAX_PACKAGE_BYTES = MAX_FIRMWARE_BYTES + 65_536


class FirmwarePackageError(ValueError):
    pass


@dataclass(frozen=True)
class FirmwareRelease:
    version: str
    hardware: str
    size: int
    sha256: str
    url: str
    signature: str
    force: bool
    published_at: str
    notes: str = ""
    withdrawn: bool = False
    minimum_version: str = ""

    def manifest(self) -> dict[str, object]:
        return {
            "version": self.version,
            "hardware": self.hardware,
            "size": self.size,
            "sha256": self.sha256,
            "url": self.url,
            "signature": self.signature,
            "force": self.force,
        }


def _version_tuple(value: str) -> tuple[int, int, int]:
    match = VERSION_PATTERN.fullmatch(value)
    if not match:
        raise FirmwarePackageError("固件版本必须是主.次.修订三段数字")
    parts = tuple(int(part) for part in match.groups())
    if any(part > 999_999 for part in parts):
        raise FirmwarePackageError("固件版本的每一段不能超过 999999")
    return parts


class FirmwareRepository:
    def __init__(
        self,
        root: Path,
        public_key_path: Path,
        public_base_url: str,
        hardware_id: str,
    ) -> None:
        self.root = root.resolve()
        self.public_key_path = public_key_path.resolve()
        self.public_base_url = public_base_url.rstrip("/")
        self.hardware_id = hardware_id
        self.lock = threading.Lock()

    def _public_key(self) -> rsa.RSAPublicKey:
        try:
            key = serialization.load_pem_public_key(self.public_key_path.read_bytes())
        except (OSError, ValueError) as exc:
            raise FirmwarePackageError("固件验签公钥不存在或格式无效") from exc
        if not isinstance(key, rsa.RSAPublicKey) or key.key_size < 2048:
            raise FirmwarePackageError("固件验签公钥必须是至少 2048 位 RSA 公钥")
        return key

    def _validate_manifest(self, raw: bytes, firmware: bytes) -> dict[str, object]:
        if not raw or len(raw) > MAX_MANIFEST_BYTES:
            raise FirmwarePackageError("manifest.json 必须非空且不超过 4 KiB")
        try:
            manifest = json.loads(raw.decode("utf-8"))
        except (UnicodeError, json.JSONDecodeError) as exc:
            raise FirmwarePackageError("manifest.json 不是有效 UTF-8 JSON") from exc
        required = {"version", "hardware", "size", "sha256", "url", "signature", "force"}
        if not isinstance(manifest, dict) or set(manifest) != required:
            raise FirmwarePackageError("manifest.json 字段集合不正确")
        if isinstance(manifest["size"], bool) or not isinstance(manifest["size"], int):
            raise FirmwarePackageError("固件 size 必须是整数")
        if not isinstance(manifest["force"], bool):
            raise FirmwarePackageError("固件 force 必须是布尔值")
        for name in ("version", "hardware", "sha256", "url", "signature"):
            if not isinstance(manifest[name], str):
                raise FirmwarePackageError(f"固件 {name} 必须是字符串")

        version = manifest["version"]
        _version_tuple(version)
        if manifest["hardware"] != self.hardware_id:
            raise FirmwarePackageError("固件硬件通道与服务器配置不一致")
        if not MIN_FIRMWARE_BYTES <= len(firmware) <= MAX_FIRMWARE_BYTES:
            raise FirmwarePackageError("firmware.bin 大小必须在 64 KiB 到 3 MiB 之间")
        if manifest["size"] != len(firmware):
            raise FirmwarePackageError("固件实际大小与清单不一致")
        digest = hashlib.sha256(firmware).hexdigest()
        if not SHA256_PATTERN.fullmatch(manifest["sha256"]) or manifest["sha256"] != digest:
            raise FirmwarePackageError("固件 SHA-256 与清单不一致")
        if firmware[0] != 0xE9:
            raise FirmwarePackageError("firmware.bin 不是有效的 ESP32 应用镜像")

        expected_url = f"{self.public_base_url}/voice-api/v1/firmware/download/{version}"
        parsed = urlsplit(manifest["url"])
        if parsed.scheme != "https" or parsed.username or parsed.password or manifest["url"] != expected_url:
            raise FirmwarePackageError("固件下载 URL 必须与服务器 HTTPS 公网地址和版本完全匹配")
        canonical = (
            f"{version}\n{self.hardware_id}\n{len(firmware)}\n{digest}\n"
            f"{manifest['url']}\n{'true' if manifest['force'] else 'false'}"
        )
        try:
            signature = base64.b64decode(manifest["signature"], validate=True)
            self._public_key().verify(
                signature, canonical.encode("utf-8"), padding.PKCS1v15(), hashes.SHA256()
            )
        except (ValueError, InvalidSignature) as exc:
            raise FirmwarePackageError("固件清单 RSA 签名无效") from exc
        return manifest

    def publish_package(self, package_bytes: bytes, notes: str = "", *, activate: bool = True,
                        minimum_version: str = "") -> FirmwareRelease:
        if not isinstance(notes, str) or len(notes) > 2000:
            raise FirmwarePackageError("更新说明不能超过 2000 字")
        if minimum_version:
            _version_tuple(minimum_version)
        if not package_bytes or len(package_bytes) > MAX_PACKAGE_BYTES:
            raise FirmwarePackageError("上传包为空或超过允许大小")
        try:
            with zipfile.ZipFile(io.BytesIO(package_bytes)) as package:
                infos = package.infolist()
                names = [item.filename for item in infos]
                if len(names) != 2 or set(names) != PACKAGE_FILES:
                    raise FirmwarePackageError("ZIP 必须且只能包含 manifest.json 和 firmware.bin")
                if any(item.is_dir() or item.file_size > MAX_PACKAGE_BYTES for item in infos):
                    raise FirmwarePackageError("ZIP 条目类型或解压大小无效")
                manifest_raw = package.read("manifest.json")
                firmware = package.read("firmware.bin")
        except (zipfile.BadZipFile, KeyError, RuntimeError) as exc:
            raise FirmwarePackageError("上传文件不是有效的 OTA ZIP 包") from exc

        manifest = self._validate_manifest(manifest_raw, firmware)
        version = str(manifest["version"])
        if minimum_version and _version_tuple(minimum_version) >= _version_tuple(version):
            raise FirmwarePackageError("最低来源版本必须低于新发布版本")
        published_at = datetime.now(timezone.utc).isoformat()
        release = FirmwareRelease(
            version=version,
            hardware=str(manifest["hardware"]),
            size=int(manifest["size"]),
            sha256=str(manifest["sha256"]),
            url=str(manifest["url"]),
            signature=str(manifest["signature"]),
            force=bool(manifest["force"]),
            published_at=published_at,
            notes=notes.strip(),
            minimum_version=minimum_version,
        )

        releases_root = self.root / "releases"
        target = releases_root / version
        with self.lock:
            releases_root.mkdir(parents=True, exist_ok=True)
            current = self.current()
            if activate and current and _version_tuple(version) <= _version_tuple(current.version):
                raise FirmwarePackageError("新发布版本必须高于当前版本，禁止覆盖或降级发布")
            if target.exists():
                raise FirmwarePackageError("该版本已经发布；请提高版本号，已发布版本不可覆盖")
            staging = Path(tempfile.mkdtemp(prefix=".publish-", dir=releases_root))
            try:
                (staging / "firmware.bin").write_bytes(firmware)
                (staging / "package.zip").write_bytes(package_bytes)
                (staging / "manifest.json").write_text(
                    json.dumps(release.manifest(), ensure_ascii=False, separators=(",", ":")),
                    encoding="utf-8",
                )
                (staging / "release.json").write_text(
                    json.dumps(release.__dict__, ensure_ascii=False, separators=(",", ":")),
                    encoding="utf-8",
                )
                os.replace(staging, target)
                if activate:
                    pointer = self.root / ".current.tmp"
                    pointer.write_text(
                        json.dumps({"version": version}, separators=(",", ":")), encoding="utf-8"
                    )
                    os.replace(pointer, self.root / "current.json")
            finally:
                if staging.exists():
                    shutil.rmtree(staging)
        return release

    def current(self) -> FirmwareRelease | None:
        try:
            pointer = json.loads((self.root / "current.json").read_text(encoding="utf-8"))
            return self.release(pointer["version"])
        except (OSError, KeyError, TypeError, json.JSONDecodeError):
            return None

    def release(self, version: str) -> FirmwareRelease | None:
        try:
            _version_tuple(version)
            values = json.loads(
                (self.root / "releases" / version / "release.json").read_text(encoding="utf-8")
            )
            release = FirmwareRelease(**values)
            if release.version != version or release.hardware != self.hardware_id:
                return None
            return release
        except (OSError, KeyError, TypeError, json.JSONDecodeError, FirmwarePackageError):
            return None

    def list_releases(self) -> list[FirmwareRelease]:
        directory = self.root / "releases"
        if not directory.is_dir():
            return []
        releases = [release for path in directory.iterdir()
                    if path.is_dir() and (release := self.release(path.name)) is not None]
        return sorted(releases, key=lambda release: _version_tuple(release.version), reverse=True)

    def package_bytes(self, version: str) -> bytes | None:
        release = self.release(version)
        if not release or release.withdrawn:
            return None
        directory = self.root / "releases" / version
        try:
            firmware = (directory / "firmware.bin").read_bytes()
            manifest = (directory / "manifest.json").read_bytes()
            if self._validate_manifest(manifest, firmware) != release.manifest():
                return None
            stored = directory / "package.zip"
            if stored.is_file():
                payload = stored.read_bytes()
                if len(payload) > MAX_PACKAGE_BYTES:
                    return None
                with zipfile.ZipFile(io.BytesIO(payload)) as archive:
                    if (len(archive.infolist()) != 2 or set(archive.namelist()) != PACKAGE_FILES or
                            any(item.file_size > MAX_PACKAGE_BYTES for item in archive.infolist()) or
                            archive.read("firmware.bin") != firmware or
                            json.loads(archive.read("manifest.json")) != release.manifest()):
                        return None
                return payload
            output = io.BytesIO()
            with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
                archive.writestr("manifest.json", manifest)
                archive.writestr("firmware.bin", firmware)
            return output.getvalue()
        except (OSError, ValueError, KeyError, RuntimeError, zipfile.BadZipFile):
            return None

    def manifest_for(self, current_version: str, hardware_id: str) -> dict[str, object] | None:
        release = self.current()
        if not release or release.withdrawn or hardware_id != release.hardware:
            return None
        try:
            if release.minimum_version and _version_tuple(current_version) < _version_tuple(release.minimum_version):
                return None
            if _version_tuple(release.version) <= _version_tuple(current_version):
                return None
        except FirmwarePackageError:
            return None
        return release.manifest()

    def firmware_path(self, version: str) -> Path | None:
        release = self.current()
        if not release or release.withdrawn or release.version != version:
            return None
        path = self.root / "releases" / release.version / "firmware.bin"
        return path if path.is_file() and path.stat().st_size == release.size else None
