#!/usr/bin/env bash
# setup-client.sh -- one bootstrap for a ggnfs-distributed worker, CPU or GPU.
#
# Replaces the old ggnfs-client.sh / cuda-client.sh pair. A box no longer has
# to know which one it is before it starts: with no --cpu/--gpu/--both this
# script looks at the hardware and asks. GPU prerequisites are only fatal when
# GPU was explicitly requested, so one URL is safe to curl|bash anywhere.
#
# It writes the runner scripts but does not run anything:
#
#   run-client.sh        benchmark.sh        (CPU)
#   run-cuda-client.sh   benchmark-gpu.sh    (GPU)
#
# Unlike its predecessors this file holds NO server address and NO token, so it
# is tracked in git like the rest of the repo. Deployments that want defaults
# baked in wrap it:
#
#   #!/usr/bin/env bash
#   exec setup-client.sh --server=http://host:8080 --token=<fleet key> "$@"
#
# The token is the FLEET key, not a per-job one: `init --token=@fleet.key`
# makes every job share it, so switching the fleet to a new factorization
# needs nothing done here. The siever is not baked in either -- all of
# gnfs-lasieve4I14e/I15e/I16e are installed and the coordinator names which
# one each job wants.
#
# Usage:
#   ./setup-client.sh                       # detect, then prompt
#   ./setup-client.sh --cpu --workers=12
#   ./setup-client.sh --gpu --device=0 --prefetch=2
#   ./setup-client.sh --both --server=http://host:8080 --token=<key> --yes

set -euo pipefail

REPO_URL="https://github.com/kyleaskine/ggnfs-distributed"
REPO_DIR="ggnfs-distributed"
SIEVER_DIR="sievers"
SIEVER_NAMES="gnfs-lasieve4I14e gnfs-lasieve4I15e gnfs-lasieve4I16e"

MODE=""
SERVER="${GGNFS_SERVER:-}"
TOKEN="${GGNFS_TOKEN:-}"
WORKERS=""
DEVICE=""
PREFETCH=""
CLIENT_ID=""
ASSUME_YES=0

usage() {
    # Written out rather than sed'd out of "$0": the documented way to run this
    # is `curl ... | bash -s -- --help`, where $0 is "bash" and there is no
    # file to read.
    cat <<'USAGE'
setup-client.sh -- set up a ggnfs-distributed worker, CPU or GPU.

  --cpu | --gpu | --both   what to set up. Default: detect, then ask.
                           --gpu is an error on a box that cannot run one;
                           auto-detect quietly falls back to CPU instead.
  --server=URL             coordinator, e.g. http://host:8080  (or $GGNFS_SERVER)
  --token=TOKEN            bearer token from <jobdir>/token    (or $GGNFS_TOKEN)
  --client-id=NAME         label for this box on the dashboard
  --workers=N              CPU sievers to run
  --device=N               CUDA device index (default 0)
  --prefetch=N             GPU lease slots (default 2)
  -y, --yes                take every default; never prompt
  -h, --help               this

Writes run-client.sh / benchmark.sh (CPU) and run-cuda-client.sh /
benchmark-gpu.sh (GPU). Runs nothing.
USAGE
    exit "${1:-0}"
}

for arg in "$@"; do
    case "$arg" in
        --cpu)          MODE="cpu" ;;
        --gpu)          MODE="gpu" ;;
        --both)         MODE="both" ;;
        --server=*)     SERVER="${arg#*=}" ;;
        --token=*)      TOKEN="${arg#*=}" ;;
        --workers=*)    WORKERS="${arg#*=}" ;;
        --device=*)     DEVICE="${arg#*=}" ;;
        --prefetch=*)   PREFETCH="${arg#*=}" ;;
        --client-id=*)  CLIENT_ID="${arg#*=}" ;;
        -y|--yes)       ASSUME_YES=1 ;;
        -h|--help)      usage 0 ;;
        *) echo "error: unknown option '$arg'" >&2; usage 2 ;;
    esac
done

