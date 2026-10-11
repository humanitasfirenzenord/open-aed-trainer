#!/usr/bin/env python3
"""Render the AED trainer voice prompts to MP3.

Reads audio/prompts.json and writes into data/. That folder is flashed
to the internal LittleFS by `pio run -t uploadfs`:

    data/<lang>/<prompt_id>.mp3   voice prompts, one folder per language

Sound effects and the CPR metronome are synthesised by the firmware, and
any prompt can be replaced from the web UI ("Messaggi vocali").

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
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROMPTS = ROOT / "audio" / "prompts.json"
OUT = ROOT / "data"
LOCAL = ROOT / "audio" / "voice.local.json"
VOICES = ROOT / "tools" / "voices"
KOKORO_PY = ROOT / "tools" / ".venv-kokoro" / "bin" / "python"
KOKORO_HELPER = ROOT / "tools" / "kokoro_tts.py"

SAMPLE_RATE = 22050  # plenty for speech and small speakers
CHANNELS = 2  # identical L/R: the I2S driver handles stereo much faster than mono
BITRATE = "64k"


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


# ---------------------------------------------------------------- main ---

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine", choices=["kokoro", "piper", "say"], help="default: \"engine\" in prompts.json")
    ap.add_argument("--lang", action="append", help="language code(s), default all")
    ap.add_argument("--only", action="append", help="prompt/sfx id(s) to render")
    ap.add_argument("--force", action="store_true", help="re-render existing files")
    ap.add_argument("--play", action="store_true", help="play each file after rendering (macOS)")
    ap.add_argument("--out", type=Path, help="output folder (default: data/)")
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

        # Sound effects and the CPR cycle used to be files: now synthesised
        # by the firmware
        for old in [*(OUT / "sfx").glob("*.mp3"), *OUT.glob("*/cpr_cycle.mp3")]:
            old.unlink()
            print(f"  removed {old}")
        if (OUT / "sfx").is_dir() and not any((OUT / "sfx").iterdir()):
            (OUT / "sfx").rmdir()

        for lang in langs:
            lcfg = cfg["languages"][lang]
            # drop files of prompts that were removed from prompts.json
            keep = set(cfg["prompts"])
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

    Kokoro.close()
    print(f"Done, {done} file(s) written to {OUT}/")


if __name__ == "__main__":
    main()
