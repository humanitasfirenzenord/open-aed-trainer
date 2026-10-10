#!/usr/bin/env python3
"""Render the AED trainer voice prompts and sound effects to MP3.

Reads audio/prompts.json and writes into sdcard/. That folder is flashed
to the internal LittleFS by `pio run -t uploadfs` (default build), or can
be copied to the root of a FAT32 micro-SD card (audiokit-sd build):

    sdcard/<lang>/<prompt_id>.mp3   voice prompts, one folder per language
    sdcard/<lang>/cpr_cycle.mp3     30 metronome beats + breath prompts
    sdcard/sfx/<sound_id>.mp3       language independent tones

Engines ("engine" in prompts.json, audio/voice.local.json, or --engine):
    kokoro  Kokoro open-weight TTS (Apache-2.0), offline. tools/setup.sh kokoro
    piper   Piper offline TTS. Check each voice's licence. tools/setup.sh piper
    say     macOS `say`. Empty "say" voice = the system voice. Apple's licence
            only allows personal, non-commercial use of these voices.

audio/voice.local.json (not committed) overrides "engine" and the
"languages" settings for your machine, e.g. {"engine": "piper"}.

Usage:
    tools/.venv/bin/python tools/gen_audio.py              # everything
    tools/.venv/bin/python tools/gen_audio.py --lang it    # one language
    tools/.venv/bin/python tools/gen_audio.py --only shock_advised --force
"""
import argparse
import json
import math
import shutil
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROMPTS = ROOT / "audio" / "prompts.json"
OUT = ROOT / "sdcard"
LOCAL = ROOT / "audio" / "voice.local.json"
VOICES = ROOT / "tools" / "voices"
KOKORO_PY = ROOT / "tools" / ".venv-kokoro" / "bin" / "python"
KOKORO_HELPER = ROOT / "tools" / "kokoro_tts.py"

SAMPLE_RATE = 22050  # plenty for speech and small speakers
CHANNELS = 2  # identical L/R: the I2S driver handles stereo much faster than mono
BITRATE = "64k"

# CPR cycle (ERC 2021: 30:2, 100-120/min, < 10 s pause for the breaths)
CPR_BPM = 110
BREATHS = 2
BREATH_INTERVAL = 3.0  # s between "give breath" prompts: ~1 s insufflation + exhalation


def run(cmd, **kw):
    subprocess.run(cmd, check=True, **kw)


def to_mp3(src: Path, dst: Path, pad_ms: int = 150):
    """Normalise loudness, add a little trailing silence and encode."""
    dst.parent.mkdir(parents=True, exist_ok=True)
    af = f"loudnorm=I=-16:TP=-1.5:LRA=11,apad=pad_dur={pad_ms / 1000}"
    run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(src),
         "-af", af, "-ac", str(CHANNELS), "-ar", str(SAMPLE_RATE),
         "-c:a", "libmp3lame", "-b:a", BITRATE, "-write_xing", "0", str(dst)])


class Kokoro:
    """One long-running helper process per voice (loading the model is slow)."""
    procs = {}

    @classmethod
    def say(cls, cfg: dict, text: str, wav: Path):
        if not KOKORO_PY.exists():
            sys.exit("Kokoro is not installed. Run: tools/setup.sh kokoro")
        key = (cfg.get("lang", "i"), cfg.get("voice", "if_sara"), str(cfg.get("speed", 1.0)))
        if key not in cls.procs:
            cls.procs[key] = subprocess.Popen(
                [str(KOKORO_PY), str(KOKORO_HELPER), *key],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        p = cls.procs[key]
        p.stdin.write(f"{wav}\t{' '.join(text.split())}\n")
        p.stdin.flush()
        reply = p.stdout.readline().strip()
        if reply != "ok":
            sys.exit(f"kokoro failed: {reply or 'helper exited'}")

    @classmethod
    def close(cls):
        for p in cls.procs.values():
            p.stdin.close()
            p.wait()


def tts(engine: str, lang_cfg: dict, text: str, wav: Path):
    if engine == "kokoro":
        Kokoro.say(lang_cfg.get("kokoro", {}), text, wav)
    elif engine == "piper":
        model = VOICES / f"{lang_cfg['piper']}.onnx"
        if not model.exists():
            sys.exit(f"Missing voice {model}. Run: tools/setup.sh")
        args = [sys.executable, "-m", "piper", "-m", str(model), "-f", str(wav)]
        if "length_scale" in lang_cfg:  # >1 = slower speech
            args += ["--length-scale", str(lang_cfg["length_scale"])]
        run(args, input=text.encode(), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    elif engine == "say":
        aiff = wav.with_suffix(".aiff")
        # No voice name = the macOS system voice (System Settings > Accessibility >
        # Spoken Content). Siri voices can only be used this way, not with -v.
        args = ["say", "-o", str(aiff)]
        if lang_cfg.get("say"):
            args += ["-v", lang_cfg["say"]]
        if "say_rate" in lang_cfg:  # words per minute
            args += ["-r", str(lang_cfg["say_rate"])]
        run(args + [text])
        run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(aiff), str(wav)])
    else:
        sys.exit(f"Unknown engine {engine}")


