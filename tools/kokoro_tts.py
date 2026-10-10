#!/usr/bin/env python3
"""Kokoro TTS helper for gen_audio.py (runs in tools/.venv-kokoro).

Kokoro (https://github.com/hexgrad/kokoro) is an open-weight TTS model
released under the Apache-2.0 licence, so the audio it produces can be
shared. It needs Python <= 3.12, hence its own virtualenv.

Protocol: loads the model once, then reads one job per line on stdin
    <output.wav>\t<text>
and answers "ok" (or "error: ...") on stdout for each job.

Usage: kokoro_tts.py <lang_code> <voice> <speed>
    e.g. kokoro_tts.py i if_sara 1.0     (i = Italian)
"""
import os
import sys
import warnings

warnings.filterwarnings("ignore")
os.environ.setdefault("TOKENIZERS_PARALLELISM", "false")


def setup_espeak():
    # Kokoro's Italian phonemizer uses espeak-ng. The copy bundled with
    # espeakng_loader has a broken data path on macOS: prefer a system install.
    from phonemizer.backend.espeak.wrapper import EspeakWrapper

    for lib, data in [
        ("/opt/homebrew/lib/libespeak-ng.dylib", "/opt/homebrew/share/espeak-ng-data"),
        ("/usr/local/lib/libespeak-ng.dylib", "/usr/local/share/espeak-ng-data"),
        ("/usr/lib/x86_64-linux-gnu/libespeak-ng.so.1", "/usr/lib/x86_64-linux-gnu/espeak-ng-data"),
        ("/usr/lib/aarch64-linux-gnu/libespeak-ng.so.1", "/usr/lib/aarch64-linux-gnu/espeak-ng-data"),
    ]:
        if os.path.exists(lib) and os.path.isdir(data):
            EspeakWrapper.set_library(lib)
            EspeakWrapper.set_data_path(data)
            return


def main():
    lang, voice, speed = sys.argv[1], sys.argv[2], float(sys.argv[3])
    import numpy as np
    import soundfile as sf
    from kokoro import KPipeline

    pipeline = KPipeline(lang_code=lang, repo_id="hexgrad/Kokoro-82M")
    setup_espeak()  # after the import: kokoro sets its own paths on import

    for line in sys.stdin:
        line = line.rstrip("\n")
        if not line:
            continue
        try:
            out, text = line.split("\t", 1)
            chunks = [audio for _, _, audio in pipeline(text, voice=voice, speed=speed)]
            sf.write(out, np.concatenate(chunks), 24000)
            print("ok", flush=True)
        except Exception as e:  # report and keep serving
            print(f"error: {e}", flush=True)


if __name__ == "__main__":
    main()
