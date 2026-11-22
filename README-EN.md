<p align="center">
  <img src="docs/images/wiro-logo.png" width="280" alt="Wiro">
</p>

<h1 align="center">Spectrum SBE1V1K Wi-Fi 7 OpenWrt / iStoreOS Firmware</h1>

<p align="center">
  <b>Spectrum SBE1V1K · Askey RTQ7300T · Qualcomm IPQ9574 + 3× QCN9224</b><br>
  Full Qualcomm QSDK 14 port · Linux 6.6 + ath12k · NSS/PPE hardware offload · iStoreOS
</p>

<p align="center">
  <a href="../../releases/latest"><img src="https://img.shields.io/github/v/release/orangeyoo/SBE1V1K-RTQ7300T-IPQ9574-OpenWrt-iStoreOS-WiFi7?include_prereleases&style=flat-square&label=release" alt="Release"></a>
  <a href="ATTRIBUTION.md"><img src="https://img.shields.io/badge/license-GPL--2.0--or--later-blue?style=flat-square" alt="License"></a>
  <img src="https://img.shields.io/badge/Wi--Fi%207-QCN9224%20×3%20tri--band-8A2BE2?style=flat-square" alt="Wi-Fi 7">
  <img src="https://img.shields.io/badge/SoC-Qualcomm%20IPQ9574-333333?style=flat-square" alt="SoC">
  <img src="https://img.shields.io/badge/QSDK%2014-OpenWrt%2024.10-0099FF?style=flat-square" alt="OpenWrt">
  <img src="https://img.shields.io/badge/kernel-6.6.116%20·%20ath12k-green?style=flat-square" alt="Kernel">
</p>

<p align="center"><a href="README.md">简体中文</a> · <a href="../../releases/latest"><b>Download</b></a> · <a href="https://wzyp.cn/shop/155QK568"><b>Buy a flashing key</b></a> · <a href="FLASHING-GUIDE.md">Manual flashing (ZH)</a></p>

---

**Flash the SBE1V1K without opening it.** Wiro AutoFlash installs OpenWrt / iStoreOS over one network cable: no disassembly, no serial/TTL, no U-Boot commands. Press Reset twice and it finishes in 10–15 minutes, backing up the factory boot area and RF calibration first; Wiro Rescue restores a bricked unit. Both are paid tools ([buy a key](https://wzyp.cn/shop/155QK568) — one key = one device, 2 flashes, failed attempts don't count); buyers join the member group, QQ **1061612207**.

Unofficial community firmware for the **Spectrum SBE1V1K** (Askey RTQ7300T). The stock system runs Linux 5.4 with the closed qca-wifi stack; this firmware ports the whole board to **Qualcomm QSDK 14 (OpenWrt 24.10 / Linux 6.6 / open-source ath12k)**:

- all four Ethernet ports, the three Wi-Fi 7 radios, NSS/PPE offload, fan and LEDs are brought up from scratch
- iStoreOS web UI and iStore app store
- newer QCN9224 Wi-Fi firmware (WLAN.WBE.1.4), so a crashed radio recovers in place within seconds

> ⚠️ For the SBE1V1K / RTQ7300T only.

## Measured results

Measured 2026-11-17 on two SBE1V1K units in daily use (main router + 6 GHz mesh node, 1.0 series firmware) with other household clients attached. iperf3 3.22; PC NICs: HPE 10G and Qualcomm FastConnect 7800 Wi-Fi 7; Wi-Fi tests bind to the WLAN address.

| Test | Result |
|---|---|
| 10G port, PC → router | **7.72 Gbps** (4 streams) · 4.70 Gbps (1 stream) |
| 10G port, router → PC | **6.96 Gbps** (8 streams) · 4.15 Gbps (4 streams) |
| 10G port to a switch | works; the main router's 10G port serves a switch with 3 devices in daily use |
| 6 GHz mesh backhaul (320 MHz 802.11s) | main → node **1.17 Gbps** · node → main 0.96 Gbps (8 streams) |
| 5 GHz 160 MHz (PC on the node, 2882 Mbps link, -47 dBm) | up **0.82 Gbps** · down 0.61 Gbps, node also carrying the mesh and other clients |
| Uptime | both units 21 h+, PPPoE never dropped (21.5 GB in / 10.7 GB out), zero radio crashes |
| Radio crash recovery | injected 6 GHz firmware assert: that radio recovers in **~8 s**, mesh rejoins in 35 s, no reboot |
| Thermal | ~54–59 °C, fan ~2150 rpm, temperature-controlled |

Notes: each band is its own single-link MLD (no cross-band aggregation); crypto uses the Qualcomm QCE engine, no NSS EIP IPsec offload; 2.5G WAN full-speed test over PPPoE still to be added.

## Install

The firmware and the Wiro U-Boot are free. See [FLASHING-GUIDE.md](FLASHING-GUIDE.md): sysupgrade from a running Wiro system, U-Boot web recovery at `192.168.255.1`, or a first install from the stock U-Boot over TTL + TFTP. After flashing: `http://192.168.30.1`, `root` / `password`; the 2.5G port is WAN.

**Wiro AutoFlash** (paid, closed source) installs it without opening the case or a serial console — press Reset twice, done in 10–15 min, with an automatic factory backup; **Wiro Rescue** restores bricked units. The fee saves you the TTL work and funds further development (the port burned a lot of AI tokens). Buyers can join the member group, QQ **1061612207**.

## Source

| | Branch / tag |
|---|---|
| Firmware (full QSDK 14 build tree, `release/build.sh`) | [`source-1.0`](../../tree/source-1.0) · `v1.1.2` |
| Wiro U-Boot (2026.08 chainloader + HTTP eMMC/GPT recovery) | [`uboot-1.9c`](../../tree/uboot-1.9c) · `wiro-uboot-webfix1.9c` |

Ported and maintained by **Orangeyoo** (Orangeyoo@50848); see [ATTRIBUTION.md](ATTRIBUTION.md). Thanks to [YYH2913](https://github.com/YYH2913), to the authors of every upstream project, and to the AI assistants GPT, Claude and GLM. Not affiliated with Spectrum, Charter, Askey or Qualcomm. Flash at your own risk.