# ---------------------------------------------------------------- tones ---

def write_wav(path: Path, samples):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1, min(1, s)) * 32000)) for s in samples))


def read_wav(path: Path):
    with wave.open(str(path), "rb") as w:
        assert w.getnchannels() == 1 and w.getsampwidth() == 2
        data = w.readframes(w.getnframes())
    return [v / 32000 for v in struct.unpack(f"<{len(data) // 2}h", data)]


def envelope(i, n, attack=0.005, release=0.02):
    a, r = int(attack * SAMPLE_RATE), int(release * SAMPLE_RATE)
    if i < a:
        return i / a
    if i > n - r:
        return max(0.0, (n - i) / r)
    return 1.0


def tone(freq, dur, vol=0.8, harmonics=(1.0,)):
    n = int(dur * SAMPLE_RATE)
    out = []
    for i in range(n):
        t = i / SAMPLE_RATE
        s = sum(h * math.sin(2 * math.pi * freq * (k + 1) * t) for k, h in enumerate(harmonics))
        out.append(vol * envelope(i, n) * s / sum(harmonics))
    return out


def silence(dur):
    return [0.0] * int(dur * SAMPLE_RATE)


def sweep(f0, f1, dur, vol=0.6):
    n = int(dur * SAMPLE_RATE)
    out, phase = [], 0.0
    for i in range(n):
        f = f0 + (f1 - f0) * (i / n)
        phase += 2 * math.pi * f / SAMPLE_RATE
        out.append(vol * envelope(i, n) * math.sin(phase))
    return out


def sfx_bank():
    """Synthesised effects, roughly modelled on real AED sounds."""
    beep = tone(1000, 0.08, 0.9, (1.0, 0.3))
    period = 60.0 / 110  # 110 compressions per minute (ERC/AHA 100-120)
    return {
        # Short UI beep (power on, button acknowledge)
        "beep": tone(880, 0.15, 0.8, (1.0, 0.2)),
        # Power-on chime
        "power_on": tone(660, 0.12) + silence(0.04) + tone(990, 0.2),
        # Power-off descending chime
        "power_off": tone(990, 0.12) + silence(0.04) + tone(660, 0.25),
        # Shock delivered: thump + short burst
        "shock": sweep(180, 60, 0.18, 1.0) + tone(1200, 0.25, 0.7, (1, 0.5, 0.3)),
        # Disarm / internal discharge
        "disarm": sweep(1200, 300, 0.6, 0.5),
        # Single metronome tick and a 30-beat metronome block (for previews)
        "metronome": beep + silence(period - 0.08),
        # Alert / error tone (pads, motion...)
        "alert": (tone(600, 0.2) + silence(0.1)) * 3,
    }


def metronome_block(beats=30, bpm=CPR_BPM):
    period = 60.0 / bpm
    click = tone(1000, 0.08, 0.9, (1.0, 0.3))
    out = []
    for _ in range(beats):
        out += click + silence(period - len(click) / SAMPLE_RATE)
    return out


