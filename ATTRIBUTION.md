# 开源来源、版权与许可证说明 / Attribution & Licenses

本仓库是 **Spectrum SBE1V1K（Askey RTQ7300T）** 的社区固件与 Wiro U-Boot 源码，由 **Orangeyoo**（Orangeyoo@50848）移植和维护。
它不是 Spectrum、Charter、Askey、Qualcomm、LinkEase/iStoreOS 或 OpenWrt 的官方产品。

各文件和二进制组件继续适用其上游许可证与版权声明。本仓库原创的移植代码、设备树、构建脚本和文档，在没有其他文件级声明时采用 `GPL-2.0-or-later`；该许可不覆盖、也不重新授权第三方组件。上游许可证全文见 `COPYING`、`LICENSES/`，以及各软件包与源码目录内的声明。

**不在本仓库范围内**：Wiro 自动刷机、Wiro 救砖两个 Windows 工具及其服务端是闭源收费软件，只以二进制形式在 Releases 中分发，不适用本仓库的开源许可证。

## 本项目原创部分（Orangeyoo）

| 位置 | 内容 |
|---|---|
| `target/linux/feeds/*/files-6.6/arch/arm64/boot/dts/qcom/ipq9574-sbe1v1k.dts` | SBE1V1K 设备树（PHY 绑定、CPR 调压、内存布局、风扇与 LED） |
| `target/linux/feeds/ipq95xx/image/generic.mk`（`Device/orangeyoo_sbe1v1k`）、`base-files/` | 镜像配方、后台升级、端口角色 |
| `package/orangeyoo-*`、`package/firmware/orangeyoo-*`、`package/sbe-*` | 出厂默认值、QuickStart 集成、射频板级数据生成、Wi-Fi 固件打包、PPPoE PADT |
| `release/` | 可复现构建、版本锁定、发布检查（gate） |
| `uboot-1.9c` 分支：`board/qualcomm/sbe1v1k-chainloader-fit/`、`net/lwip/httpd_recovery.c` 中的 eMMC/GPT 恢复扩展、`lib/lwip/httpd/htdocs/` | 链式引导 shim、HTTP 网页恢复、恢复页 |

## 上游项目

| 项目 | 在本项目中的用途 | 许可证 |
|---|---|---|
| [OpenWrt](https://openwrt.org) | 构建系统与基础系统（24.10 线） | GPL-2.0 等，见各包 |
| Qualcomm QSDK 14（[CodeLinaro](https://git.codelinaro.org/clo/qsdk)） | IPQ95xx 平台支持、NSS/PPE、SSDK、Linux 6.6 内核树 | 见各组件（GPL-2.0 / ISC / BSD 等） |
| [Linux](https://kernel.org) / ath12k | 内核与 Wi-Fi 7 驱动 | GPL-2.0 |
| [U-Boot](https://source.denx.de/u-boot/u-boot) | 2026.08 基线、lwIP 网络栈 | GPL-2.0+ 等，见 `Licenses/` |
| [YYH2913](https://github.com/YYH2913) | Wiro U-Boot 网页恢复与 iStoreOS 社区固件的开源工作 | 见原项目 |
| [iStoreOS / LinkEase](https://github.com/istoreos/istoreos) | iStoreOS 界面、iStore 商店、QuickStart | 见各包 |
| [OpenClash](https://github.com/vernesong/OpenClash) | 代理客户端 | MIT |
| [PassWall](https://github.com/xiaorouji/openwrt-passwall) | 代理客户端 | GPL-3.0 |
| [Xray-core](https://github.com/XTLS/Xray-core) / [sing-box](https://github.com/SagerNet/sing-box) | 代理内核 | MPL-2.0 / GPL-3.0 |
| [luci-theme-argon](https://github.com/jerrykuku/luci-theme-argon) | 界面主题 | 见原项目 |
| ASUS TUF-BE9400 GPL 源码包 | QCN9224 Wi-Fi 固件 WLAN.WBE.1.4（`amss.bin` / `m3.bin`，仅二进制） | Qualcomm 固件再分发条款 |

PassWall、sing-box 等 GPL-3.0 组件按 GPL-3.0 分发。射频板级数据（`package/firmware/orangeyoo-sbe1v1k-bdf`）由设备原厂 ART 数据生成，仅用于 SBE1V1K，运行时会优先读取本机 ART。

## 致谢

感谢 [YYH2913](https://github.com/YYH2913)，感谢以上所有上游开源项目的作者，也感谢 AI 助手 GPT、Claude 和 GLM 在移植过程中提供的帮助。
