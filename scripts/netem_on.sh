#!/usr/bin/env bash
set -euo pipefail

IFACE=""
PRESET="wan_mild"
DRY_RUN=0

DELAY_MS=""
JITTER_MS=""
LOSS_PCT=""
RATE=""
BURST="32kbit"
LATENCY_MS="50"
OVERRIDE_DELAY_MS=""
OVERRIDE_JITTER_MS=""
OVERRIDE_LOSS_PCT=""
OVERRIDE_RATE=""

usage() {
  cat <<'USAGE'
Usage: scripts/netem_on.sh --iface <name> [options]

Apply Linux tc/netem impairment profile to a network interface.
This script is Linux-only and intended for controlled benchmark experiments.

Required:
  --iface <name>          Network interface (example: eth0, ens5, lo)

Options:
  --preset <name>         Impairment preset (default: wan_mild)
  --delay-ms <ms>         Override one-way delay in ms
  --jitter-ms <ms>        Override jitter in ms
  --loss-pct <pct>        Override random loss percentage
  --rate <rate>           Optional bandwidth cap (example: 50mbit, 1gbit)
  --burst <size>          TBF burst when --rate is used (default: 32kbit)
  --latency-ms <ms>       TBF queue latency when --rate is used (default: 50)
  --dry-run               Print tc commands only
  --list-presets          Show preset catalog and exit
  -h, --help              Show help

Preset catalog:
  lan_clean      delay=0.25ms jitter=0.05ms loss=0.00% rate=none
  wan_mild       delay=15.0ms jitter=3.00ms loss=0.10% rate=none
  wan_noisy      delay=40.0ms jitter=10.0ms loss=0.50% rate=none
  constrained_50 delay=20.0ms jitter=5.00ms loss=0.20% rate=50mbit
USAGE
}

list_presets() {
  cat <<'PRESETS'
lan_clean delay_ms=0.25 jitter_ms=0.05 loss_pct=0.00 rate=none
wan_mild delay_ms=15.0 jitter_ms=3.00 loss_pct=0.10 rate=none
wan_noisy delay_ms=40.0 jitter_ms=10.0 loss_pct=0.50 rate=none
constrained_50 delay_ms=20.0 jitter_ms=5.00 loss_pct=0.20 rate=50mbit
PRESETS
}

require_option_value() {
  local opt="$1"
  if [[ $# -lt 2 || -z "${2:-}" || "${2:-}" == --* ]]; then
    echo "[netem_on] fail: ${opt} requires a value" >&2
    usage >&2
    exit 2
  fi
}

apply_preset() {
  case "${PRESET}" in
    lan_clean)
      DELAY_MS="0.25"
      JITTER_MS="0.05"
      LOSS_PCT="0.00"
      RATE=""
      ;;
    wan_mild)
      DELAY_MS="15.0"
      JITTER_MS="3.00"
      LOSS_PCT="0.10"
      RATE=""
      ;;
    wan_noisy)
      DELAY_MS="40.0"
      JITTER_MS="10.0"
      LOSS_PCT="0.50"
      RATE=""
      ;;
    constrained_50)
      DELAY_MS="20.0"
      JITTER_MS="5.00"
      LOSS_PCT="0.20"
      RATE="50mbit"
      ;;
    *)
      echo "[netem_on] fail: unknown preset '${PRESET}'" >&2
      echo "[netem_on] hint: use --list-presets to see available presets" >&2
      exit 2
      ;;
  esac
}

