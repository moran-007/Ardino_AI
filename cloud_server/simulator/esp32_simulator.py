from __future__ import annotations

import argparse
import base64
import time
import wave
from pathlib import Path

import httpx


def run(base_url: str, device_id: str, token: str, question: str, output: Path, timeout: float = 30) -> dict:
    simulated_text = base64.urlsafe_b64encode(question.encode("utf-8")).decode("ascii")
    headers = {"X-Device-ID": device_id, "Authorization": f"Bearer {token}", "Content-Type": "audio/L16", "X-Simulated-Text-B64": simulated_text}
    pcm = bytes(16_000 * 2)  # one second of silence; dev server uses X-Simulated-Text
    with httpx.Client(base_url=base_url.rstrip("/"), timeout=10) as client:
        response = client.post("/voice-api/v1/dialogue", headers=headers, content=pcm)
        response.raise_for_status()
        job_id = response.json()["job_id"]
        after = -1
        audio = bytearray()
        sample_rate = 16_000
        deadline = time.monotonic() + timeout
        status = "queued"
        while time.monotonic() < deadline:
            poll = client.get(f"/voice-api/v1/jobs/{job_id}/segments", params={"after": after}, headers=headers)
            poll.raise_for_status()
            payload = poll.json()
            status = payload["status"]
            for segment in payload["segments"]:
                segment_response = client.get(segment["audio_url"], headers=headers)
                segment_response.raise_for_status()
                if len(segment_response.content) != segment["byte_count"]:
                    raise RuntimeError("segment Content-Length mismatch")
                audio.extend(segment_response.content)
                sample_rate = segment["sample_rate"]
                after = segment["index"]
                print(f"segment {after}: {segment['text']}")
            if status == "completed":
                break
            if status == "failed":
                raise RuntimeError(payload.get("error") or "job failed")
            time.sleep(0.15)
        else:
            raise TimeoutError(f"job {job_id} did not finish in {timeout}s")
    output.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(output), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(audio)
    return {"job_id": job_id, "status": status, "segments": after + 1, "bytes": len(audio), "output": str(output)}


def main() -> None:
    parser = argparse.ArgumentParser(description="simulate the ESP32 cloud streaming protocol")
    parser.add_argument("--url", default="http://127.0.0.1:18765")
    parser.add_argument("--device-id", required=True)
    parser.add_argument("--token", required=True)
    parser.add_argument("--question", default="请用两句话介绍你自己")
    parser.add_argument("--output", type=Path, default=Path("cloud_server/output/simulator-answer.wav"))
    args = parser.parse_args()
    print(run(args.url, args.device_id, args.token, args.question, args.output))


if __name__ == "__main__":
    main()
