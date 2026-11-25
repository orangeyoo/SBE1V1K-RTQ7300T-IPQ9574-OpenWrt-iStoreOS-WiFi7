# SBE1V1K Flashing Guide

简体中文版：[FLASHING-GUIDE.md](FLASHING-GUIDE.md)

This guide explains how to flash the Wiro firmware onto a **Spectrum SBE1V1K (Askey RTQ7300T)**. The firmware and the Wiro U-Boot are free downloads from [Releases](../../releases/latest). Pick the method that matches your situation:

| Your situation | Method | You need |
|---|---|---|
| Already running Wiro firmware | [A. Web upgrade](#a-web-upgrade) | A browser |
| Wiro U-Boot already installed | [B. U-Boot web recovery](#b-u-boot-web-recovery) | A browser |
| Stock unit, first flash, comfortable DIY | [C. First flash from stock over TTL](#c-first-flash-from-stock-over-ttl) | Case opening, USB-TTL adapter, TFTP |
| Stock unit, first flash, no disassembly | [D. One-click flashing (paid)](#d-one-click-flashing-paid) | A flashing key |

> ⚠️ For the SBE1V1K / RTQ7300T only. Flashing is at your own risk; damage caused by flashing is the user's responsibility. Do not cut power or unplug the cable while writing.

## Files you need

| File | Notes |
|---|---|
| `sbe1v1k-1.1.2-sysupgrade.bin` | System firmware (used by methods A, B, C) |
| `wiro-uboot-webfix1.9c-sbe1v1k-chainloader.itb` | Wiro U-Boot (used by method C, or to upgrade U-Boot) |
| `SHA256SUMS.txt` | Verify checksums after downloading |

## A. Web upgrade

On a unit already running Wiro firmware, open the admin UI at **System → Backup / Flash Firmware → Flash new firmware image**, upload `sbe1v1k-*-sysupgrade.bin` and wait for the device to reboot itself.

## B. U-Boot web recovery

For units that already have the Wiro U-Boot installed:

1. Connect the network cable to the **LAN3 port (1 GbE)** and set the PC NIC to DHCP.
2. Power off, hold Reset, power on, keep holding until the LEDs start chasing (recovery mode), then release.
3. Open `http://192.168.255.1` in a browser, go to the **Firmware** page and upload `sbe1v1k-*-sysupgrade.bin`.
4. The page walks through Upload → Erase → Write; the device reboots automatically when done.

If the recovery page won't open, move the cable to the 10G port and try again.

## C. First flash from stock over TTL

This requires opening the case and connecting a USB-TTL adapter (3.3 V) to the board's serial header at 115200 8N1. The PC needs a TFTP server (e.g. Tftpd64) with `wiro-uboot-webfix1.9c-sbe1v1k-chainloader.itb` in its root directory.

**1. Boot the Wiro U-Boot temporarily**

Set the PC's wired NIC to `192.168.1.2/24`. Power the device on, press any key in the serial console to interrupt boot, and at the stock U-Boot prompt enter:

```sh
setenv ipaddr 192.168.1.1
setenv serverip 192.168.1.2
tftpboot 0x80000000 wiro-uboot-webfix1.9c-sbe1v1k-chainloader.itb
bootm 0x80000000
```

> **Do not run `saveenv`.** Only temporary variables are used here; the stock U-Boot environment stays untouched.

**2. Enter web recovery and back up first**

Once the Wiro U-Boot is up, type `http_recovery` at its prompt. Switch the PC back to DHCP (it will get `192.168.255.2`) and open `http://192.168.255.1`.

On the **Backup** page click **Download all (.tar)** to save a full partition backup to the PC. It takes about 1.5 hours and exceeds 7 GB — the target disk must not be FAT32. It contains this unit's unique RF calibration data; store it safely.

**3. Repartition the eMMC layout**

On the **eMMC layout** page choose **Large storage**, type `SBE1V1K_REPARTITION` in the confirmation box and apply. Wait for verification to finish — **do not cut power, do not reboot**.

**4. Write the firmware**

On the **Firmware** page upload `sbe1v1k-*-sysupgrade.bin`, wait for Upload → Erase → Write to complete, and the device reboots automatically.

U-Boot sources and details: see the README on branch [`uboot-1.9c`](../../tree/uboot-1.9c).

## D. One-click flashing (paid)

If you'd rather not open the case and attach a TTL, use **Wiro AutoFlash**: press Reset twice and everything completes automatically in 10–15 minutes, with the factory boot area and RF calibration backed up beforehand. A bricked unit can be restored with **Wiro Rescue**.

This is a paid tool and requires [buying a flashing key](https://wzyp.cn/shop/155QK568). Why it costs money: it saves you the disassembly, wiring and command-line work, and it funds further firmware development — the porting and debugging consumed a large number of AI tokens. Buyers can join the member group **1061612207**.

Illustrated steps (Chinese): [SBE1V1K-Flashing-Guide-ZH.pdf](docs/SBE1V1K-Flashing-Guide-ZH.pdf).

## After flashing

- Admin UI at `http://192.168.30.1`, user `root`, initial password `password` — change it right after logging in
- The 2.5G port is **WAN**: connect it to your modem or upstream router; connect PCs to the other ports
- First Wi-Fi: `iStoreOS_xxxx_2.4G` / `_5G` are open networks, `_6G` uses OWE encryption — set passwords after logging in
- The first boot reboots itself once (storage initialization); that is normal
