from __future__ import annotations

import argparse
import time
import wave
from pathlib import Path

import httpx


def run(base_url: str, device_id: str, token: str, text: str, output: Path, timeout: float = 90) -> dict:
    headers = {"X-Device-ID": device_id, "Authorization": f"Bearer {token}"}
    with httpx.Client(base_url=base_url.rstrip("/"), timeout=20) as client:
        response = client.post("/voice-api/v1/text-dialogue", headers=headers, json={"text": text})
        response.raise_for_status()
        job_id = response.json()["job_id"]
        deadline = time.monotonic() + timeout
        after = -1
        audio = bytearray()
        sample_rate = 16_000
        answer_parts: list[str] = []
        while time.monotonic() < deadline:
            poll = client.get(
                f"/voice-api/v1/jobs/{job_id}/segments", params={"after": after}, headers=headers
            )
            poll.raise_for_status()
            payload = poll.json()
            for segment in payload["segments"]:
                audio_response = client.get(segment["audio_url"], headers=headers)
                audio_response.raise_for_status()
                if len(audio_response.content) != segment["byte_count"]:
                    raise RuntimeError("分段音频长度与服务端声明不一致")
                audio.extend(audio_response.content)
                answer_parts.append(segment["text"])
                sample_rate = segment["sample_rate"]
                after = segment["index"]
                print(segment["text"], flush=True)
            if payload["status"] == "completed":
                break
            if payload["status"] == "failed":
                raise RuntimeError(payload.get("error") or "服务端任务失败")
            time.sleep(0.2)
        else:
            raise TimeoutError(f"任务 {job_id} 在 {timeout} 秒内未完成")
    output.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(output), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(audio)
    return {"job_id": job_id, "answer": "".join(answer_parts), "segments": after + 1, "output": str(output)}


def main() -> None:
    parser = argparse.ArgumentParser(description="电脑文字对话客户端")
    parser.add_argument("--url", default="http://voice.bsnlch.xyz")
    parser.add_argument("--device-id", required=True)
    parser.add_argument("--token", required=True)
    parser.add_argument("--text", default="你好，请用一句话介绍你自己")
    parser.add_argument("--output", type=Path, default=Path("cloud_server/output/text-answer.wav"))
    args = parser.parse_args()
    print(run(args.url, args.device_id, args.token, args.text, args.output))


if __name__ == "__main__":
    main()
