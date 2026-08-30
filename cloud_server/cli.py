from __future__ import annotations

import argparse
import getpass

from cryptography.fernet import Fernet

from .admin import hash_admin_password
from .config import Settings
from .store import Store


def main() -> None:
    parser = argparse.ArgumentParser(description="ESP32 cloud voice administration")
    subcommands = parser.add_subparsers(dest="command", required=True)
    register = subcommands.add_parser("register-device", help="register or rotate one device credential")
    register.add_argument("device_id")
    register.add_argument("--name", default="ESP32 voice device")
    subcommands.add_parser("hash-admin-password", help="prompt for an admin password and print its PBKDF2 hash")
    subcommands.add_parser("generate-config-key", help="generate a Fernet key for encrypted web configuration")
    args = parser.parse_args()
    if args.command == "hash-admin-password":
        password = getpass.getpass("管理员密码: ")
        confirm = getpass.getpass("再次输入: ")
        if password != confirm:
            raise SystemExit("两次密码不一致")
        print(hash_admin_password(password))
        return
    if args.command == "generate-config-key":
        print(Fernet.generate_key().decode("ascii"))
        return
    settings = Settings.from_env()
    store = Store(settings.database_path, settings.auth_pepper)
    if args.command == "register-device":
        token = store.register_device(args.device_id, args.name)
        print(f"device_id={args.device_id}")
        print(f"device_token={token}")
        print("此 token 只显示一次；请写入设备 NVS 或安全密码库。")


if __name__ == "__main__":
    main()
