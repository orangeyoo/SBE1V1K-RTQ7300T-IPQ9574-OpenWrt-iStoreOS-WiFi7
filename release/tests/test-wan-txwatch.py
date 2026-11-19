#!/usr/bin/env python3
"""Offline decision tests. Replace paths and stub all side-effecting commands."""
import os
import shlex
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DAEMON = ROOT / "target/linux/feeds/ipq95xx/base-files/usr/sbin/sbe-wan-txwatch"
STUBS = r"""
sleep() {
    if [ "$1" = 10 ]; then
        read -r time link tx drops epoch <&3 || exit 0
        printf '%s.00 0\n' "$time" > "$sandbox/uptime"
        printf '%s\n' "$link" > "$sandbox/eth-test/carrier"
        printf '%s\n' "$tx" > "$sandbox/eth-test/statistics/tx_packets"
    else
        [ "$1" = 3 ] || exit 20
        printf '%s.00 0\n' "$((time + 3))" > "$sandbox/uptime"
    fi
}
ethtool() {
    [ "$*" = '-S eth-test' ] || exit 21
    [ "$drops" = missing ] || printf '     tx_dropped: %s\n' "$drops"
}
ip() {
    [ "$1 $2 $3 $4" = 'link set dev eth-test' ] || exit 22
    printf '%s %s\n' "$time" "$5"
}
date() { printf '%s\n' "$epoch"; }
sync() { :; }
logger() { printf '%s\n' "$*" >&2; }
reboot() { printf '%s reboot\n' "$time"; }
"""
NOW = 1800000000


def scenario(name, rows, actions, marker=None, expected_marker=None, message=None):
    with tempfile.TemporaryDirectory() as work:
        work = Path(work)
        (work / "eth-test/statistics").mkdir(parents=True)
        (work / "eth-test/operstate").write_text("up\n")
        if marker is not None:
            (work / "marker").write_text(f"{marker}\n")
        script = DAEMON.read_text().replace("/sys/class/net/", f"{work}/")
        script = script.replace("/proc/uptime", f"{work}/uptime")
        script = script.replace("/etc/sbe-wan-txwatch.last-reboot", f"{work}/marker")
        assert all(path not in script for path in ("/sys/", "/proc/", "/etc/"))
        source = f"sandbox={shlex.quote(str(work))}\nexec 3<&0\n" + STUBS + script
        shell = shlex.split(os.environ.get("TEST_SHELL", "/bin/sh"))
        result = subprocess.run(
            [*shell, "-c", source, "watchdog-test", "eth-test"],
            input="".join(f"{t} {link} {tx} {drops} {NOW + t}\n" for t, link, tx, drops in rows),
            capture_output=True, text=True, check=True, timeout=10,
        )
        assert result.stdout.splitlines() == actions, (name, result.stdout, result.stderr)
        marker_path = work / "marker"
        observed = marker_path.read_text().strip() if marker_path.is_file() else None
        assert observed == (str(expected_marker) if expected_marker is not None else None), (name, observed)
        assert message is None or message in result.stderr, (name, result.stderr)
        print(f"PASS {name}")


def stalled(start, end):
    return [(t, 1, 42, t) for t in range(start, end + 1, 10)]


scenario("boot grace", stalled(10, 140), [])
scenario("idle WAN", [(t, 1, 42, 3) for t in range(120, 301, 10)], [])
scenario("one recent increase", [(t, 1, 42, int(t >= 130)) for t in range(120, 301, 10)],
         ["150 down", "150 up"])
scenario("healthy TX", [(t, 1, t, t) for t in range(120, 301, 10)], [])
scenario("invalid link or stats break window", stalled(120, 140) +
         [(150, 0, 42, 150), (160, 1, 42, "missing")] + stalled(170, 190), [])
scenario("driver counter reset breaks window", stalled(120, 140) +
         [(150, 1, 42, 0), (160, 1, 42, 1), (170, 1, 42, 2)], [])
scenario("recovery cancels reboot", stalled(120, 190) +
         [(t, 1, 43, t) for t in range(200, 220, 10)], ["150 down", "150 up"])
scenario("persistent stall reboots after 60 seconds", stalled(120, 220),
         ["150 down", "150 up", "220 reboot"], expected_marker=NOW + 220)
scenario("persisted marker blocks reboot and retries after 5 min", stalled(120, 450),
         ["150 down", "150 up", "450 down", "450 up"], marker=NOW,
         expected_marker=NOW, message="reboot rate-limited")
scenario("6 h cooldown expires", stalled(120, 220), ["150 down", "150 up", "220 reboot"],
         marker=NOW + 220 - 21600, expected_marker=NOW + 220)
scenario("future marker blocks reboot", stalled(120, 220), ["150 down", "150 up"],
         marker=NOW + 1000, expected_marker=NOW + 1000, message="behind reboot marker")
scenario("invalid marker blocks reboot", stalled(120, 220), ["150 down", "150 up"],
         marker="garbled", expected_marker="garbled", message="invalid clock or marker")
