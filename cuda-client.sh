#!/usr/bin/env bash
# cuda-client.sh -- one-shot setup for a GPU ggnfs-distributed worker.
#
# The GPU counterpart to ggnfs-client.sh. Builds the sieve client, builds
# cuda-sieve's `bench` (and `fbgen_gpu`), prompts for connection settings, and
# writes run-cuda-client.sh plus benchmark-gpu.sh.
#
# Unlike the CPU path there is no prebuilt binary to drop in: `bench` is CUDA
# code and has to be compiled against the toolkit on the box. Budget a few
# minutes for it.
#
# Usage on a rented GPU box:
#   wget https://ecm.kyleaskine.com/downloads/cuda-client.sh && bash cuda-client.sh

set -euo pipefail

REPO_URL="https://github.com/kyleaskine/ggnfs-distributed"
REPO_DIR="ggnfs-distributed"

DEFAULT_SERVER="http://165.227.115.69:8080"
DEFAULT_TOKEN=""
DEFAULT_PREFETCH="2"
DEFAULT_DEVICE="0"

# When invoked via `curl ... | bash`, stdin is the pipe and `read` would
# silently consume the rest of the script. Force prompts to /dev/tty.
prompt_tty() {
    local var=$1 msg=$2 default=$3 reply
    printf '%s [%s]: ' "$msg" "$default" > /dev/tty
    IFS= read -r reply < /dev/tty || reply=""
    printf '\n' > /dev/tty
    reply=${reply//$'\r'/}
    printf -v "$var" '%s' "${reply:-$default}"
}

missing_pkgs=""
need() {
    command -v "$1" >/dev/null 2>&1 || missing_pkgs="$missing_pkgs $2"
}
need_header() {
    # The client links -lgmp -lzstd; without dev packages make fails at link
    # time with a less obvious error. Catch it up front.
    [ -f "/usr/include/$1" ] || [ -f "/usr/include/x86_64-linux-gnu/$1" ] ||
        missing_pkgs="$missing_pkgs $2"
}

need git           git
need make          build-essential
need cc            build-essential
need_header gmp.h  libgmp-dev
need_header zstd.h libzstd-dev

if [ -n "$missing_pkgs" ]; then
    pkgs=$(printf '%s\n' "$missing_pkgs" | tr ' ' '\n' | awk 'NF && !seen[$0]++' | tr '\n' ' ')
    echo "error: missing prerequisites" >&2
    echo "       Debian/Ubuntu: sudo apt-get install -y $pkgs" >&2
    exit 1
fi

echo "==> ggnfs-distributed GPU worker bootstrap"

# 1. The card and the toolkit, before anything expensive.
if ! command -v nvidia-smi >/dev/null 2>&1; then
    echo "error: nvidia-smi not found -- no NVIDIA driver on this box." >&2
    echo "       This is the GPU client; use ggnfs-client.sh for a CPU worker." >&2
    exit 1
fi
if ! nvidia-smi --query-gpu=name --format=csv,noheader >/dev/null 2>&1; then
    echo "error: nvidia-smi ran but reported no usable GPU." >&2
    exit 1
fi
echo "==> GPU: $(nvidia-smi --query-gpu=name,memory.total --format=csv,noheader | head -1)"

if ! command -v nvcc >/dev/null 2>&1; then
    echo "error: nvcc not found. cuda-sieve is CUDA source and must be compiled here." >&2
    echo "       Install the CUDA toolkit, or add it to PATH:" >&2
    echo "         export PATH=/usr/local/cuda/bin:\$PATH" >&2
    exit 1
fi
echo "==> CUDA: $(nvcc --version | sed -n 's/.*release \([0-9.]*\).*/\1/p' | head -1)"

# 2. Clone (or refresh) the coordinator client.
if [ -d "$REPO_DIR/.git" ]; then
    echo "==> $REPO_DIR/ exists, pulling latest"
    git -C "$REPO_DIR" pull --ff-only
else
    echo "==> Cloning $REPO_URL"
    git clone "$REPO_URL" "$REPO_DIR"
fi
cd "$REPO_DIR"

if [ -x ./ggnfs-sieve-client ]; then
    echo "==> ggnfs-sieve-client already built, skipping make"
else
    echo "==> Building client"
    make client
fi
CLIENT_DIR=$(pwd)

# 3. cuda-sieve. Prefer a checkout the operator already has; otherwise ask.
CUDA_SIEVE_DIR=""
for cand in "../cuda-sieve" "$HOME/cuda-sieve" "$HOME/code/cuda-sieve"; do
    if [ -f "$cand/bench/Makefile" ]; then
        CUDA_SIEVE_DIR=$(cd "$cand" && pwd)
        break
    fi
done
if [ -z "$CUDA_SIEVE_DIR" ]; then
    prompt_tty CUDA_SIEVE_SRC "cuda-sieve git URL or existing path" ""
    if [ -z "$CUDA_SIEVE_SRC" ]; then
        echo "error: need a cuda-sieve checkout to build the GPU siever." >&2
        exit 1
    fi
    if [ -d "$CUDA_SIEVE_SRC/bench" ]; then
        CUDA_SIEVE_DIR=$(cd "$CUDA_SIEVE_SRC" && pwd)
    else
        git clone "$CUDA_SIEVE_SRC" ../cuda-sieve
        CUDA_SIEVE_DIR=$(cd ../cuda-sieve && pwd)
    fi
fi
echo "==> cuda-sieve: $CUDA_SIEVE_DIR"

BENCH="$CUDA_SIEVE_DIR/bench/bench"
FBGEN="$CUDA_SIEVE_DIR/bench/fbgen_gpu"

# GPU_ARCH=native compiles only for the card in this box. That is minutes
# rather than tens of minutes (ptxas is slow on the newest targets), and a
# rented box never needs a portable binary.
if [ -x "$BENCH" ]; then
    echo "==> bench already built, skipping"
else
    echo "==> Building cuda-sieve bench (GPU_ARCH=native; a few minutes)"
    make -C "$CUDA_SIEVE_DIR/bench" GPU_ARCH=native
fi
if [ -x "$FBGEN" ]; then
    echo "==> fbgen_gpu already built, skipping"
else
    echo "==> Building fbgen_gpu (lets the client cache the factor base once"
    echo "    per job instead of rebuilding it on every workunit)"
    make -C "$CUDA_SIEVE_DIR/bench" GPU_ARCH=native fbgen_gpu || {
        echo "warning: fbgen_gpu failed to build; the client will fall back to" >&2
        echo "         building the factor base in-process on every workunit." >&2
        FBGEN=""
    }
fi

# 4. Connection settings.
prompt_tty SERVER_URL "Server URL"       "$DEFAULT_SERVER"
prompt_tty TOKEN      "Auth token"       "$DEFAULT_TOKEN"
prompt_tty DEVICE     "CUDA device index" "$DEFAULT_DEVICE"
prompt_tty PREFETCH   "Lease slots (prefetch)" "$DEFAULT_PREFETCH"

if [ -z "$TOKEN" ]; then
    echo "error: a token is required (see <jobdir>/token on the server)." >&2
    exit 1
fi
case "$PREFETCH" in
    ''|*[!0-9]*) echo "error: prefetch must be an integer in 1..8" >&2; exit 1 ;;
