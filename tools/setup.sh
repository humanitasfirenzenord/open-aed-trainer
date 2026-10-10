#!/usr/bin/env bash
# One-time setup of the build and audio tooling on macOS (Homebrew).
#
#   tools/setup.sh            build tools + Kokoro voice engine
#   tools/setup.sh piper      build tools + Piper voice engine
#   tools/setup.sh none       build tools only (macOS `say` needs nothing)
set -euo pipefail
cd "$(dirname "$0")/.."
engine="${1:-kokoro}"

brew list platformio >/dev/null 2>&1 || brew install platformio
brew list ffmpeg >/dev/null 2>&1 || brew install ffmpeg

# Python venv for tools/gen_audio.py
PY=$(command -v python3.14 || command -v python3.13 || command -v python3.12 || command -v python3)
[ -d tools/.venv ] || "$PY" -m venv tools/.venv
tools/.venv/bin/pip install -q --upgrade pip

case "$engine" in
  kokoro)
    # Kokoro (Apache-2.0) needs Python <= 3.12 and espeak-ng for Italian
    brew list espeak-ng >/dev/null 2>&1 || brew install espeak-ng
    command -v uv >/dev/null || brew install uv
    [ -d tools/.venv-kokoro ] || uv venv -q -p 3.12 tools/.venv-kokoro
    VIRTUAL_ENV=tools/.venv-kokoro uv pip install -q kokoro soundfile
    ;;
  piper)
    tools/.venv/bin/pip install -q piper-tts
    mkdir -p tools/voices
    [ -f tools/voices/it_IT-paola-medium.onnx ] ||
      (cd tools/voices && ../.venv/bin/python -m piper.download_voices it_IT-paola-medium)
    ;;
  none) ;;
  *) echo "unknown engine '$engine' (kokoro | piper | none)"; exit 1 ;;
esac

# PlatformIO's mklittlefs (used by uploadfs) is x86_64 only on macOS:
# Apple Silicon needs Rosetta
if [ "$(uname -m)" = arm64 ] && ! arch -x86_64 /usr/bin/true 2>/dev/null; then
  echo "Rosetta is required for 'pio run -t uploadfs'. Install it with:"
  echo "  softwareupdate --install-rosetta"
fi

pio pkg install
echo "Setup done. Next:"
echo "  tools/.venv/bin/python tools/gen_audio.py    # render the voice prompts"
echo "  pio run -t uploadfs && pio run -t upload"