# When invoked via `curl ... | bash`, stdin is the pipe and `read` would
# silently consume the rest of the script. Force prompts to /dev/tty, and fall
# back to the default when there is no tty (so --yes is not strictly required
# for a fully-flagged non-interactive run).
prompt_tty() {
    local var=$1 msg=$2 default=$3 reply
    if [ "$ASSUME_YES" = 1 ] || [ ! -e /dev/tty ]; then
        printf -v "$var" '%s' "$default"
        return
    fi
    printf '%s [%s]: ' "$msg" "$default" > /dev/tty
    IFS= read -r reply < /dev/tty || reply=""
    printf '\n' > /dev/tty
    reply=${reply//$'\r'/}
    printf -v "$var" '%s' "${reply:-$default}"
}

require_int() {
    local val=$1 name=$2 lo=$3 hi=$4
    case "$val" in
        ''|*[!0-9]*) echo "error: $name must be an integer in $lo..$hi" >&2; exit 1 ;;
    esac
    if [ "$val" -lt "$lo" ] || [ "$val" -gt "$hi" ]; then
        echo "error: $name must be an integer in $lo..$hi" >&2
        exit 1
    fi
}

# ---------------------------------------------------------------- prereqs ---
missing_pkgs=""
need() { command -v "$1" >/dev/null 2>&1 || missing_pkgs="$missing_pkgs $2"; }
need_header() {
    # The client links -lgmp -lzstd; without the dev packages make fails at
    # link time with a much less obvious error. Catch it up front.
    #
    # /usr/local/include is checked too: a box with a locally-built GMP (which
    # is common, since a tuned GMP is worth real sieving throughput) has no
    # /usr/include/gmp.h, and telling its owner to apt-get install something
    # they already have is worse than not checking at all.
    local d
    for d in /usr/include /usr/include/x86_64-linux-gnu /usr/local/include; do
        [ -f "$d/$1" ] && return 0
    done
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

# ------------------------------------------------------------ gpu capable ---
# Probed before the mode is settled so auto-detect has something to go on.
GPU_WHY=""
gpu_capable() {
    if ! command -v nvidia-smi >/dev/null 2>&1; then
        GPU_WHY="no NVIDIA driver (nvidia-smi not found)"; return 1
    fi
    if ! nvidia-smi --query-gpu=name --format=csv,noheader >/dev/null 2>&1; then
        GPU_WHY="nvidia-smi ran but reported no usable GPU"; return 1
    fi
    if ! command -v nvcc >/dev/null 2>&1; then
        GPU_WHY="no CUDA toolkit (nvcc not found; try export PATH=/usr/local/cuda/bin:\$PATH)"
        return 1
    fi
    return 0
}

HAVE_GPU=0
if gpu_capable; then HAVE_GPU=1; fi

echo "==> ggnfs-distributed worker setup"
if [ "$HAVE_GPU" = 1 ]; then
    echo "    GPU  : $(nvidia-smi --query-gpu=name,memory.total --format=csv,noheader | head -1)"
    echo "    CUDA : $(nvcc --version | sed -n 's/.*release \([0-9.]*\).*/\1/p' | head -1)"
else
    echo "    GPU  : none usable -- $GPU_WHY"
fi
echo "    CPU  : $(nproc 2>/dev/null || echo '?') cores"

# ------------------------------------------------------------------- mode ---
if [ -z "$MODE" ]; then
    if [ "$HAVE_GPU" = 1 ]; then
        prompt_tty MODE "Set up [c]pu, [g]pu or [b]oth?" "g"
    else
        MODE="cpu"
    fi
fi
case "$MODE" in
    c|cpu)  MODE="cpu" ;;
    g|gpu)  MODE="gpu" ;;
    b|both) MODE="both" ;;
    *) echo "error: mode must be cpu, gpu or both" >&2; exit 1 ;;
esac

