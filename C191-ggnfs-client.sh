#!/usr/bin/env bash
# C191-ggnfs-client.sh -- one-shot setup for a C191 ggnfs-distributed worker.
#
# Clones the repo, builds the client, installs a gnfs-lasieve4I15e binary
# matched to this CPU (AVX-512 build vs. generic), prompts for connection
# settings, and writes a run-client.sh that you can launch.
#
# Usage (host this on your webserver, then on each worker box):
#   curl -fsSL https://ecm.kyleaskine.com/C191-ggnfs-client.sh | bash
#   # or
#   wget -qO- https://ecm.kyleaskine.com/C191-ggnfs-client.sh | bash
#
# Or download and run directly:
#   wget https://ecm.kyleaskine.com/C191-ggnfs-client.sh && bash C191-ggnfs-client.sh

set -euo pipefail

REPO_URL="https://github.com/kyleaskine/ggnfs-distributed"
REPO_DIR="ggnfs-distributed"
SIEVER_NAME="gnfs-lasieve4I15e"
SIEVER_AVX_URL="https://ecm.kyleaskine.com/downloads/ggnfs/${SIEVER_NAME}"
SIEVER_NOIFMA_URL="https://ecm.kyleaskine.com/downloads/ggnfs-noifma/${SIEVER_NAME}"
SIEVER_NOAVX_URL="https://ecm.kyleaskine.com/downloads/ggnfs-noavx/${SIEVER_NAME}"

DEFAULT_SERVER="http://165.227.115.69:8081"
DEFAULT_TOKEN="a36cc0a79859b75dac75b874a810106bafa9f3ebf257d49306eac1b8771ff650"

# When invoked via `curl ... | bash`, stdin is the pipe and `read` would
# silently consume the rest of the script. Force interactive prompts to /dev/tty.
prompt_tty() {
    local var=$1 msg=$2 default=$3 reply
    printf '%s [%s]: ' "$msg" "$default" > /dev/tty
    IFS= read -r reply < /dev/tty || reply=""
    printf -v "$var" '%s' "${reply:-$default}"
}

missing_pkgs=""
need() {
    command -v "$1" >/dev/null 2>&1 || missing_pkgs="$missing_pkgs $2"
}
need_header() {
    # The client links -lgmp -lzstd; without dev packages make fails at link
    # time with a less obvious error. Catch it up front.
    [ -f "/usr/include/$1" ] || [ -f "/usr/include/x86_64-linux-gnu/$1" ] || missing_pkgs="$missing_pkgs $2"
}

need git           git
need make          build-essential
need cc            build-essential
need wget          wget
need_header gmp.h  libgmp-dev
need_header zstd.h libzstd-dev

if [ -n "$missing_pkgs" ]; then
    # Dedup while preserving order.
    pkgs=$(printf '%s\n' "$missing_pkgs" | tr ' ' '\n' | awk 'NF && !seen[$0]++' | tr '\n' ' ')
    echo "error: missing prerequisites" >&2
    echo "       Debian/Ubuntu: sudo apt-get install -y $pkgs" >&2
    exit 1
fi

echo "==> C191 ggnfs-distributed worker bootstrap"

# 1. Clone (or refresh)
if [ -d "$REPO_DIR/.git" ]; then
    echo "==> $REPO_DIR/ exists, pulling latest"
    git -C "$REPO_DIR" pull --ff-only
else
    echo "==> Cloning $REPO_URL"
    git clone "$REPO_URL" "$REPO_DIR"
fi
cd "$REPO_DIR"

# 2. Build client
if [ -x ./ggnfs-sieve-client ]; then
    echo "==> ggnfs-sieve-client already built, skipping make"
else
    echo "==> Building client"
    make client
fi

# 3. Pick the right siever for this CPU. Three tiers:
#   * AVX-512 + IFMA (Ice Lake / Sapphire Rapids / Zen4+): the full build,
#     compiled -march=native. gcc emits Ice-Lake-only ops (e.g. VBMI
#     vpermi2b/vpermt2b) so it SIGILLs on older AVX-512 parts.
#   * AVX-512 without IFMA (Skylake-SP/Platinum 61xx/81xx, Cascade/Cooper Lake):
#     the no-IFMA build -- the sieve compiled -march=skylake-avx512 (runs 16-wide),
#     with no Ice-Lake-only instructions. Runs the sieve in AVX-512 without SIGILL.
#   * No AVX-512 (Zen2/3, Broadwell, older laptops): generic scalar+asm build.
# IFMA is the Ice-Lake-or-newer gate: every CPU with avx512ifma also has the
# other modern sub-features (VBMI, ...); every AVX-512 CPU that lacks IFMA also
# lacks them, so it gets the Skylake build.
if grep -qw avx512f /proc/cpuinfo && grep -qw avx512ifma /proc/cpuinfo; then
    SIEVER_URL="$SIEVER_AVX_URL"
    SIEVER_DIST_PATH="dist/ggnfs/$SIEVER_NAME"
    echo "==> AVX-512 + IFMA detected; will use full AVX-512 siever"
elif grep -qw avx512f /proc/cpuinfo; then
    SIEVER_URL="$SIEVER_NOIFMA_URL"
    SIEVER_DIST_PATH="dist/ggnfs-noifma/$SIEVER_NAME"
    echo "==> AVX-512 without IFMA (Skylake-SP / Cascade Lake / Cooper Lake);"
    echo "    will use the AVX-512-sieve (no-IFMA) siever"
else
    SIEVER_URL="$SIEVER_NOAVX_URL"
    SIEVER_DIST_PATH="dist/ggnfs-noavx/$SIEVER_NAME"
    echo "==> No AVX-512; will use generic siever"
fi

if [ -x "./$SIEVER_NAME" ]; then
    echo "==> $SIEVER_NAME already present, skipping install"
elif [ -f "$SIEVER_DIST_PATH" ]; then
    echo "==> Installing $SIEVER_DIST_PATH"
    cp "$SIEVER_DIST_PATH" "$SIEVER_NAME"
    chmod +x "$SIEVER_NAME"
else
    echo "==> Downloading $SIEVER_URL"
    wget -q --show-progress -O "$SIEVER_NAME" "$SIEVER_URL"
    chmod +x "$SIEVER_NAME"
fi

# 4. Collect client config
echo
echo "==> Configure the worker (press Enter to accept defaults)"
prompt_tty CLIENT_ID "client id"        "$(hostname -s 2>/dev/null || echo worker)"
prompt_tty WORKERS   "workers"          "$(nproc 2>/dev/null || echo 4)"
prompt_tty SERVER    "server URL"       "$DEFAULT_SERVER"
prompt_tty TOKEN     "auth token"       "$DEFAULT_TOKEN"

# 5. Write run-client.sh -- exec replaces this shell so Ctrl-C lands on the
# client and its drain/release paths run.
ABS_DIR=$(pwd)
RUN_SCRIPT="$ABS_DIR/run-client.sh"
cat > "$RUN_SCRIPT" <<EOF
#!/usr/bin/env bash
cd "$ABS_DIR"
exec ./ggnfs-sieve-client \\
    --server-url="$SERVER" \\
    --token="$TOKEN" \\
    --siever="$ABS_DIR/$SIEVER_NAME" \\
    --workers="$WORKERS" \\
    --client-id="$CLIENT_ID"
EOF
chmod +x "$RUN_SCRIPT"

echo
echo "==> Done."
echo "    Start the worker:  $RUN_SCRIPT"
echo "    Ctrl-C once  = drain (finish in-flight work, return cleanly)"
echo "    Ctrl-C twice = cancel (release leases and exit)"
