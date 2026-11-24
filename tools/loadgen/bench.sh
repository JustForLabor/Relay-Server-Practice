#!/usr/bin/env bash
# bench.sh - 부하 측정 시나리오를 돌려 docs/report-data/에 회차별 원자료와 요약을 남긴다.
#
#   tools/loadgen/bench.sh selfcheck|s1|s2|all
#
# 호스트에서 실행하면 gcc:14 컨테이너를 tc 권한(NET_ADMIN)과 fd 상한 65536으로 띄우고 이
# 스크립트를 그 안에서 다시 실행한다. 서버와 loadgen은 같은 컨테이너(같은 기계, 같은 CPU)에서
# 돈다. 지연 · 손실은 tc netem으로 루프백(lo)에 건다. lo는 보내는 쪽과 받는 쪽이 같은 장치라
# 지연 · 손실이 서버로 갈 때와 돌아올 때 두 번 걸린다(지연 20ms = 왕복 약 40ms).
#
# 환경 변수: REPS(기본 3), DUR(측정 초, 기본 60), SC_DUR(selfcheck 측정 초, 기본 20),
#            OUT(기본 /src/docs/report-data)
set -uo pipefail

if [ ! -f /.dockerenv ]; then
    repo=$(cd "$(dirname "$0")/../.." && { pwd -W 2>/dev/null || pwd; })
    export MSYS_NO_PATHCONV=1
    exec docker run --rm --cap-add NET_ADMIN --ulimit nofile=65536:65536 \
        -e REPS -e DUR -e SC_DUR -e OUT -v "$repo:/src" -w /src gcc:14 \
        bash /src/tools/loadgen/bench.sh "$@"
fi

REPS=${REPS:-3}
DUR=${DUR:-60}
OUT=${OUT:-/src/docs/report-data}
PORT=7777
WARMUP=3
SRV=/tmp/rb/dist/relay-server
LG=/tmp/lg/loadgen

log() { echo "[bench $(date +%H:%M:%S)] $*" >&2; }

setup() {
    if ! command -v tc >/dev/null; then
        log "iproute2 설치"
        apt-get update -qq >/dev/null 2>&1 && apt-get install -y -qq iproute2 >/dev/null 2>&1 ||
            { log "iproute2 설치 실패"; exit 1; }
    fi
    # 호스트와 공유하는 build/를 건드리지 않게 산출물은 컨테이너의 /tmp에 둔다.
    # 서버는 배포본(make dist: release와 같은 -O2 목적 파일을 정적 링크)을 잰다.
    make -s -C /src B=/tmp/rb dist >/tmp/build.log 2>&1 || { cat /tmp/build.log; exit 1; }
    make -s -C /src/tools/loadgen O=/tmp/lg >>/tmp/build.log 2>&1 || { cat /tmp/build.log; exit 1; }
    mkdir -p "$OUT"
    write_env >"$OUT/env.txt"
}

write_env() {
    echo "date_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "cpu_model: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
    echo "cpu_cores_visible: $(nproc)"
    echo "mem_total_kb (Docker VM): $(grep MemTotal /proc/meminfo | awk '{print $2}')"
    echo "cgroup_memory_max: $(cat /sys/fs/cgroup/memory.max 2>/dev/null || echo unknown)"
    echo "kernel: $(uname -r)"
    echo "os: $(. /etc/os-release && echo "$PRETTY_NAME")"
    echo "gcc: $(gcc --version | head -1)"
    echo "server_build: make dist (-O2, sanitizer 없음, 정적 링크), same container as loadgen"
    echo "server_bin_sha256: $(sha256sum "$SRV" | cut -d' ' -f1)"
    # 커밋 전 작업 트리로 잴 수 있어 소스 내용의 해시를 함께 남긴다(커밋 해시만으로는 못 가린다).
    echo "server_src_sha256: $(find /src/server/src -name '*.[ch]' | sort | xargs cat | sha256sum | cut -d' ' -f1)"
    echo "loadgen_build: -O2, single thread epoll"
    echo "loadgen_src_sha256: $(cat /src/tools/loadgen/*.[ch] | sha256sum | cut -d' ' -f1)"
    echo "git_commit: $(git -c safe.directory=/src -C /src rev-parse --short HEAD 2>/dev/null || echo unknown)"
    echo "git_dirty_files: $(git -c safe.directory=/src -C /src status --porcelain 2>/dev/null | wc -l)"
    echo "nofile_limit: $(ulimit -n)"
    echo "netem: tc qdisc on lo (both directions pass lo, so delay and loss apply twice per round trip)"
}

netem() { # netem 지연ms 손실% : 둘 다 0이면 규칙을 지운다
    tc qdisc del dev lo root 2>/dev/null
    if [ "$1" != 0 ] || [ "$2" != 0 ]; then
        # limit: 기본 1000패킷이면 지연 동안 쌓인 패킷이 넘쳐 손실이 더해진다
        tc qdisc add dev lo root netem delay "${1}ms" loss "${2}%" limit 100000 ||
            { log "netem 설정 실패"; return 1; }
    fi
}

