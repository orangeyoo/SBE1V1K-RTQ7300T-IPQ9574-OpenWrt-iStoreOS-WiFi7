#!/bin/sh
# dhcp_wan_renew.sh - drive the WAN DHCP client after wired 802.1X state changes.
#
# Called by 8021x_action.sh:
#   dhcp_wan_renew.sh up   <ifname>   - port authenticated: (re)start WAN protocol
#   dhcp_wan_renew.sh down <ifname>   - port unauthenticated: stop WAN protocol
#
# The WAN interface is looked up in /etc/config/network by the optional first
# config argument convention: wan first, then wan6, else the interface that
# carries the default route.

ACTION="$1"
PORT="$2"

wan_iface() {
	local iface
	iface="$(ubus -S call network.interface.wan status 2>/dev/null | sed -n 's/.*"interface"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n1)"
	[ -n "$iface" ] && { echo "$iface"; return; }
	iface="$(uci -q get network.wan.interface 2>/dev/null)"
	[ -n "$iface" ] && { echo "$iface"; return; }
	# last resort: device carrying the default route
	iface="$(ip -4 route show default 2>/dev/null | sed -n 's/.*dev \([^ ]*\).*/\1/p' | head -n1)"
	echo "$iface"
}

renew_dhcp_clients() {
	# Nudge whatever DHCP clients exist on the port.
	for pid in $(pgrep -f "udhcpc.*-i $PORT" 2>/dev/null); do
		kill -USR1 "$pid" 2>/dev/null
	done
	for pid in $(pgrep -f "odhcpc6.*$PORT" 2>/dev/null); do
		kill -USR1 "$pid" 2>/dev/null
	done
}

case "$ACTION" in
	up)
		IFACE="$(wan_iface)"
		[ -n "$IFACE" ] || exit 0
		renew_dhcp_clients
		# Ask netifd to bring the interface up again; this reruns the proto
		# setup (teardown first so stale state is cleared).
		ubus call network.interface."$IFACE" down >/dev/null 2>&1
		sleep 1
		ubus call network.interface."$IFACE" up >/dev/null 2>&1
		;;
	down)
		IFACE="$(wan_iface)"
		[ -n "$IFACE" ] || exit 0
		ubus call network.interface."$IFACE" down >/dev/null 2>&1
		;;
	*)
		echo "Usage: $0 <up|down> [port]" >&2
		exit 1
		;;
esac

exit 0
