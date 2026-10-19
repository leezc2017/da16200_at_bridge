# da16200_at_bridge 工程功能说明

## 1. 工程概述

本工程是基于瑞萨 RA4M2（e2studio + FSP）的固件，功能是把 **RA 板的 USB 串口数据重定向到 DA16200 WiFi 模块的 UART**，形成一个透传测试桥，用于通过 AT 命令测试 DA16200 的 WiFi 功能。

> **板卡来源**：RA4M2 开发板与 DA16200 WiFi 模块均为向 **RA 生态工作室**申请的测试板卡。

数据流向：

```
PC 串口助手 ──USB──> RA4M2 (SCI9) ──板内UART──> DA16200 (SCI0, AT口)
   <── 打印 [WIFI] 回显 <──                    <── 模块响应/上报 ──
```

- 用户在 PC 串口助手里输入的每一行命令，经 USB 串口送入 RA，回显为 `[TX]` 并转发给 DA16200（自动补 `\r\n` 行结束符）。
- DA16200 返回的数据逐行打印为 `[WIFI]`，回到 PC 串口助手。

本工程**不解析 AT 命令**，只做透明转发 + 行级打印，方便直接手工敲 AT 命令测试 WiFi 模块。

## 2. 硬件连接

| 项目 | 配置 |
|---|---|
| MCU | RA4M2（R7FA4M2AD），Cortex-M33 |
| USB 串口 | SCI 通道 9（`g_usb_uart`），115200 8N1 |
| WIFI 串口 | SCI 通道 0（`g_wifi_uart`），115200 8N1 |
| WIFI 侧引脚 | P410 = SCI0_RXD0（板收）、P411 = SCI0_TXD0（板发） |
| 对端模块 | DA16200，AT 命令口为 **UART1**（UART0 是调试控制台） |

接线注意：RA 的 TX/RX 需与 DA16200 的 RX/TX **交叉**相连，且接到模块的 UART1（AT 口），不是 UART0。

**实际连接方式：DA16200 WiFi 模块直接插在 RA4M2 板的 PMOD2 接口上**，无需飞线，模块通过 PMOD2 与 SCI0（P410/P411）相连。

### 实物连接图

![实物连接图：WiFi 模块插在 PMOD2](images/pmod2_connection.png)

## 3. 软件设计（src/hal_entry.c）

### 3.1 中断回调（ISR 只做入队）

- `usb_uart_cb()`：USB 串口收到字符（`UART_EVENT_RX_CHAR`）写入 `rx_ring[]` 环形缓冲（128 字节，满则丢弃）；发送完成置 `usb_uart_send_complete_flag`。
- `wifi_uart_cb()`：WIFI 串口同理写入 `wifi_rx_ring[]`。

接收**不使用 `R_SCI_UART_Read`**，完全依靠 `UART_EVENT_RX_CHAR` 事件逐字节入队，避免读长缓存导致的 `UART_EVENT_RX_COMPLETE` 等待问题。

### 3.2 主循环（src/hal_entry.c `hal_entry()`）

每 10ms 轮询一次：

1. `usb_uart_rx_process()`：从 `rx_ring` 取字节，按行组装（`\r` 或 `\n` 作为行结束），满行调用 `rx_line_flush()`。
2. `wifi_uart_rx_process()`：同理从 `wifi_rx_ring` 组行，满行以 `[WIFI]` 打印。

### 3.3 命令转发（`rx_line_flush()` / `wifi_line_send()`）

- 打印 `[TX] <命令>` 回显到 USB。
- `wifi_line_send()`：复制到临时缓冲，若行尾没有 `\r`/`\n` 则**自动补 `\r\n`**（这就是"PC 端按回车即可直接发 AT 命令"的实现），然后 `R_SCI_UART_Write` 发给 DA16200，等待发送完成标志。

### 3.4 输出标记约定

| 标记 | 含义 |
|---|---|
| `[TX]` | 用户输入的命令（已回显并转发给模块） |
| `[WIFI]` | DA16200 的响应或异步上报（如 `+WFJAP:1,...`、`+INIT:DONE`） |
| `[ERR]` | 串口写失败等错误 |

### 3.5 初始化顺序

`hal_entry()` 中先 `R_SCI_UART_Open` 两个串口（USB、WIFI），再打印开机 banner：

```
RA E2STUDIO da16200_at_bridge
USB input -> WIFI uart (auto CRLF), WIFI response back to USB
```

## 4. 使用方法