esac
if [ "$PREFETCH" -lt 1 ] || [ "$PREFETCH" -gt 8 ]; then
    echo "error: prefetch must be an integer in 1..8" >&2
    exit 1
fi
case "$DEVICE" in
    ''|*[!0-9]*) echo "error: device must be a non-negative integer" >&2; exit 1 ;;
esac

CLIENT_ID="$(hostname)-gpu$DEVICE"

# Emitted into a bash ARRAY below, not spliced into a backslash-continued
# command line: when fbgen_gpu is absent this line is empty, and an empty line
# in the middle of a `\`-continued command silently truncates it -- dropping
# every argument after it.
fbgen_arg=""
[ -n "$FBGEN" ] && fbgen_arg="  --fbgen-gpu=$FBGEN"

# 5. Runner + benchmark wrapper.
cat > run-cuda-client.sh <<EOF
#!/usr/bin/env bash
# Generated by cuda-client.sh
set -euo pipefail
cd "\$(dirname "\$0")"
args=(
  --server-url=$SERVER_URL
  --token=$TOKEN
  --engine=cuda
  --cuda-bench=$BENCH
$fbgen_arg
  --device=$DEVICE
  --prefetch=$PREFETCH
  --client-id=$CLIENT_ID
)
exec ./ggnfs-sieve-client "\${args[@]}" "\$@"
EOF
chmod +x run-cuda-client.sh

cat > benchmark-gpu.sh <<EOF
#!/usr/bin/env bash
# Generated by cuda-client.sh -- screen this box before committing to it.
# Takes no lease and never submits, so it is safe against a live coordinator.
# Exit 3 means throughput was below --min-rels-per-sec.
set -euo pipefail
cd "\$(dirname "\$0")"
args=(
  benchmark
  --server-url=$SERVER_URL
  --token=$TOKEN
  --engine=cuda
  --cuda-bench=$BENCH
$fbgen_arg
  --device=$DEVICE
)
exec ./ggnfs-sieve-client "\${args[@]}" "\$@"
EOF
chmod +x benchmark-gpu.sh

echo
echo "==> Ready."
echo "    $CLIENT_DIR/run-cuda-client.sh   # start sieving"
echo "    $CLIENT_DIR/benchmark-gpu.sh     # screen the box first"
echo
echo "    client id : $CLIENT_ID"
echo "    device    : $DEVICE, $PREFETCH lease slots"
[ -z "$FBGEN" ] && echo "    NOTE: no fbgen_gpu; the factor base is rebuilt per workunit."

prompt_tty RUN_BENCH "Run the benchmark now? (y/N)" "N"
case "$RUN_BENCH" in
    [yY]*) ./benchmark-gpu.sh ;;
    *) echo "Skipping benchmark." ;;
esac