run_cmd() {
  if (( DRY_RUN == 1 )); then
    printf '[netem_on] dry-run:'
    printf ' %q' "$@"
    printf '\n'
    return 0
  fi
  "$@"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --iface)
      require_option_value "$1" "${2:-}"
      IFACE="$2"
      shift 2
      ;;
    --preset)
      require_option_value "$1" "${2:-}"
      PRESET="$2"
      shift 2
      ;;
    --delay-ms)
      require_option_value "$1" "${2:-}"
      OVERRIDE_DELAY_MS="$2"
      shift 2
      ;;
    --jitter-ms)
      require_option_value "$1" "${2:-}"
      OVERRIDE_JITTER_MS="$2"
      shift 2
      ;;
    --loss-pct)
      require_option_value "$1" "${2:-}"
      OVERRIDE_LOSS_PCT="$2"
      shift 2
      ;;
    --rate)
      require_option_value "$1" "${2:-}"
      OVERRIDE_RATE="$2"
      shift 2
      ;;
    --burst)
      require_option_value "$1" "${2:-}"
      BURST="$2"
      shift 2
      ;;
    --latency-ms)
      require_option_value "$1" "${2:-}"
      LATENCY_MS="$2"
      shift 2
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    --list-presets)
      list_presets
      exit 0
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "[netem_on] fail: unknown option '$1'" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ -z "${IFACE}" ]]; then
  echo "[netem_on] fail: --iface is required" >&2
  usage >&2
  exit 2
fi

if (( DRY_RUN == 0 )); then
  if [[ "$(uname -s)" != "Linux" ]]; then
    echo "[netem_on] fail: Linux-only script (tc/netem unavailable on this OS)" >&2
    exit 1
  fi

  if ! command -v tc >/dev/null 2>&1; then
    echo "[netem_on] fail: 'tc' not found; install iproute2" >&2
    exit 1
  fi

  if [[ ! -d "/sys/class/net/${IFACE}" ]]; then
    echo "[netem_on] fail: interface '${IFACE}' not found" >&2
    exit 1
  fi

  if (( EUID != 0 )); then
    echo "[netem_on] fail: root privileges required (run with sudo)" >&2
    exit 1
  fi
fi

apply_preset

if [[ -n "${OVERRIDE_DELAY_MS}" ]]; then
  DELAY_MS="${OVERRIDE_DELAY_MS}"
fi
if [[ -n "${OVERRIDE_JITTER_MS}" ]]; then
  JITTER_MS="${OVERRIDE_JITTER_MS}"
fi
if [[ -n "${OVERRIDE_LOSS_PCT}" ]]; then
  LOSS_PCT="${OVERRIDE_LOSS_PCT}"
fi
if [[ -n "${OVERRIDE_RATE}" ]]; then
  RATE="${OVERRIDE_RATE}"
fi

if [[ -n "${DELAY_MS}" ]]; then
  # Overriding delay implies explicit jitter default unless already set.
  if [[ -z "${JITTER_MS}" ]]; then
    JITTER_MS="0.00"
  fi
fi
if [[ -z "${LOSS_PCT}" ]]; then
  LOSS_PCT="0.00"
fi

if [[ -n "${RATE}" ]]; then
  run_cmd tc qdisc replace dev "${IFACE}" root handle 1: tbf \
    rate "${RATE}" burst "${BURST}" latency "${LATENCY_MS}ms"
  run_cmd tc qdisc replace dev "${IFACE}" parent 1:1 handle 10: netem \
    delay "${DELAY_MS}ms" "${JITTER_MS}ms" \
    loss "${LOSS_PCT}%"
else
  run_cmd tc qdisc replace dev "${IFACE}" root handle 1: netem \
    delay "${DELAY_MS}ms" "${JITTER_MS}ms" \
    loss "${LOSS_PCT}%"
fi

if (( DRY_RUN == 1 )); then
  echo "[netem_on] dry-run complete iface=${IFACE} preset=${PRESET} delay_ms=${DELAY_MS} jitter_ms=${JITTER_MS} loss_pct=${LOSS_PCT} rate=${RATE:-none}"
else
  echo "[netem_on] applied iface=${IFACE} preset=${PRESET} delay_ms=${DELAY_MS} jitter_ms=${JITTER_MS} loss_pct=${LOSS_PCT} rate=${RATE:-none}"
fi