# An EXPLICIT --gpu on a box that cannot run one is an error worth stopping
# for; the same situation reached by auto-detect is not, or a single curl|bash
# URL could never be used on a CPU-only box.
if [ "$MODE" != "cpu" ] && [ "$HAVE_GPU" = 0 ]; then
    echo "error: GPU setup requested but this box cannot run one: $GPU_WHY" >&2
    exit 1
fi
echo "==> Mode: $MODE"

# ------------------------------------------------------------ repo + build --
# Run from inside a checkout, or clone one next to us.
if [ -f "./Makefile" ] && [ -f "./client.c" ]; then
    echo "==> Using the checkout in $(pwd)"
elif [ -d "$REPO_DIR/.git" ]; then
    echo "==> $REPO_DIR/ exists, pulling latest"
    git -C "$REPO_DIR" pull --ff-only
    cd "$REPO_DIR"
else
    echo "==> Cloning $REPO_URL"
    git clone "$REPO_URL" "$REPO_DIR"
    cd "$REPO_DIR"
fi
ABS_DIR=$(pwd)

# Always build: a box that ran an older bootstrap has a binary predating
# --siever=<dir>, and skipping the build there produces a client that cannot
# follow a job change. make is incremental, so this is a no-op when current.
echo "==> Building client"
make client

# ------------------------------------------------------------- cpu sievers --
siever_executes() {
    # With no arguments the siever exits nonzero but normally; a binary too
    # new for this CPU dies on a signal (exit status >= 128).
    "$1" >/dev/null 2>&1 || [ $? -lt 128 ]
}

setup_cpu() {
    # Four microarchitecture tiers. IFMA is the Ice-Lake-or-newer gate: every
    # CPU with avx512ifma also has the other modern sub-features (VBMI, ...),
    # and every AVX-512 CPU lacking IFMA also lacks them.
    local tier
    if grep -qw avx512f /proc/cpuinfo && grep -qw avx512ifma /proc/cpuinfo; then
        tier="dist/ggnfs";          echo "==> AVX-512 + IFMA: full AVX-512 sievers"
    elif grep -qw avx512f /proc/cpuinfo; then
        tier="dist/ggnfs-noifma";   echo "==> AVX-512 without IFMA (Skylake-SP / Cascade / Cooper Lake)"
    elif grep -qw avx2 /proc/cpuinfo && grep -qw bmi2 /proc/cpuinfo \
            && grep -qw adx /proc/cpuinfo && grep -qw fma /proc/cpuinfo; then
        # "noavx" is only AVX-512-free; it still needs AVX2/FMA/BMI2/ADX.
        tier="dist/ggnfs-noavx";    echo "==> No AVX-512: generic AVX2 sievers"
    else
        tier="dist/ggnfs-portable"; echo "==> Pre-AVX2: portable baseline sievers"
    fi

    # ALL of them, not just one. Which siever a job needs is a property of the
    # job, and the coordinator names it per lease -- so installing the whole
    # set is what lets this box follow a switch to another factorization
    # without being touched.
    mkdir -p "$SIEVER_DIR"
    local name src
    for name in $SIEVER_NAMES; do
        src="$tier/$name"
        if [ ! -f "$src" ]; then
            echo "error: $src not found in the checkout" >&2
            echo "       (sievers ship in dist/; try 'git pull --ff-only')" >&2
            exit 1
        fi
        # Keep an installed binary only if it has the factor-base cache
        # trim/audit code AND actually runs here. A binary from a higher tier,
        # left by an older bootstrap, dies with SIGILL and must be replaced.
        if [ -x "$SIEVER_DIR/$name" ] &&
           grep -aq "Trimmed cached aFB" "$SIEVER_DIR/$name" &&
           siever_executes "$SIEVER_DIR/$name"; then
            echo "    $name: current"
            continue
        fi
        cp "$src" "$SIEVER_DIR/$name"
        chmod +x "$SIEVER_DIR/$name"
        if grep -aq "Trimmed cached aFB" "$SIEVER_DIR/$name"; then
            echo "    $name: installed"
        else
            # True of the whole portable tier. Say so, or the keep-check
            # reinstalling it on every run looks like a bug.
            echo "    $name: installed (no factor-base cache support in this"
            echo "                     build; it will be rebuilt per workunit)"
        fi
    done

    cat > run-client.sh <<EOF
#!/usr/bin/env bash
# Generated by setup-client.sh
set -euo pipefail
cd "\$(dirname "\$0")"
# --siever names a DIRECTORY: the coordinator says which binary each job
# needs, so this keeps working when the fleet moves to another factorization.
exec ./ggnfs-sieve-client \\
    --server-url="$SERVER" \\
    --token="$TOKEN" \\
    --siever="$ABS_DIR/$SIEVER_DIR" \\
    --client-id="$CLIENT_ID" \\
    --workers="$WORKERS" \\
    "\$@"
EOF
    chmod +x run-client.sh

    cat > benchmark.sh <<EOF
#!/usr/bin/env bash
# Generated by setup-client.sh -- screen this box before committing it.
# Takes no lease and never submits, so it is safe against a live coordinator.
# Exit 3 means throughput was below --min-rels-per-sec.
set -euo pipefail
cd "\$(dirname "\$0")"
exec ./ggnfs-sieve-client benchmark \\
    --server-url="$SERVER" \\
    --token="$TOKEN" \\
    --siever="$ABS_DIR/$SIEVER_DIR" \\
    "\$@"
EOF
    chmod +x benchmark.sh
}

