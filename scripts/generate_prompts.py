"""Generate the two fixed device cues once, using the configured Xiaoyun voice.

Builds only read the checked-in PCM files; the device never calls TTS for cues.
This explicit authoring command uses the existing backend's cloud credentials.
"""
import asyncio
from array import array
import base64
from dataclasses import replace
import hashlib
import json
from pathlib import Path
import sys
import unicodedata
import wave

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'xiaoyun-realtime'))
from xiaoyun.config import Settings
from xiaoyun.provider import QwenProvider

PHRASES = {'wake': '我在。', 'sleep': '好的，我先退下了。'}


def words(text):
    return ''.join(c for c in text if not c.isspace() and not unicodedata.category(c).startswith('P'))


def pcm16k(raw):
    source = array('h', raw)
    if sys.byteorder != 'little':
        source.byteswap()
    # 24k -> 16k with a short anti-aliasing FIR, evaluated at 1.5-sample steps.
    from scipy.signal import resample_poly
    import numpy as np
    samples = resample_poly(np.asarray(source, dtype=np.float64), 2, 3)
    active = np.flatnonzero(np.abs(samples) > 180)
    if not len(active):
        raise RuntimeError('empty_prompt_audio')
    samples = samples[max(0, active[0]-320):min(len(samples), active[-1]+1600)]
    peak = np.max(np.abs(samples))
    if peak > 26000:
        samples *= 26000 / peak
    n = min(80, len(samples)//2)
    samples[:n] *= np.linspace(0, 1, n)
    samples[-n:] *= np.linspace(1, 0, n)
    return np.rint(samples).astype('<i2').tobytes()


async def generate(cfg, phrase):
    provider = QwenProvider(cfg)
    try:
        await provider.connect('你负责录制设备提示音。请只逐字朗读用户提供的那句话，语气自然、简短。不回答问题，不加任何其他词语。', [])
        await provider.send({'type': 'conversation.item.create', 'item': {
            'type': 'message', 'role': 'user', 'content': [{'type': 'input_text', 'text': phrase}]}})
        await provider.send({'type': 'response.create', 'response': {'modalities': ['text', 'audio']}})
        raw, text = bytearray(), ''
        async with asyncio.timeout(45):
            async for event in provider.events():
                kind = event['type']
                if kind == 'error':
                    raise RuntimeError('prompt_generation_rejected')
                if kind == 'response.audio.delta':
                    raw.extend(base64.b64decode(event['delta'], validate=True))
                elif kind == 'response.audio_transcript.delta':
                    text += event['delta']
                elif kind == 'response.audio_transcript.done':
                    text = event['transcript']
                elif kind == 'response.done':
                    if event['response'].get('status') != 'completed':
                        raise RuntimeError('prompt_generation_incomplete')
                    break
        if words(text) != words(phrase):
            raise RuntimeError('prompt_transcript_did_not_match')
        return pcm16k(raw)
    finally:
        await provider.close()


async def main():
    cfg = replace(Settings.load(), turn_detection='manual')
    target = ROOT / 'main' / 'prompts'
    target.mkdir(exist_ok=True)
    manifest = {'model': cfg.model, 'voice': cfg.voice, 'sample_rate': 16000, 'format': 'mono signed PCM16 little-endian', 'prompts': {}}
    for name, phrase in PHRASES.items():
        pcm = await generate(cfg, phrase)
        (target / (name+'.pcm')).write_bytes(pcm)
        preview = ROOT / '.local' / ('prompt-'+name+'.wav')
        preview.parent.mkdir(exist_ok=True)
        with wave.open(str(preview), 'wb') as wav:
            wav.setnchannels(1); wav.setsampwidth(2); wav.setframerate(16000); wav.writeframes(pcm)
        manifest['prompts'][name] = {'text': phrase, 'samples': len(pcm)//2, 'sha256': hashlib.sha256(pcm).hexdigest()}
        print(json.dumps({'prompt': name, 'duration_ms': round(len(pcm)/32), 'preview': str(preview)}, ensure_ascii=False), flush=True)
    (target / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')


if __name__ == '__main__':
    asyncio.run(main())
