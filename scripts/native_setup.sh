#!/usr/bin/env bash
# One-time setup for a native Linux session (e.g. Ubuntu live USB). Needs sudo + network.
# Installs the toolchain and perf, then sets benchmark-friendly CPU/perf settings.
# Everything here is undone by a reboot (live sessions keep nothing unless persistence is on).
# usage: scripts/native_setup.sh
set -uo pipefail

say()  { printf '\n== %s\n' "$*"; }
warn() { printf 'WARN: %s\n' "$*"; }
set_all() {  # value, glob: write value to every matching sysfs file that exists
  local value=$1 f n=0; shift
  for f in "$@"; do [ -e "$f" ] || continue; echo "$value" | sudo tee "$f" >/dev/null && n=$((n + 1)); done
  echo "$n"
}

if grep -qi microsoft /proc/sys/kernel/osrelease; then
  warn "this is WSL2, not native Linux; perf and governor steps will not work here"
fi

say "Packages (universe is needed for clang/valgrind on the live ISO)"
sudo add-apt-repository -y universe >/dev/null 2>&1 || warn "could not enable universe"
sudo apt-get update -qq
sudo apt-get install -y build-essential g++ clang cmake git valgrind gdb util-linux
if ! sudo apt-get install -y linux-tools-common linux-tools-generic "linux-tools-$(uname -r)"; then
  warn "linux-tools-$(uname -r) not available: perf will not work on this kernel"
fi

say "perf permissions"
sudo sysctl -w kernel.perf_event_paranoid=1
sudo sysctl -w kernel.kptr_restrict=0   # kernel symbols in perf record

say "CPU frequency policy"
echo "scaling driver: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_driver 2>/dev/null || echo none)"
if command -v powerprofilesctl >/dev/null; then
  powerprofilesctl set performance && echo "power profile: $(powerprofilesctl get)"
fi
echo "governor=performance on $(set_all performance /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor) CPUs"
echo "EPP=performance on $(set_all performance /sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference) CPUs"
echo "boost (left as-is; recorded by native_run.sh): $(cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null || cat /sys/devices/system/cpu/cpu0/cpufreq/boost 2>/dev/null || echo n/a)"

say "Verify"
g++ --version | head -1
cmake --version | head -1
if perf stat -e cycles,instructions -- true 2>&1 | grep -q instructions; then
  perf stat -e cycles,instructions -- true 2>&1 | grep -E 'cycles|instructions'
  echo "perf: OK"
else
  warn "perf counters unavailable; native_run.sh will skip perf steps"
fi
echo "clocksource: $(cat /sys/devices/system/clocksource/clocksource0/current_clocksource)"
echo "power: $(cat /sys/class/power_supply/A*/online 2>/dev/null | head -1 | sed 's/1/AC online/;s/0/ON BATTERY - plug in AC/')"
say "Setup done. Next: scripts/native_run.sh full <results-dir>"