1. 打开 PC 串口助手，连接 RA 的 USB 串口，115200 8N1。
2. 复位板子，看到 banner 即固件运行正常。
3. 直接输入 AT 命令并回车，例如：

```
AT
[WIFI] OK

AT+VER
[WIFI] <版本信息>

HELP AT+WFJAPA
[WIFI]     AT+WFJAPA=<ssid>[,<key>][,<hidden>]
[WIFI] OK
```

4. 异步上报（连接结果等）随时会以 `[WIFI]` 打出来，属正常现象。

## 5. 典型 WiFi 测试流程（DA16200）

```
AT+WFMODE=?            查模式，0 = STA
AT+WFMODE=0            切到 Station 模式
AT+RESTART             重启生效，等 +INIT:DONE
AT+WFSCAN              扫描周边 AP
AT+WFJAPA=<ssid>,<密码>    连接（密码 8~63 位）
AT+NWIP=?              查 IP
AT+NWSNTP=1,pool.ntp.org,86400   NTP 校时（验证出网）
AT+NWHTC=http://www.baidu.com,get   HTTP GET 测网站连接
AT+WFQAP               断开
ATF                    恢复出厂（清 NVRAM）
```

连接成功的标志：先回 `OK`，随后异步回 `+WFJAP:1,'<ssid>','<ip>'`。

### 常见错误码（UM-WI-003 Table 1）

| 码 | 含义 | 码 | 含义 |
|---|---|---|---|
| -1 | 未知命令 | -5 | 功能不支持（如非 STA 模式下发 WFJAPA） |
| -2 | 参数太少 | -6 | 未连接 AP |
| -3 | 参数太多 | -7 | 无结果 |
| -4 | 参数值错误 | -9 | 功能未配置 |

## 6. 编译

本工程无 make 于 PATH，使用 e2studio 自带 GNU make。

`Debug/` 下的构建描述文件已纳入版本控制：`makefile`、`makefile.init`、
`sources.mk`、`objects.mk`、各子目录的 `subdir.mk`，以及链接需要的
`fsp_gen.ld`、`memory_regions.ld`、`bsp_linker_info.h`。其中的工程路径已改为
相对路径（以 `Debug/` 为工作目录），因此新克隆的工程不必打开 e2studio 也能直接编译。

工具链根目录取自环境变量 `FSP_ROOT`，缺省值在 `makefile.init` 里写为
`C:/RA/e2studio_v2026-07_fsp_v6.6.0`。装在别的路径时，设好 `FSP_ROOT`
或改这一行即可。

```powershell
# 在 Debug\ 目录下执行
$mk = "$env:FSP_ROOT\eclipse\plugins\com.renesas.ide.exttools.gnumake.win32.x86_64_4.3.1.v20240909-0854\mk\make.exe"
& $mk -j8 all
```

注意：用 RASC 重新生成 FSP 代码时，e2studio 会按本机绝对路径重写这些 makefile
（并把行尾改回 CRLF）。换机器或重新生成后，需要把绝对路径再改回相对路径。

产物：`Debug/da16200_at_bridge.elf`、`Debug/da16200_at_bridge.hex`。

代码规模（当前）：`text 9252, data 100, bss 13444`。

## 7. 关键配置/背景知识

- `BSP_CFG_PARAM_CHECKING_ENABLE = 0`（ra_cfg/fsp_cfg/bsp/bsp_cfg.h）：FSP 参数检查关闭，对未 Open 的串口句柄调用写接口会因 `p_reg == NULL` 直接 HardFault，因此两个串口必须先成功 `Open`。
- DA16200 的 AT 命令手册：UM-WI-003；`HELP <命令>` 可让固件自报真实语法（以固件自报为准，不同 SDK 版本语法有差异）。
- WPA2 口令必须 8~63 字符；SSID/口令含逗号或单引号时需用单引号整体包住。

## 8. 文件结构

```
da16200_at_bridge/
├── src/hal_entry.c        应用主逻辑（回调、行缓冲、转发、打印）
├── src/hal_warmstart.c    启动warm start（FSP生成）
├── ra_gen/                FSP 配置生成代码（hal_data、pin_data 等）
├── ra_cfg/                FSP 配置参数（含参数检查开关）
├── ra/                    FSP 驱动与 CMSIS 库
├── script/fsp.ld          链接脚本
├── Debug/                 构建描述（makefile/*.mk/*.ld，已入库）+ 产物（忽略）
├── doc/                   本说明文档
├── .project / .cproject   e2studio 工程文件
└── configuration.xml      FSP 配置快照
```
