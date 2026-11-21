# SBE1V1K 刷机说明

本文说明怎么把 Wiro 固件刷进 **Spectrum SBE1V1K（Askey RTQ7300T）**。固件和 Wiro U-Boot 都可以从 [Releases](../../releases/latest) 免费下载。按你的情况选一种方式：

| 你的情况 | 方式 | 需要 |
|---|---|---|
| 已经在用 Wiro 固件 | [A. 后台升级](#a-后台升级) | 浏览器 |
| 已经装过 Wiro U-Boot | [B. U-Boot 网页恢复](#b-u-boot-网页恢复) | 浏览器 |
| 原厂机第一次刷，愿意自己动手 | [C. TTL 手动刷入](#c-原厂机ttl-手动刷入) | 拆机、USB 转 TTL、TFTP |
| 原厂机第一次刷，不想拆机 | [D. 一键刷机（收费）](#d-一键刷机收费) | 刷机密钥 |

> ⚠️ 只适用于 SBE1V1K / RTQ7300T。刷机有风险，因刷机造成的损坏由使用者自行承担。写入过程中不要断电、不要拔网线。

## 需要的文件

| 文件 | 说明 |
|---|---|
| `sbe1v1k-1.1-sysupgrade.bin` | 系统固件（A、B、C 方式使用） |
| `wiro-uboot-webfix1.9c-sbe1v1k-chainloader.itb` | Wiro U-Boot（C 方式使用，或用于升级 U-Boot） |
| `SHA256SUMS.txt` | 下载后核对校验值 |

## A. 后台升级

已经在运行 Wiro 固件时，在管理页面 **系统 → 备份/升级 → 刷写新的固件** 上传 `sbe1v1k-*-sysupgrade.bin`，等待设备自动重启即可。

## B. U-Boot 网页恢复

已经装过 Wiro U-Boot 的设备：

1. 网线接设备的 **LAN3 口（1G 口）**，电脑网口设为自动获取 IP。
2. 断电，按住 Reset，插电，直到指示灯变成跑马灯（恢复模式），再松开。
3. 浏览器打开 `http://192.168.255.1`，进入 **Firmware（固件）** 页，选择 `sbe1v1k-*-sysupgrade.bin` 上传。
4. 页面依次显示 上传 → 擦除 → 写入，完成后设备自动重启。

恢复页打不开时，把网线换到 10G 口再试。

## C. 原厂机：TTL 手动刷入

需要拆开外壳，用 USB 转 TTL 模块（3.3V）连接主板串口，串口参数 115200 8N1。电脑需要能运行 TFTP 服务器（例如 Tftpd64），并把 `wiro-uboot-webfix1.9c-sbe1v1k-chainloader.itb` 放在 TFTP 根目录。

**1. 临时引导 Wiro U-Boot**

电脑有线网口设为 `192.168.1.2/24`。给设备上电，在串口里按任意键打断启动，进入原厂 U-Boot 命令行后输入：

```sh
setenv ipaddr 192.168.1.1
setenv serverip 192.168.1.2
tftpboot 0x80000000 wiro-uboot-webfix1.9c-sbe1v1k-chainloader.itb
bootm 0x80000000
```

> **不要执行 `saveenv`**。这里只用临时变量，原厂环境变量保持不变。

**2. 进入网页恢复，先备份**

Wiro U-Boot 启动后，在它的命令行输入 `http_recovery`。电脑改回自动获取 IP（会拿到 `192.168.255.2`），浏览器打开 `http://192.168.255.1`。

在 **Backup（备份）** 页点 **Download all (.tar)**，把整机分区备份下载到电脑。约 1.5 小时，文件超过 7 GB，存放的磁盘不能是 FAT32。里面有这台设备独有的射频校准数据，请妥善保存。

**3. 调整分区布局**

在 **eMMC layout** 页选择 **Large storage**，在确认框输入 `SBE1V1K_REPARTITION`，点击应用。等待校验完成，**不要断电、不要重启**。

**4. 写入固件**

在 **Firmware** 页上传 `sbe1v1k-*-sysupgrade.bin`，等待 上传 → 擦除 → 写入 完成，设备自动重启。

U-Boot 源码与详细说明见分支 [`uboot-1.9c`](../../tree/uboot-1.9c) 的 README。

## D. 一键刷机（收费）

不想拆机接 TTL，可以用 **Wiro 自动刷机**：按两次 Reset，10～15 分钟全自动完成，刷之前会自动备份原厂引导区和射频校准数据。刷坏了可以用 **Wiro 救砖** 恢复。

这是收费工具，需要[购买刷机密钥](https://wzyp.cn/shop/155QK568)。收费的原因：帮你省下拆机、接线、敲命令的时间，同时支持作者继续优化固件——移植和调试消耗了大量 AI token。购买后可以加入会员群 **1061612207**。

图文步骤见 [SBE1V1K-Flashing-Guide-ZH.pdf](docs/SBE1V1K-Flashing-Guide-ZH.pdf)。

## 刷完之后

- 管理地址 `http://192.168.30.1`，账号 `root`，初始密码 `password`，登录后请立即修改
- 2.5G 口是 **WAN 口**，接光猫或上级路由；电脑接其余网口
- 首次 Wi-Fi：`iStoreOS_xxxx_2.4G` / `_5G` 为开放网络，`_6G` 为 OWE 加密，登录后请设置密码
- 第一次开机会自动重启一次（初始化存储），属于正常现象
