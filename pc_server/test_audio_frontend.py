from __future__ import annotations

from array import array
import math
from pathlib import Path
import sys
import unittest

SERVER_DIR = Path(__file__).resolve().parent
if str(SERVER_DIR) not in sys.path:
    sys.path.insert(0, str(SERVER_DIR))

from audio_frontend import MicTuning, process_pcm


def pcm_bytes(samples: list[int]) -> bytes:
    return array("h", samples).tobytes()


class AudioFrontendTests(unittest.TestCase):
    def test_auto_gain_and_silence_trim(self) -> None:
        rate = 16000
        silence = [0] * rate
        voice = [int(1200 * math.sin(2 * math.pi * 440 * index / rate)) for index in range(rate)]
        processed, stats = process_pcm(pcm_bytes(silence + voice + silence), rate, MicTuning())
        self.assertTrue(stats["speech_found"])
        self.assertGreater(stats["trimmed_ms"], 1000)
        self.assertGreater(stats["output_peak"], 10000)
        self.assertLess(len(processed), len(pcm_bytes(silence + voice + silence)))

    def test_manual_gain_clips_safely(self) -> None:
        tuning = MicTuning(gain_mode="manual", manual_gain=16)
        processed, stats = process_pcm(pcm_bytes([30000, -30000] * 1000), 16000, tuning)
        self.assertEqual(len(processed), 4000)
        self.assertGreater(stats["clipped_samples"], 0)
        self.assertLessEqual(stats["output_peak"], 32768)


if __name__ == "__main__":
    unittest.main()