# ---------------------------------------------------------------- main ---

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine", choices=["kokoro", "piper", "say"], help="default: \"engine\" in prompts.json")
    ap.add_argument("--lang", action="append", help="language code(s), default all")
    ap.add_argument("--only", action="append", help="prompt/sfx id(s) to render")
    ap.add_argument("--force", action="store_true", help="re-render existing files")
    ap.add_argument("--play", action="store_true", help="play each file after rendering (macOS)")
    ap.add_argument("--out", type=Path, help="output folder (default: sdcard/)")
    args = ap.parse_args()

    global OUT
    if args.out:
        OUT = args.out.resolve()
    if not shutil.which("ffmpeg"):
        sys.exit("ffmpeg not found (brew install ffmpeg)")

    cfg = json.loads(PROMPTS.read_text())
    if LOCAL.exists():  # per-machine voice settings, not committed
        local = json.loads(LOCAL.read_text())
        cfg["engine"] = local.get("engine", cfg.get("engine"))
        for lang, over in local.get("languages", {}).items():
            cfg["languages"].setdefault(lang, {}).update(over)
    args.engine = args.engine or cfg.get("engine", "kokoro")
    print(f"engine: {args.engine}")
    langs = args.lang or list(cfg["languages"].keys())
    want = set(args.only) if args.only else None
    done = 0

    def need(key, dst):
        return (want is None or key in want) and (args.force or not dst.exists())

    def played(dst):
        nonlocal done
        done += 1
        print(f"  {dst}")
        if args.play:
            run(["afplay", str(dst)])

    with tempfile.TemporaryDirectory() as tmpd:
        tmp = Path(tmpd)

        print("Sound effects")
        bank = sfx_bank()
        for old in (OUT / "sfx").glob("*.mp3"):
            if old.stem not in bank:
                old.unlink()
                print(f"  removed {old}")
        for sid, samples in bank.items():
            dst = OUT / "sfx" / f"{sid}.mp3"
            if need(sid, dst):
                w = tmp / f"{sid}.wav"
                write_wav(w, samples)
                # tones are already at a good level; skip loudnorm so the
                # metronome keeps its exact length
                dst.parent.mkdir(parents=True, exist_ok=True)
                run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(w),
                     "-af", "apad=pad_dur=0.3",  # the decoder may drop the last frames
                     "-ac", str(CHANNELS), "-ar", str(SAMPLE_RATE), "-c:a", "libmp3lame", "-b:a", BITRATE, "-write_xing", "0",
                     str(dst)])
                played(dst)

        for lang in langs:
            lcfg = cfg["languages"][lang]
            # drop files of prompts that were removed from prompts.json
            keep = set(cfg["prompts"]) | {"cpr_cycle"}
            for old in (OUT / lang).glob("*.mp3"):
                if old.stem not in keep:
                    old.unlink()
                    print(f"  removed {old}")
            print(f"Voice prompts [{lang}] engine={args.engine}")
            for pid, texts in cfg["prompts"].items():
                if lang not in texts:
                    print(f"  ! {pid}: no '{lang}' text, skipped")
                    continue
                dst = OUT / lang / f"{pid}.mp3"
                if need(pid, dst):
                    w = tmp / f"{lang}_{pid}.wav"
                    tts(args.engine, lcfg, texts[lang], w)
                    to_mp3(w, dst)
                    played(dst)

            # CPR cycle, looped by the firmware for the whole CPR period:
            #   30 compressions at 110/min
            #   "give breaths" spoken once per breath, BREATH_INTERVAL apart
            #   then straight into the next 30 compressions
            dst = OUT / lang / "cpr_cycle.mp3"
            if need("cpr_cycle", dst) and "give_breaths" in cfg["prompts"]:
                def spoken(pid):
                    """TTS -> loudness-normalised mono samples at SAMPLE_RATE."""
                    raw, norm = tmp / f"{lang}_{pid}_raw.wav", tmp / f"{lang}_{pid}_n.wav"
                    tts(args.engine, lcfg, cfg["prompts"][pid][lang], raw)
                    run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(raw),
                         "-af", "loudnorm=I=-16:TP=-1.5:LRA=11", "-ac", "1", "-ar", str(SAMPLE_RATE),
                         "-sample_fmt", "s16", str(norm)])
                    return read_wav(norm)

                def padded(samples, seconds):
                    return samples + silence(max(0.0, seconds - len(samples) / SAMPLE_RATE))

                breath = spoken("give_breaths")
                cycle = metronome_block()
                for _ in range(BREATHS):
                    cycle += padded(breath, BREATH_INTERVAL)
                wav = tmp / f"{lang}_cpr.wav"
                write_wav(wav, cycle)
                dst.parent.mkdir(parents=True, exist_ok=True)
                run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(wav),
                     "-ac", str(CHANNELS), "-ar", str(SAMPLE_RATE), "-c:a", "libmp3lame", "-b:a", BITRATE, "-write_xing", "0",
                     str(dst)])
                played(dst)

    Kokoro.close()
    print(f"Done, {done} file(s) written to {OUT}/")


if __name__ == "__main__":
    main()
