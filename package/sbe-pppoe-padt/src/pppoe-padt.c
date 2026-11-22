/*
 * pppoe-padt - tell the ISP access concentrator to drop a PPPoE session.
 *
 * SBE1V1K: when the router loses power, panics or hangs, pppd never sends
 * PADT and the BRAS keeps the old session. ISPs that allow one session per
 * account then refuse the next dial ("Access number is exceed") until the
 * stale session times out; users had to power-cycle the ONT to get back
 * online. sbe-pppoe-padt runs this at boot with the session recorded by
 * the last ip-up.
 *
 * usage: pppoe-padt <ifname> <session-id hex> <ac-mac>
 */
#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define ETH_P_PPP_DISC	0x8863
#define PADT_CODE	0xa7

static int parse_mac(const char *s, unsigned char *mac)
{
	unsigned int b[6];
	int i;

	if (sscanf(s, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
		return -1;
	for (i = 0; i < 6; i++) {
		if (b[i] > 0xff)
			return -1;
		mac[i] = b[i];
	}
	return 0;
}

int main(int argc, char **argv)
{
	unsigned char frame[ETH_ZLEN] = { 0 };
	struct sockaddr_ll sll = { 0 };
	unsigned char peer[6];
	struct ifreq ifr;
	unsigned long sid;
	char *end;
	int fd, i;

	if (argc != 4) {
		fprintf(stderr, "usage: %s <ifname> <session-id hex> <ac-mac>\n", argv[0]);
		return 2;
	}
	sid = strtoul(argv[2], &end, 16);
	if (*end || !sid || sid > 0xffff || parse_mac(argv[3], peer)) {
		fprintf(stderr, "bad session id or mac\n");
		return 2;
	}

	fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_PPP_DISC));
	if (fd < 0) {
		perror("socket");
		return 1;
	}

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, argv[1], IFNAMSIZ - 1);
	/* ifr_ifindex and ifr_hwaddr share a union: index first */
	if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
		perror(argv[1]);
		return 1;
	}
	sll.sll_ifindex = ifr.ifr_ifindex;
	if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) {
		perror(argv[1]);
		return 1;
	}

	/* Ethernet header */
	memcpy(frame, peer, 6);
	memcpy(frame + 6, ifr.ifr_hwaddr.sa_data, 6);
	frame[12] = ETH_P_PPP_DISC >> 8;
	frame[13] = ETH_P_PPP_DISC & 0xff;
	/* PPPoE header: ver 1, type 1, code PADT, session id, no tags */
	frame[14] = 0x11;
	frame[15] = PADT_CODE;
	frame[16] = sid >> 8;
	frame[17] = sid & 0xff;
	frame[18] = 0;
	frame[19] = 0;

	sll.sll_family = AF_PACKET;
	sll.sll_protocol = htons(ETH_P_PPP_DISC);
	sll.sll_halen = 6;
	memcpy(sll.sll_addr, peer, 6);

	for (i = 0; i < 3; i++) {
		if (sendto(fd, frame, sizeof(frame), 0, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
			perror("sendto");
			return 1;
		}
		usleep(200000);
	}
	close(fd);
	return 0;
}
