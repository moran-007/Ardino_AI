from __future__ import annotations

import threading
import uuid
from pathlib import Path

from .config import Settings
from .providers import build_asr, build_llms, build_tts, sentence_segments
from .store import Store


class VoiceService:
    def __init__(self, settings: Settings, store: Store, asr=None, llms=None, tts=None):
        self.settings = settings
        self.store = store
        self.semaphore = threading.BoundedSemaphore(settings.worker_concurrency)
        self.initialization_error = ""
        self.active_jobs = 0
        self._active_lock = threading.Lock()
        try:
            self.asr = asr or build_asr(settings)
            self.llms = llms or build_llms(settings)
            self.tts = tts or build_tts(settings)
        except Exception as exc:
            self.asr, self.llms, self.tts = None, [], None
            self.initialization_error = str(exc)

    @property
    def ready(self) -> bool:
        return not self.status_error

    @property
    def status_error(self) -> str:
        if self.initialization_error:
            return self.initialization_error
        if self.asr is None or self.tts is None:
            return "本地 ASR/TTS 未就绪"
        if not self.llms:
            return "没有启用任何 LLM Provider"
        usable = any(not hasattr(provider, "config") or bool(provider.config.api_key) for provider in self.llms)
        return "" if usable else "已启用的 LLM 尚未配置 API Key"

    def submit(self, device_id: str, pcm: bytes, simulated_text: str = "") -> str:
        return self._start_job(device_id, pcm, simulated_text)

    def submit_text(self, device_id: str, text: str) -> str:
        return self._start_job(device_id, b"", text.strip())

    def _start_job(self, device_id: str, pcm: bytes, input_text: str) -> str:
        job_id = uuid.uuid4().hex
        self.store.create_job(job_id, device_id)
        with self._active_lock:
            self.active_jobs += 1
        thread = threading.Thread(target=self._process, args=(job_id, device_id, pcm, input_text), name=f"voice-{job_id[:8]}", daemon=True)
        thread.start()
        return job_id

    def translate(self, text: str, target_language: str) -> tuple[str, str]:
        prompt = f"把下面内容翻译成{target_language}。只输出译文，不要解释。\n\n{text}"
        errors: list[str] = []
        for provider in self.llms:
            parts: list[str] = []
            try:
                parts.extend(provider.stream(prompt, "你是准确、自然的翻译助手。"))
                translated = "".join(parts).strip()
                if translated:
                    return translated, provider.name
                raise RuntimeError("模型返回空内容")
            except Exception as exc:
                errors.append(f"{provider.name}: {exc}")
                if parts:
                    break
        raise RuntimeError("；".join(errors) or "所有翻译模型均不可用")

    def _process(self, job_id: str, device_id: str, pcm: bytes, input_text: str) -> None:
        try:
            with self.semaphore:
                try:
                    self.store.update_job(job_id, status="transcribing")
                    question = input_text.strip() or self.asr.recognize(pcm, 16_000)
                    if not question:
                        raise RuntimeError("未识别到有效语音")
                    self.store.update_job(job_id, status="generating", question=question)
                    answer_parts: list[str] = []
                    segment_index = 0
                    selected_provider = ""
                    provider_errors: list[str] = []
                    device = self.store.get_device(device_id)
                    speed = float(device.get("tts_speed", 1.0) if device else 1.0)
                    chinese_speaker_id = int(device.get("tts_speaker_id", 0) if device else 0)
                    english_speaker_id = int(device.get("tts_english_speaker_id", 0) if device else 0)
                    segment_maximum = max(20, round(self.settings.segment_max_chars * min(speed, 1.0)))
                    memory_enabled = bool(device and device.get("memory_enabled"))
                    system_prompt = self.settings.system_prompt
                    if device and device.get("persona"):
                        system_prompt += "\n\n当前设备的专属角色设定：\n" + str(device["persona"])
                    model_question = question
                    if memory_enabled:
                        memories = self.store.list_memories(
                            device["device_id"], limit=int(device.get("memory_turns", 4))
                        )
                        if memories:
                            selected: list[str] = []
                            used_chars = 0
                            for item in reversed(memories):
                                entry = f"用户：{item['question']}\n助手：{item['answer']}"
                                remaining = 1600 - used_chars
                                if remaining <= 0:
                                    break
                                selected.append(entry[-remaining:])
                                used_chars += min(len(entry), remaining)
                            history = "\n".join(reversed(selected))
                            model_question = (
                                "下面是该设备最近的独立对话记忆，仅用于保持上下文。\n"
                                f"<history>\n{history}\n</history>\n"
                                f"当前用户问题：{question}"
                            )
                    for provider in self.llms:
                        yielded = False
                        try:
                            chunks = provider.stream(model_question, system_prompt)

                            def observed():
                                nonlocal yielded
                                for chunk in chunks:
                                    yielded = True
                                    yield chunk

                            for text in sentence_segments(observed(), self.settings.segment_min_chars, segment_maximum):
                                selected_provider = provider.name
                                answer_parts.append(text)
                                audio = self.tts.synthesize(
                                    text,
                                    speaker_id=chinese_speaker_id,
                                    english_speaker_id=english_speaker_id,
                                    speed=speed,
                                )
                                job_dir = self.settings.data_dir / "jobs" / job_id
                                job_dir.mkdir(parents=True, exist_ok=True)
                                audio_path = job_dir / f"segment-{segment_index:04d}.pcm"
                                audio_path.write_bytes(audio)
                                self.store.add_segment(job_id, segment_index, text, audio_path, len(audio), self.tts.sample_rate)
                                segment_index += 1
                            if yielded:
                                break
                            raise RuntimeError("模型返回空内容")
                        except Exception as exc:
                            provider_errors.append(f"{provider.name}: {exc}")
                            if yielded:
                                raise RuntimeError("模型在流式回答中途失败，已停止以避免混合厂商内容") from exc
                    if not answer_parts:
                        raise RuntimeError("；".join(provider_errors) or "所有模型均不可用")
                    answer = "".join(answer_parts)
                    if memory_enabled and device:
                        self.store.add_memory(device["device_id"], question, answer)
                    self.store.update_job(job_id, status="completed", answer=answer, provider=selected_provider)
                except Exception as exc:
                    self.store.update_job(job_id, status="failed", error=str(exc)[:500])
        finally:
            with self._active_lock:
                self.active_jobs = max(0, self.active_jobs - 1)