start_server() { # 폴더
    "$SRV" --port $PORT --stats-interval 1 --quiet >"$1/server_stats.jsonl" 2>"$1/server_err.log" &
    SPID=$!
    for _ in $(seq 50); do
        grep -q '^listening' "$1/server_stats.jsonl" 2>/dev/null && return 0
        kill -0 $SPID 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}

# run_one 시나리오 회차 모드 봇수 지연ms 손실% [추가 loadgen 인자...]
run_one() {
    local scen=$1 rep=$2 mode=$3 bots=$4 delay=$5 loss=$6
    shift 6
    local name d ramp status=ok lrc src la0 la1
    name=$(printf '%s_b%04d_d%02d_l%s_r%d' "$mode" "$bots" "$delay" "$loss" "$rep")
    d="$OUT/$scen/$name"
    rm -rf "$d" && mkdir -p "$d"
    # 접속을 초당 약 400개로 나눠 연다(SYN 대기열이 넘쳐 재시도 지연이 섞이지 않게)
    ramp=$(( bots / 400 )); [ "$ramp" -lt 2 ] && ramp=2
    netem "$delay" "$loss" || status=netem_failed
    if [ "$status" = ok ] && ! start_server "$d"; then
        status=server_start_failed
    fi
    # Docker VM 전체의 1 · 5 · 15분 부하. 같은 기계의 다른 컨테이너가 돌면 여기서 드러난다.
    la0=$(cut -d' ' -f1-3 /proc/loadavg)
    if [ "$status" = ok ]; then
        log "$scen $name 시작 (측정 ${DUR}s, ramp ${ramp}s, loadavg $la0)"
        timeout $((DUR + ramp + WARMUP + 60)) "$LG" --port $PORT --bots "$bots" --mode "$mode" \
            --duration "$DUR" --ramp "$ramp" --warmup $WARMUP --seed "$rep" \
            --out "$d/loadgen.json" "$@" 2>"$d/loadgen.log"
        lrc=$?
        if ! kill -0 $SPID 2>/dev/null; then
            wait $SPID; src=$?
            status="server_died_rc$src"
        else
            kill -TERM $SPID; wait $SPID; src=$?
            [ "$src" != 0 ] && status="server_rc$src"
        fi
        [ "$lrc" != 0 ] && [ "$status" = ok ] && status="loadgen_rc$lrc"
    fi
    la1=$(cut -d' ' -f1-3 /proc/loadavg)
    netem 0 0
    printf '{"scenario":"%s","run":"%s","rep":%d,"mode":"%s","bots":%d,"delay_ms":%d,"loss_pct":%s,"duration_s":%d,"ramp_s":%d,"warmup_s":%d,"status":"%s","extra_args":"%s","loadavg_start":"%s","loadavg_end":"%s"}\n' \
        "$scen" "$name" "$rep" "$mode" "$bots" "$delay" "$loss" "$DUR" "$ramp" "$WARMUP" \
        "$status" "$*" "$la0" "$la1" >"$d/meta.json"
    log "$scen $name 끝: $status $(tail -1 "$d/loadgen.log" 2>/dev/null)"
    sleep 2
}

selfcheck() {
    # 봇 2개(한 경기)로 도구가 상식적인 값을 내는지: 입력 반영 지연은 진행 간격 안팎,
    # netem 20ms를 넣으면 RTT가 약 40ms 늘어야 한다.
    local saved=$DUR
    DUR=${SC_DUR:-20}
    for rep in $(seq "$REPS"); do
        for mode in tcp udp; do
            for delay in 0 20; do
                run_one selfcheck "$rep" $mode 2 $delay 0
            done
        done
    done
    # 다시 하기 경로: 1점 경기를 여러 번 하는지, 다시 한 경기에서도 입력 지연이 그대로인지.
    # udp는 묶음에 앞 경기 입력이 섞이면 3진행 늦게 굳어서 따로 본다.
    for mode in tcp udp; do
        run_one selfcheck_rematch 1 $mode 2 0 0 --win-score 1
    done
    DUR=$saved
}

s1() {
    for rep in $(seq "$REPS"); do
        for bots in 100 500 1000 2000 4000; do
            run_one S1 "$rep" tcp $bots 0 0
        done
    done
}

s2() {
    for rep in $(seq "$REPS"); do
        for loss in 0 1 5; do
            for mode in tcp udp; do
                run_one S2 "$rep" $mode 200 20 $loss
            done
        done
    done
}

setup
for what in "$@"; do
    case $what in
    selfcheck) selfcheck ;;
    s1) s1 ;;
    s2) s2 ;;
    all) selfcheck; s1; s2 ;;
    *) log "알 수 없는 시나리오: $what"; exit 2 ;;
    esac
done
python3 /src/tools/loadgen/summarize.py "$OUT"
