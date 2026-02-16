#!/usr/bin/env bash
set -euo pipefail

IFACE=""
DRY_RUN=0
QUIET=0

usage() {
  cat <<'USAGE'
Usage: scripts/netem_off.sh --iface <name> [options]

Remove root qdisc/netem impairment from a Linux network interface.

Required:
  --iface <name>          Network interface (example: eth0, ens5, lo)

Options:
  --dry-run               Print tc command only
  --quiet                 Suppress warning when no root qdisc exists
  -h, --help              Show help
USAGE
}

require_option_value() {
  local opt="$1"
  if [[ $# -lt 2 || -z "${2:-}" || "${2:-}" == --* ]]; then
    echo "[netem_off] fail: ${opt} requires a value" >&2
    usage >&2
    exit 2
  fi
}

run_cmd() {
  if (( DRY_RUN == 1 )); then
    printf '[netem_off] dry-run:'
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
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    --quiet)
      QUIET=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "[netem_off] fail: unknown option '$1'" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ -z "${IFACE}" ]]; then
  echo "[netem_off] fail: --iface is required" >&2
  usage >&2
  exit 2
fi

if (( DRY_RUN == 0 )); then
  if [[ "$(uname -s)" != "Linux" ]]; then
    echo "[netem_off] fail: Linux-only script (tc/netem unavailable on this OS)" >&2
    exit 1
  fi

  if ! command -v tc >/dev/null 2>&1; then
    echo "[netem_off] fail: 'tc' not found; install iproute2" >&2
    exit 1
  fi

  if [[ ! -d "/sys/class/net/${IFACE}" ]]; then
    echo "[netem_off] fail: interface '${IFACE}' not found" >&2
    exit 1
  fi

  if (( EUID != 0 )); then
    echo "[netem_off] fail: root privileges required (run with sudo)" >&2
    exit 1
  fi
fi

if ! run_cmd tc qdisc del dev "${IFACE}" root; then
  if (( QUIET == 0 )); then
    echo "[netem_off] warn: no root qdisc removed for iface=${IFACE} (already clean or unsupported state)" >&2
  fi
else
  if (( DRY_RUN == 1 )); then
    echo "[netem_off] dry-run complete iface=${IFACE}"
  else
    echo "[netem_off] cleared root qdisc on iface=${IFACE}"
  fi
fi