# ----------------------------------------------------------------- gpu ------
setup_gpu() {
    local cuda_dir="" cand
    for cand in "../cuda-sieve" "$HOME/cuda-sieve" "$HOME/code/cuda-sieve"; do
        if [ -f "$cand/bench/Makefile" ]; then
            cuda_dir=$(cd "$cand" && pwd)
            break
        fi
    done
    if [ -z "$cuda_dir" ]; then
        local src
        prompt_tty src "cuda-sieve git URL or existing path" ""
        if [ -z "$src" ]; then
            echo "error: need a cuda-sieve checkout to build the GPU siever." >&2
            exit 1
        fi
        if [ -d "$src/bench" ]; then
            cuda_dir=$(cd "$src" && pwd)
        else
            git clone "$src" ../cuda-sieve
            cuda_dir=$(cd ../cuda-sieve && pwd)
        fi
    fi
    echo "==> cuda-sieve: $cuda_dir"

    local bench="$cuda_dir/bench/bench"
    local fbgen="$cuda_dir/bench/fbgen_gpu"

    # GPU_ARCH=native compiles only for the card in this box: minutes rather
    # than tens of minutes (ptxas is slow on the newest targets), and a rented
    # box never needs a portable binary.
    if [ -x "$bench" ]; then
        echo "==> bench already built, skipping"
    else
        echo "==> Building cuda-sieve bench (GPU_ARCH=native; a few minutes)"
        make -C "$cuda_dir/bench" GPU_ARCH=native
    fi
    if [ -x "$fbgen" ]; then
        echo "==> fbgen_gpu already built, skipping"
    else
        echo "==> Building fbgen_gpu (lets the client cache the factor base"
        echo "    once per job instead of rebuilding it every workunit)"
        make -C "$cuda_dir/bench" GPU_ARCH=native fbgen_gpu || true
    fi
    # Trust the artifact, not make's exit status: a target that is a no-op or
    # writes elsewhere exits 0 while leaving nothing here, and passing a
    # nonexistent --fbgen-gpu makes the client rebuild the ~230 MB factor base
    # on every workunit while the summary claims a cache is available.
    if [ ! -x "$fbgen" ]; then
        echo "warning: no fbgen_gpu binary; the client will build the factor" >&2
        echo "         base in-process on every workunit." >&2
        fbgen=""
    fi

    # Emitted into a bash ARRAY, not a backslash-continued command line: when
    # fbgen_gpu is absent that line is empty, and an empty line in the middle
    # of a `\`-continued command silently truncates it, dropping every
    # argument after it.
    local fbgen_arg=""
    [ -n "$fbgen" ] && fbgen_arg="  \"--fbgen-gpu=$fbgen\""

    cat > run-cuda-client.sh <<EOF
#!/usr/bin/env bash
# Generated by setup-client.sh
set -euo pipefail
cd "\$(dirname "\$0")"
args=(
  "--server-url=$SERVER"
  "--token=$TOKEN"
  --engine=cuda
  "--cuda-bench=$bench"
$fbgen_arg
  "--device=$DEVICE"
  "--prefetch=$PREFETCH"
  "--client-id=$CLIENT_ID-gpu$DEVICE"
)
exec ./ggnfs-sieve-client "\${args[@]}" "\$@"
EOF
    chmod +x run-cuda-client.sh

    cat > benchmark-gpu.sh <<EOF
#!/usr/bin/env bash
# Generated by setup-client.sh -- screen this box before committing it.
# Takes no lease and never submits, so it is safe against a live coordinator.
set -euo pipefail
cd "\$(dirname "\$0")"
args=(
  benchmark
  "--server-url=$SERVER"
  "--token=$TOKEN"
  --engine=cuda
  "--cuda-bench=$bench"
$fbgen_arg
  "--device=$DEVICE"
)
exec ./ggnfs-sieve-client "\${args[@]}" "\$@"
EOF
    chmod +x benchmark-gpu.sh
}

