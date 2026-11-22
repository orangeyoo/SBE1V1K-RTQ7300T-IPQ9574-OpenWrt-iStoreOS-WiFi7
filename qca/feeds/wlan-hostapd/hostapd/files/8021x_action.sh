#!/bin/sh
# 8021x_action.sh - wpa_supplicant action script for wired 802.1X.
#
# wpa_supplicant invokes this script with two arguments whenever the
# authentication state changes:
#   $1 = interface name
#   $2 = event (CONNECTED, DISCONNECTED, ...)
#
# Port authentication gates the WAN uplink, so the DHCP client has to be
# kicked every time the port comes up or goes down.

IFNAME="$1"
EVENT="$2"
LOG_TAG="8021x-action"

logger_once() {
	logger -t "$LOG_TAG" "$1"
}

case "$EVENT" in
	CONNECTED)
		logger_once "$IFNAME: 802.1X port authenticated, renewing WAN lease"
		/etc/802.1x/dhcp_wan_renew.sh up "$IFNAME" >/dev/null 2>&1
		;;
	DISCONNECTED)
		logger_once "$IFNAME: 802.1X port lost authentication, tearing down WAN"
		/etc/802.1x/dhcp_wan_renew.sh down "$IFNAME" >/dev/null 2>&1
		;;
	*)
		logger_once "$IFNAME: unhandled event $EVENT"
		;;
esac

exit 0
