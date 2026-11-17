// SPDX-License-Identifier: GPL-2.0+

#include <command.h>
#include <env.h>

int run_tftp_recovery(void);

static int do_tftp_recovery(struct cmd_tbl *cmdtp, int flag, int argc,
			    char *const argv[])
{
	(void)cmdtp;
	(void)flag;
	(void)argc;
	(void)argv;

	env_set("ipaddr", "192.168.255.1");
	env_set("netmask", "255.255.255.0");
	env_set("gatewayip", "0.0.0.0");

	return run_tftp_recovery();
}

U_BOOT_CMD(
	tftp_recovery, 1, 0, do_tftp_recovery,
	"full clean + flash firmware pulled from a TFTP server",
	"\n"
	"Waits for the sysupgrade firmware via TFTP from the PC (default\n"
	"192.168.255.2, file 'firmware.bin'), then runs the destructive full\n"
	"clean (canonical GPT + rootfs_data zero-fill) and flashes the\n"
	"received image through the standard recovery pipeline.\n"
	"Environment: tftp_recovery_serverip, tftp_recovery_file,\n"
	"tftp_recovery_timeout (seconds, 0 = wait forever)"
);