# ------------------------------------------------------------- settings -----
echo
echo "==> Coordinator settings"
[ -n "$SERVER" ] || prompt_tty SERVER "Server URL (http://host:port)" ""
[ -n "$TOKEN" ]  || prompt_tty TOKEN  "Auth token" ""
[ -n "$CLIENT_ID" ] || prompt_tty CLIENT_ID "Client id" "$(hostname -s 2>/dev/null || echo worker)"

if [ -z "$SERVER" ]; then
    echo "error: a server URL is required (--server=http://host:port)." >&2
    exit 1
fi
if [ -z "$TOKEN" ]; then
    echo "error: a token is required (--token=...; see <jobdir>/token)." >&2
    exit 1
fi

if [ "$MODE" = "cpu" ] || [ "$MODE" = "both" ]; then
    [ -n "$WORKERS" ] || prompt_tty WORKERS "Workers (CPU sievers)" "$(nproc 2>/dev/null || echo 4)"
    require_int "$WORKERS" "workers" 1 256
fi
if [ "$MODE" = "gpu" ] || [ "$MODE" = "both" ]; then
    [ -n "$DEVICE" ]   || prompt_tty DEVICE   "CUDA device index" "0"
    [ -n "$PREFETCH" ] || prompt_tty PREFETCH "Lease slots (prefetch)" "2"
    require_int "$DEVICE" "device" 0 255
    require_int "$PREFETCH" "prefetch" 1 8
fi

echo
case "$MODE" in
    cpu)  setup_cpu ;;
    gpu)  setup_gpu ;;
    both) setup_cpu; setup_gpu ;;
esac

# --------------------------------------------------------------- summary ----
echo
echo "==> Done."
echo "    server    : $SERVER"
echo "    client id : $CLIENT_ID"
if [ "$MODE" = "cpu" ] || [ "$MODE" = "both" ]; then
    echo
    echo "    CPU: $ABS_DIR/run-client.sh      # start sieving ($WORKERS workers)"
    echo "         $ABS_DIR/benchmark.sh       # screen the box first"
    echo "         sievers in $ABS_DIR/$SIEVER_DIR -- the coordinator picks one per job"
fi
if [ "$MODE" = "gpu" ] || [ "$MODE" = "both" ]; then
    echo
    echo "    GPU: $ABS_DIR/run-cuda-client.sh # start sieving (device $DEVICE, $PREFETCH slots)"
    echo "         $ABS_DIR/benchmark-gpu.sh   # screen the box first"
fi
echo
echo "    Ctrl-C once  = drain (finish in-flight work, return cleanly)"
echo "    Ctrl-C twice = cancel (release leases and exit)"
