"""Small, dependency-free PCM front end for the isolated ASR experiment."""

from __future__ import annotations

from array import array
from dataclasses import asdict, dataclass
import json
import math
from pathlib import Path
import sys


@dataclass
class MicTuning:
    gain_mode: str = "auto"
    manual_gain: float = 1.0
    max_auto_gain: float = 12.0
    target_peak: float = 0.72
    trim_silence: bool = True
    vad_multiplier: float = 2.8
    vad_floor: int = 100
    noise_gate: int = 0
    pre_roll_ms: int = 240
    post_roll_ms: int = 420

    def validate(self) -> None:
        if self.gain_mode not in {"auto", "manual"}:
            raise ValueError("gain_mode must be auto or manual")
        if not 0.25 <= self.manual_gain <= 16.0:
            raise ValueError("manual_gain must be in 0.25..16")
        if not 1.0 <= self.max_auto_gain <= 24.0:
            raise ValueError("max_auto_gain must be in 1..24")
        if not 0.20 <= self.target_peak <= 0.90:
            raise ValueError("target_peak must be in 0.20..0.90")
        if not 1.2 <= self.vad_multiplier <= 8.0:
            raise ValueError("vad_multiplier must be in 1.2..8")
        if not 0 <= self.vad_floor <= 4000:
            raise ValueError("vad_floor must be in 0..4000")
        if not 0 <= self.noise_gate <= 4000:
            raise ValueError("noise_gate must be in 0..4000")

    @classmethod
    def load(cls, path: Path) -> "MicTuning":
        if not path.exists():
            return cls()
        values = json.loads(path.read_text(encoding="utf-8"))
        tuning = cls(**values)
        tuning.validate()
        return tuning

    def save(self, path: Path) -> None:
        self.validate()
        path.write_text(json.dumps(asdict(self), ensure_ascii=False, indent=2), encoding="utf-8")


def _rms(samples: list[int]) -> int:
    if not samples:
        return 0
    return int(round(math.sqrt(sum(sample * sample for sample in samples) / len(samples))))


def _pcm16_to_list(pcm: bytes) -> list[int]:
    if len(pcm) & 1:
        raise ValueError("PCM byte count must be even")
    values = array("h")
    values.frombytes(pcm)
    if sys.byteorder != "little":
        values.byteswap()
    return values.tolist()


def _list_to_pcm16(samples: list[int]) -> bytes:
    values = array("h", samples)
    if sys.byteorder != "little":
        values.byteswap()
    return values.tobytes()


def _trim_by_energy(samples: list[int], sample_rate: int, tuning: MicTuning) -> tuple[list[int], dict]:
    frame_samples = max(1, sample_rate * 20 // 1000)
    frame_rms = [_rms(samples[offset : offset + frame_samples]) for offset in range(0, len(samples), frame_samples)]
    if not frame_rms:
        return samples, {"vad_threshold": 0, "speech_found": False, "trimmed_ms": 0}

    # The quietest 20 percent is a more robust noise estimate than assuming
    # that the first frame is silent; some users start speaking immediately.
    quiet_count = max(1, len(frame_rms) // 5)
    noise_rms = _rms(sorted(frame_rms)[:quiet_count])
    threshold = max(tuning.vad_floor, int(noise_rms * tuning.vad_multiplier))
    active = [index for index, value in enumerate(frame_rms) if value >= threshold]
    if not active:
        return samples, {
            "noise_rms": noise_rms,
            "vad_threshold": threshold,
            "speech_found": False,
            "trimmed_ms": 0,
        }

    pre_frames = tuning.pre_roll_ms // 20
    post_frames = tuning.post_roll_ms // 20
    first = max(0, active[0] - pre_frames)
    last = min(len(frame_rms), active[-1] + post_frames + 1)
    start = first * frame_samples
    end = min(len(samples), last * frame_samples)
    trimmed = samples[start:end]
    return trimmed, {
        "noise_rms": noise_rms,
        "vad_threshold": threshold,
        "speech_found": True,
        "trimmed_ms": int(round((len(samples) - len(trimmed)) * 1000 / sample_rate)),
    }


def process_pcm(pcm: bytes, sample_rate: int, tuning: MicTuning) -> tuple[bytes, dict]:
    """Trim silence, remove DC, apply optional gate, and safely normalize PCM."""
    tuning.validate()
    samples = _pcm16_to_list(pcm)
    if not samples:
        raise ValueError("empty PCM")

    raw_peak = max(abs(sample) for sample in samples)
    raw_rms = _rms(samples)
    dc = int(round(sum(samples) / len(samples)))
    samples = [sample - dc for sample in samples]

    vad_stats = {"noise_rms": 0, "vad_threshold": 0, "speech_found": True, "trimmed_ms": 0}
    if tuning.trim_silence:
        samples, vad_stats = _trim_by_energy(samples, sample_rate, tuning)

    if tuning.noise_gate:
        samples = [0 if abs(sample) < tuning.noise_gate else sample for sample in samples]

    current_peak = max((abs(sample) for sample in samples), default=0)
    if tuning.gain_mode == "auto":
        wanted_peak = int(32767 * tuning.target_peak)
        gain = min(tuning.max_auto_gain, wanted_peak / max(1, current_peak))
        gain = max(0.25, gain)
    else:
        gain = tuning.manual_gain

    clipped = 0
    output: list[int] = []
    for sample in samples:
        scaled = int(round(sample * gain))
        if scaled > 32767:
            scaled = 32767
            clipped += 1
        elif scaled < -32768:
            scaled = -32768
            clipped += 1
        output.append(scaled)

    stats = {
        "raw_rms": raw_rms,
        "raw_peak": raw_peak,
        "dc": dc,
        "output_rms": _rms(output),
        "output_peak": max((abs(sample) for sample in output), default=0),
        "gain": round(gain, 3),
        "clipped_samples": clipped,
        "duration_ms": int(round(len(output) * 1000 / sample_rate)),
        **vad_stats,
    }
    return _list_to_pcm16(output), stats
