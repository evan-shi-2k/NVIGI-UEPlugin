#!/usr/bin/env sh
set -eu

# Usage (from Linux or WSL):
#   ./run_nim.sh                # uses $NGC_API_KEY from env
#   NGC_API_KEY=... ./run_nim.sh
#   ./run_nim.sh YOUR_API_KEY   # pass key as first arg

# --- Resolve API key ---------------------------------------------------------
if [ "$#" -ge 1 ]; then
  NGC_API_KEY="$1"
elif [ -n "${NGC_API_KEY:-}" ]; then
  NGC_API_KEY="${NGC_API_KEY}"
else
  echo "Error: NGC_API_KEY not set. Set it in the environment or pass as first argument."
  exit 1
fi

# --- Move into ACE directory (where this script lives) -----------------------
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$SCRIPT_DIR"
echo "Running from ACE directory: $SCRIPT_DIR"

# --- TODO: Python venv in ACE/ace_venv --------------------------------------------
VENV_DIR="$SCRIPT_DIR/ace_venv"

if [ ! -d "$VENV_DIR" ]; then
  echo "Creating Python venv at $VENV_DIR..."
  python3 -m venv "$VENV_DIR"
fi

if [ -f "$VENV_DIR/bin/activate" ]; then
  . "$VENV_DIR/bin/activate"
elif [ -f "$VENV_DIR/Scripts/activate" ]; then
  . "$VENV_DIR/Scripts/activate"
else
  echo "Error: could not find venv activation script in $VENV_DIR."
  exit 1
fi

if [ ! -f "$VENV_DIR/.deps_installed" ]; then
  echo "Installing Python deps in venv..."
  python3 -m pip install --upgrade pip
  pip install openai
  touch "$VENV_DIR/.deps_installed"
else
  echo "Python deps already installed in venv."
fi

# --- Cache directory for NIM -------------------------------------------------
if [ -z "${LOCAL_NIM_CACHE:-}" ]; then
  LOCAL_NIM_CACHE="$HOME/.cache/nim"
fi

mkdir -p "$LOCAL_NIM_CACHE"
echo "Using cache directory: $LOCAL_NIM_CACHE"

# --- Isolated Docker config to avoid desktop.exe helper in WSL --------------
DOCKER_CONFIG="${DOCKER_CONFIG:-"$HOME/.docker-nim"}"
export DOCKER_CONFIG
mkdir -p "$DOCKER_CONFIG"

CONFIG_FILE="$DOCKER_CONFIG/config.json"
if [ ! -f "$CONFIG_FILE" ]; then
  printf '{ "auths": {} }\n' > "$CONFIG_FILE"
fi

# --- Docker login using password-stdin --------------------------------------
echo "Logging into nvcr.io..."
printf '%s\n' "$NGC_API_KEY" | docker login nvcr.io -u '$oauthtoken' --password-stdin

# --- Run NIM container -------------------------------------------------------
docker run -it --rm \
  --gpus all \
  --shm-size=16g \
  -e NGC_API_KEY \
  -e NIM_MAX_MODEL_LEN=65000 \
  -v "${LOCAL_NIM_CACHE}:/opt/nim/.cache" \
  -p 8000:8000 \
  nvcr.io/nim/meta/llama-3.2-3b-instruct:latest
