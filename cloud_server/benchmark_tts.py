from __future__ import annotations

import argparse
import ctypes
import json
import os
import statistics
import time
import wave
from dataclasses import replace
from pathlib import Path

from .config import Settings
from .providers import SherpaVitsTTS


def process_rss_mb() -> float | None:
    if os.name == "nt":
        class Counters(ctypes.Structure):
            _fields_ = [
                ("cb", ctypes.c_ulong),
                ("PageFaultCount", ctypes.c_ulong),
                ("PeakWorkingSetSize", ctypes.c_size_t),
                ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t),
                ("PeakPagefileUsage", ctypes.c_size_t),
            ]

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        psapi = ctypes.WinDLL("psapi", use_last_error=True)
        kernel32.GetCurrentProcess.restype = ctypes.c_void_p
        psapi.GetProcessMemoryInfo.argtypes = [ctypes.c_void_p, ctypes.POINTER(Counters), ctypes.c_ulong]
        psapi.GetProcessMemoryInfo.restype = ctypes.c_int
        counters = Counters()
        counters.cb = ctypes.sizeof(counters)
        if psapi.GetProcessMemoryInfo(kernel32.GetCurrentProcess(), ctypes.byref(counters), counters.cb):
            return counters.WorkingSetSize / (1024 * 1024)
        return None
    try:
        import resource

        value = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        return value / (1024 * 1024) if os.uname().sysname == "Darwin" else value / 1024
    except Exception:
        return None


def write_wav(path: Path, pcm: bytes, sample_rate: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(sample_rate)
        output.writeframes(pcm)


def main() -> None:
    parser = argparse.ArgumentParser(description="benchmark a local sherpa-onnx VITS model")
    parser.add_argument("--model-dir", type=Path, required=True)
    parser.add_argument("--speaker-ids", default="0", help="comma-separated speaker IDs")
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--text", default="你好，我是运行在本地服务器上的语音助手。这是一段完全免费的语音合成性能测试。")
    parser.add_argument("--output-dir", type=Path, default=Path("cloud_server/output/tts-benchmark"))
    args = parser.parse_args()
    if args.rounds < 1 or args.rounds > 20:
        parser.error("--rounds must be between 1 and 20")
    model_dir = args.model_dir.resolve()
    speaker_ids = [int(value.strip()) for value in args.speaker_ids.split(",") if value.strip()]
    base = Settings.from_env()
    results = []
    for speaker_id in speaker_ids:
        settings = replace(
            base,
            sherpa_tts_model=model_dir / "model.onnx",
            sherpa_tts_tokens=model_dir / "tokens.txt",
            sherpa_tts_lexicon=model_dir / "lexicon.txt",
            sherpa_tts_data_dir=None,
            sherpa_tts_speaker_id=speaker_id,
        )
        before_mb = process_rss_mb()
        load_started = time.perf_counter()
        tts = SherpaVitsTTS(settings)
        load_seconds = time.perf_counter() - load_started
        after_load_mb = process_rss_mb()
        rounds = []
        for round_index in range(args.rounds):
            started = time.perf_counter()
            pcm = tts.synthesize(args.text)
            elapsed = time.perf_counter() - started
            audio_seconds = len(pcm) / (tts.sample_rate * 2)
            rounds.append({"round": round_index + 1, "wall_seconds": round(elapsed, 3), "audio_seconds": round(audio_seconds, 3), "rtf": round(elapsed / audio_seconds, 3)})
            if round_index == 0:
                write_wav(args.output_dir / f"speaker-{speaker_id}.wav", pcm, tts.sample_rate)
        results.append(
            {
                "speaker_id": speaker_id,
                "load_seconds": round(load_seconds, 3),
                "rss_before_mb": round(before_mb, 1) if before_mb is not None else None,
                "rss_after_load_mb": round(after_load_mb, 1) if after_load_mb is not None else None,
                "mean_rtf": round(statistics.mean(item["rtf"] for item in rounds), 3),
                "worst_rtf": max(item["rtf"] for item in rounds),
                "rounds": rounds,
            }
        )
    print(json.dumps({"model_dir": str(model_dir), "sample_rate": 16000, "results": results}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
