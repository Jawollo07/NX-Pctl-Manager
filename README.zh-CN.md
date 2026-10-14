# NX-Pctl-Manager

[![build](https://github.com/tailiang2008/NX-Pctl-Manager/actions/workflows/build.yml/badge.svg)](https://github.com/tailiang2008/NX-Pctl-Manager/actions/workflows/build.yml)
[![license: GPLv3](https://img.shields.io/badge/license-GPLv3-blue.svg)](LICENSE)
[![latest release](https://img.shields.io/github/v/release/tailiang2008/NX-Pctl-Manager)](https://github.com/tailiang2008/NX-Pctl-Manager/releases/latest)

[English](README.md) | **简体中文**

Nintendo Switch 家长控制工具。**不需要手机 App，不需要任天堂账号，也不需要联网**，直接在主机上设置每日游玩时间限制，也能设置或删除 PIN 码、解除手机 App 绑定。

> ⚠️ **需要自定义系统（CFW，推荐大气层 Atmosphère）。** 本工具直接调用系统受限的 `pctl` 服务，只能在破解主机上运行，正版未破解主机无法使用。它不绕过任何账号或在线验证，只是把原本藏在手机 App 或系统设置深处的家长控制选项，搬到主机上离线操作。
>
> 本工具直接调用 `pctl` 的受限命令和 `*ForDebug` 命令。**需要系统版本 21.0.0 或更新。** 已测试的组合：
>
> - 22.5.0 + 大气层 1.12.0（v3.1.0）
> - 22.1.0 + 大气层 1.11.1（v3.0.0）
>
> **其他版本未经验证，使用风险自负。** `pctl` 如何处理这些写入，见 [`docs/pctl-reverse-engineering.md`](docs/pctl-reverse-engineering.md)。

---

## 功能

### 离线设置游玩时间（本工具的主要用途）

任天堂**只允许通过“Nintendo Switch 家长控制”手机 App 设置每日游玩时间**，再同步到主机，主机的系统设置里没有这个选项。本工具直接调用主机的 `pctl` 服务，所以可以**完全离线、直接在主机上**完成以下操作：

- **所有日子统一限时**：一个分钟数，应用到周日至周六。
- **按星期分别设置**：周日、周一……周六各设一个值。在子菜单里逐天修改，最后一次性保存。
- **取消限时**：删除每日限时。如果设置了就寝闹钟，闹钟会保留。
- **就寝闹钟**：从指定时间开始限制游玩，直到第二天早上（默认早上 06:00 解除），每天生效。
- **到时间后**：中断软件（默认），或者仅提醒。

说明：

- `0` 分钟表示当天**完全禁止游玩**，不是“不限时”。某一天要改回“不限时”，在“按星期分别设置”里操作。
- **必须先设置家长控制 PIN 码**。没有 PIN 码时，`pctl` 会直接丢弃游玩时间设置，而且不报错。

> **离线主机：要先设置网络时钟。** `pctl` 只有在网络时钟有效时才会计时。主机从来没连上任天堂的时间服务器（离线，或者屏蔽了任天堂），网络时钟就一直是空的。
>
> 本工具启动时会检测这一点，并提示用系统时间设置网络时钟；主菜单里也有同样的选项。设置之后按网络时钟计时，在系统设置里改日期重置不了当天的限时。
>
> 如果计时已经开始，每次写入都会先临时暂停家长控制（命令 1201），写入后马上重新开启（命令 1007），不需要你手动操作。

### 其他功能（家长控制维护）

- **设置 / 修改 PIN 码**：打开系统自带的家长控制 PIN 码界面。
- **删除全部家长控制**：清除 PIN 码和所有限制。**不可恢复**，执行前会二次确认。
- **解除手机 App 绑定**：断开“Nintendo Switch 家长控制”手机 App 与本机的绑定。
- **查看状态**：系统版本、PIN 码、限制是否启用、游玩时间状态、网络时钟状态。
- **诊断报告**：只读导出到 `sd:/switch/nx_pctl_dump.txt`，PIN 码已打码。反馈问题时请附上它，以及 `sd:/switch/nx_pctl_manager.log` 和 `sd:/switch/nx_pctl_borealis.log`。

### 启动时验证 PIN 码

设置了 PIN 码时，打开本工具会先要求输入 PIN 码，输错 5 次或按 B 都会退出。这样孩子就没法用本工具解除限制。

代价是本工具不能再用来恢复忘记的 PIN 码。忘记 PIN 码时，请使用任天堂官方的主密钥重置：系统设置 → 家长控制 → 忘记 PIN 码。

## 使用方法

操作方式：

- ↑ / ↓ 移动光标，(A) 确认。
- (B) 返回上一级；在主菜单按 (B) 退出。
- 删除全部家长控制、解除手机 App 绑定、取消限时这类不可逆操作，执行前都会确认。

### 按主机的情况选择流程

**A. 这台主机还没设置过家长控制**（没有绑定手机 App）

1. 在主机上进入 *系统设置* → *家长控制*，设置 PIN 码。不用绑定手机 App，本工具就是为了省掉这一步。也可以用本工具里的 *设置 / 修改家长控制 PIN 码*。
2. 从 hbmenu 打开本工具。如果提示网络时钟从未设置过，选 *立即设置*。
3. 继续看下面的“设置游玩时间”。

**B. 已经通过“Nintendo Switch 家长控制”手机 App 设置过**（手机 App 目前绑定着这台主机）

1. 从 hbmenu 打开本工具，选 *解除手机 App 绑定*。否则手机 App 下次同步时，会覆盖你在这里设置的限时。
2. 继续看下面的“设置游玩时间”。

### 设置游玩时间

主菜单 → *游玩时间（每日限时 / 就寝闹钟）*：

- *设置每日限时（所有日子）*：弹出数字键盘输入分钟数（0–1440），应用到每一天。
- *按星期分别设置*：进入子菜单，在某一天上按 (A)，可以设置分钟数或“不限时”。修改先暂存，用 `(*)` 标记。选 *保存按星期的设置* 会一次性写入 7 天，按 (B) 退出则不保存。
- *就寝闹钟*：设置开始时间和第二天早上的解除时间（四位数字 HHMM，例如 2100），或者关闭闹钟。
- *到时间后*：选择 *中断软件* 或 *仅提醒*。
- *取消每日限时*：删除每日限时，执行前会确认。
- ⚠️ **不要把限时设得比“今天已经玩过的时间”还短**，否则写入后主机会立即锁定。

### 其他操作

- *设置 / 修改家长控制 PIN 码*：切换到系统界面设置 PIN 码，完成后自动返回。
- *删除全部家长控制*：清除 PIN 码和所有限制。执行前会确认，**不可恢复**。
- *用系统时间设置网络时钟*：设置网络时钟，见上面关于离线主机的说明。
- *保存诊断报告*：把只读诊断信息写到 SD 卡。
- 主菜单顶部一直显示状态栏，选 *刷新状态* 可以重新读取。

## 安装

从 [**Releases**](../../releases) 页面下载 `nx_pctl_manager.nro`，放到 SD 卡的 `/switch/` 目录，从 hbmenu 打开即可。

也可以下载同一页面的 `nx_pctl_manager.zip`，直接解压到 SD 卡根目录，`.nro` 会自动放到 `/switch/` 里。

## 从源码编译

需要 [devkitPro](https://devkitpro.org/) 和 `switch-dev` 工具链（含 libnx）：

```sh
export DEVKITPRO=/opt/devkitpro
make            # 生成 ./nx_pctl_manager.nro
make dist       # 打包 nx_pctl_manager.zip，可直接解压到 SD 卡
make nxlink     # 编译后通过 nxlink 推送到局域网内正在运行 hbmenu 的 Switch
make clean
```

没有本地工具链的话，可以用仓库里的 `./run.sh`，它通过官方 Docker 镜像 `devkitpro/devkita64` 编译：

- `./run.sh`：只编译。
- `./run.sh <switch-ip>`：编译后通过 nxlink 推送到主机。推送前先在 hbmenu 里按 Y。

## 许可证

GPLv3，见 [`LICENSE`](LICENSE)。

### 第三方组件

图形界面（v3.0.0 起）基于 **[borealis](https://github.com/xfangfang/borealis)**，一个仿 Horizon 系统风格的 Switch 自制软件 UI 库，采用 Apache License 2.0。所用版本固定在 `extern/borealis/`，许可信息见其中的 `LICENSE` 和 `NOTICE`。v2 的文字界面版本不依赖 borealis。
