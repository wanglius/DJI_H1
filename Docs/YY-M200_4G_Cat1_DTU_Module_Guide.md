# YY-M200 — 4G Cat-1 DTU Module: Complete Integration & Usage Guide

> **Purpose of this document:** a machine- and agent-readable conversion of the official
> *YY-M200 手册* (42-page Chinese PDF manual). It contains **all** usage information from
> the manual — electrical specs, pinout, operating modes, every AT command with its
> parameters and defaults, and worked configuration examples — so that a non-PDF agent
> can integrate, configure and debug the module without access to the original PDF.
>
> **Source:** vendor manual, "手册适用产品 型号 YY-M200 (CAT1 模块)".
> Text is translated from Chinese to English; **AT commands, parameter names, numeric
> limits and default values are preserved verbatim** from the original.

---

## 1. What this module is

**YY-M200** is a serial-to-cellular **DTU (data terminal unit)** module: it transparently
bridges a serial port (UART/TTL or RS485) to a 4G **LTE Cat-1** network.

- Bidirectional **serial ⇄ 4G (Cat-1)** data transfer.
- Works on all three Chinese carriers (China Mobile / Unicom / Telecom — "全网通").
- Wide-voltage supply, 2.54 mm DIP pin-header package.
- Supports **remote firmware upgrade** and **remote (network) AT commands** for
  configuration and maintenance.
- Built-in **independent hardware watchdog** plus multiple exception-handling
  mechanisms for reliable unattended operation.

**Intended applications:** power grid, transportation, fire protection, industrial
production, meteorology/environment, agriculture & forestry, mining, and similar
distributed/remote telemetry scenarios.

### Product feature list (as stated by the vendor)

- 4G Cat-1, all-network (全网通)
- 5 Mbps upload / 10 Mbps download
- 5–36 V wide-voltage supply
- 3.3 V TTL interface / RS485 interface
- 2.54 mm DIP pin-header package
- Watchdog supervision
- Supports **4 Socket transparent-transmission channels**
- Multi-protocol: **TCP / UDP / HTTP / MQTT / point-to-point**
- Supports **offline caching**
- Supports **local (near-end) and remote upgrade**
- Supports **serial-port AT** and **network AT** commands

> Note: **GPS is only supported on some variants** of this product family.

---

## 2. Specifications

### 2.1 General / electrical

| Item | Parameter |
|---|---|
| Dimensions | 30 × 30 mm |
| Package | 2.54 mm pin header / socket header |
| Operating temperature | −35 … +70 °C |
| Operating humidity | 5 % … 95 % |
| Supply voltage | **5–36 V** |
| Supply current | 100 mA @ 12 V |
| Interface type | 3.3 V TTL / RS485 (pin header or socket) |
| USB interface | Micro-USB |

> ⚠️ Inconsistency in the source manual: the specification table states **5–36 V** supply,
> while the pin-description tables state "**5–16 V**" for pin 1 (VIN). Treat the pin-level
> figure as the conservative/authoritative limit for the header input.

### 2.2 Radio frequency

| Item | Parameter |
|---|---|
| Bands | **LTE-FDD:** B1/B3/B5/B8 (up/down: 5/10 Mbps) |
| | **LTE-TDD:** B34/B38/B39/B40/B41 (up/down: 2/8 Mbps) |
| Transmit power | LTE-FDD: Class 3 (23 dBm ± 2 dB) |
| | LTE-TDD: Class 3 (23 dBm +1/−3 dB) |

### 2.3 Serial port

| Item | Options / parameter |
|---|---|
| Baud rate | 1200 … 921600 bps (default **115200**) |
| Parity | NONE / ODD / EVEN (default NONE) |
| Data bits | 7 / 8 (default 8) |
| Stop bits | 1 / 2 (default 1) |
| Hardware flow control | NFC (none) |

### 2.4 Software / protocol capability

| Item | Parameter |
|---|---|
| Network protocols | DNS / TCP / UDP / PING |
| Transparent (DTU) protocols | TCP Client / UDP Client / HTTP / MQTT / point-to-point |
| AT commands | Serial AT / Network AT |
| Device maintenance | Local upgrade, network upgrade, exception handling |
| USB network-card function (RNDIS/ECM) | Windows and above; Linux 2.6 and above; Android 4 and above |

### 2.5 LED indicators and button

| Item | Behaviour |
|---|---|
| **Power** LED | On (steady) after power-up |
| **NET** LED | Slow blink at **1 s period** once successfully connected to the server |
| **Reload** button | Long-press **5–10 s** → restore factory settings |

**NET LED detailed status map:**

| Condition | NET LED behaviour |
|---|---|
| SIM card not recognised | 5000 ms ON / 5000 ms OFF |
| SIM card OK but cannot register on network | 100 ms blink (fast) |
| Registered on network but not connected to server | 500 ms slow blink |
| Successfully connected to server | 1000 ms slow blink (at least one channel connected to server) |

> The `NETS_LED` signal drives a green LED through transistor Q1 to indicate network status.

---

## 3. Hardware interface and pinout

The module is a 4-pin device; pins are numbered **1–4 counting left to right**.

### 3.1 RS485 variant

| Pin | Name | Type | Description |
|---|---|---|---|
| 1 | VIN | P (power) | Module supply positive; wide-voltage input, supports 5–16 V |
| 2 | DGND | P (power) | Digital ground |
| 3 | TX_485A | O (output) | RS485 differential signal, phase A |
| 4 | RX_485B | I (input) | RS485 differential signal, phase B |

### 3.2 TTL variant

| Pin | Name | Type | Description |
|---|---|---|---|
| 1 | VIN | P (power) | Module supply positive; wide-voltage input, supports 5–16 V |
| 2 | DGND | P (power) | Digital ground |
| 3 | TX | O (output) | TX output → connect to host serial **RX** |
| 4 | RX | I (input) | RX input → connect to host serial **TX** |

**Pin type key:** `P` = power pin, `I` = input, `O` = output.

**Typical wiring (TTL variant):** host MCU/USB-UART `TX → module RX (pin 4)`,
host `RX ← module TX (pin 3)`, grounds common, VIN from a 5–16 V supply.
Cross-connect TX/RX — the module's TX goes to the host's RX.

---

## 4. Getting started: the configuration tool

The device ships with a dedicated configuration tool (a PC application) that removes the
need to hand-craft AT traffic.

**Start-up sequence:**

1. Set the serial-port parameters in the tool.
2. Click **打开串口 ("Open serial port")**.
3. Wait for the device to boot, then click **进入配置 ("Enter configuration")**.
4. The tool confirms when configuration mode has been entered successfully.
5. From there you can send/receive AT commands to read and write device parameters.

**Important notes:**

1. New parameters require clicking **保存 ("Save")** and then a **reboot** to take effect.
2. The device **automatically exits configuration state after a reboot**.
3. In configuration state the serial port is used **only** for AT read/write —
   **Socket communication does not work** while in configuration state.

Typical tool UI (Tabs): `设备状态 | 基本参数 | 通道A | 通道B | 通道C | 通道D | JSON配置 | Modbus点表配置`.
Footer buttons: `导出 (Export) | 导入 (Import) | 进入配置 (Enter config) | 退出配置 (Exit config) | 读取 (Read) | 保存 (Save) | 重启 (Reboot)`.
The tool also has a log/console pane with **RX/TX timestamped packet view**, a
**show-timestamp** checkbox, a **hex-display** checkbox, and an AT-command input line with
an **append CR/LF** checkbox.

---

## 5. Serial port (UART) usage

The module exposes one serial port using either **RS485 or TTL** physical layer.

To keep AT-command naming uniform across the product family, **every AT command related
to the serial or Socket functions carries a serial-index `n`** (e.g. `AT+UARTn`). On
single-serial-port products, **`n` is always 1**.

### 5.1 Serial parameters

| Item | Options | Parameter |
|---|---|---|
| Working mode | Command/AT mode | — |
| | **Transparent mode (default)** | — |
| Baud rate | 1200 … 921600 bps | default 115200 |
| Parity | NONE / ODD / EVEN | default NONE |
| Data bits | 7 / 8 | default 8 |
| Stop bits | 1 / 2 | default 1 |
| Hardware flow control | NFC | — |

### 5.2 Serial working modes

The serial port supports two working modes:

- **AT command configuration mode** — data received on the serial port is interpreted as
  commands. Data arriving from the network is **discarded**. Parameters can be queried and
  set in this mode; **the device reverts to transparent mode after a reboot** (command
  mode is *not* saved across power cycles).
- **Transparent mode** — data received on the serial port is forwarded over the Socket.
  This is the **default mode at power-on**.

Commands may be sent from a PC or from an MCU over the serial link. **Each command line
may contain only one AT command; a single command is at most 256 bytes.** New parameters
are saved automatically but **take effect only after a reboot**.

#### 5.2.1 Entering AT command mode

Sequence terms: **UART** = the user's serial device, **DTU** = this product.

1. The user device sends `+++`
2. The user device waits — **longer than the packing time, shorter than 3 s; 500 ms is
   recommended** — and then sends `a`
3. The user device checks whether it received `+ok`
   (**you must receive `+ok` before you can send commands**)

Notes:

- If the "enter command mode" action is aborted, data sent in the meantime is forwarded
  to the network.
- Receiving `+ok` on the user serial port means entry into AT command mode succeeded.
  **Command mode is not retained across power loss.**
- To reduce interference from network-side data during this process, the product
  **suspends network data output for up to 3 seconds** after receiving `+++`. Even so, the
  `a` may sometimes get mixed with other data — hence the recommended 500 ms delay above.

#### 5.2.2 Exiting command mode

From AT command mode, switch back to transparent mode with the AT command **`AT+EXIT`**
or by **rebooting the device**.

> Commands must end with carriage-return + line-feed, i.e. the escape sequence `\r\n`.

### 5.3 Packing mechanism

To improve network transmission performance, serial-received data is first **packed into
a frame** before being forwarded to the network. Two packing criteria are supported, and
**forwarding happens as soon as either one is met**:

- **Length packing:** data length ≥ packing length (**default 1024**, range 64 … 1024)
- **Interval packing:** gap between adjacent characters ≥ packing interval
  (**default 5 ms**, range 1 … 300 ms)

> ⚠️ Under **TCP**, data may arrive coalesced ("连包" / packet sticking). **If packet
> boundaries matter, the application layer must implement its own de-framing /
> packet-splitting mechanism.**

Related commands: `AT+UARTTLn` (packing interval & length), `AT+UARTn` (serial params).

---

## 6. Data transmission

Each serial port supports **4 Socket channels: SocketA / SocketB / SocketC / SocketD**.
By default **only SocketA is enabled**. When multiple sockets are enabled simultaneously,
serial-received data is forwarded **to every channel**, and network data received is
output **sequentially through the same serial port**.

> **Terminology:** *transparent transmission* means only forwarding the data without
> changing its content.

The base block diagram also shows optional **GPS** support (only on some products).

### 6.1 TCP Client

TCP is a connection-oriented, reliable transport protocol — recommended where data
integrity requirements are strict.

- TCP is a **C/S architecture**: server listens passively on a port; the client actively
  initiates a connection; after the connection is established both sides can exchange data.
- This product **enables Keepalive** when using TCP, which effectively prevents dead links.
- As a **TCP Client**, the device automatically initiates a connection to the server after
  power-up and network registration. **If the connection fails it automatically re-tries
  every 1 second.**

*Example:* device as TCP Client to a server — after save + reboot the device sends
`hello` and the server returns `hello`.

### 6.2 UDP Client

UDP is a connectionless transport protocol providing unreliable delivery, but:

- No connection setup or transmission control → **higher efficiency**.
- No resources spent maintaining a connection.

Strictly, UDP peers are fully symmetric — there is no Client/Server distinction and no
connection. A party merely needs the peer's IP and port to send data.

- In **UDP Client** mode the **local port is random**, while the **destination IP and port
  are fixed** (data can only be sent to that specific target).
- Because the local port is random, the peer does not know the device's port — **the
  device must send one packet first before the two sides can exchange data.**

Configuration is the same as TCP Client, only the protocol type differs.

### 6.3 HTTP Client

HTTP is a simple request-response protocol: the client issues a request, the server
responds. Supported versions: **HTTP/1.0 and HTTP/1.1**, with **GET / POST** requests.
An **AUTO** mode lets the user switch request style flexibly at request time.

**Test endpoints used in the manual:**

- **GET** interface: `http://www.rt-thread.com/service/rt-thread.txt`
  → returns an rt-thread introduction on success.
- **POST** interface: `http://www.rt-thread.com/service/echo`
  → returns exactly the data submitted.

> **Note:** HTTP **shares the address and port parameters with Socket** — make sure the
> corresponding Socket function is enabled before using HTTP. The `HTTP://` prefix in the
> address may be omitted.

#### 6.3.1 GET request

In GET mode the device's **URL parameter is ignored**: the device takes the data received
on the serial port **as the URL**, triggers the GET request, and outputs the server's
response data on the serial port.

- Example address: `http://www.rt-thread.com/service/rt-thread.txt` — after the serial
  port receives the data, the device outputs the server's response.

> **Note:** the URL set by `AT+HTPURLn` is **valid only for POST requests**. If there are
> multiple HTTP headers, separate them with `|`.

#### 6.3.2 POST request

Configured through the tool (Channel A example values):

| Setting | Value |
|---|---|
| Channel switch | Enabled |
| Protocol mode | HTTP Client |
| Server address | `www.rt-thread.com` |
| Server port | `80` |
| HTTP request method | `POST` |
| HTTP URL | `/service/echo` |
| HTTP timeout | `6` (seconds) |
| HTTP header filtering | off |
| HTTP protocol headers | `Connection: close|Accept: */*` |

The server address + port + URL combine into `http://www.rt-thread.com:80/service/echo`.
The console log shows the round-trip of payload `123` (TX from serial → RX echoed back),
i.e. transmit `123`, receive `123`.

#### 6.3.3 AUTO mode

In **AUTO** mode the user only needs to switch to HTTPC mode and set the headers; all
other parameters — server address, port, URL, request method, data — can be changed
flexibly at the moment of sending.

- **GET request:** send the complete URI on serial port `n`, in any of these formats
  (**port 80 is used by default when unspecified**):
  - `http://www.rt-thread.com/service/rt-thread.txt`
  - `http://www.rt-thread.com:80/service/rt-thread.txt`
  - `www.rt-thread.com/service/rt-thread.txt`
- **POST request format:** URI and data are separated by CRLF (`\r\n`), in any of:
  - `http://www.rt-thread.com/service/echo\r\nTEST`
  - `http://www.rt-thread.com:80/service/echo\r\nTEST`
  - `www.rt-thread.com/service/echo\r\nTEST`

Related commands: `AT+HTPURLn`, `AT+HTPHDn`, `AT+HTPFTn`, `AT+HTPREQn`, `AT+HTPTOn`
(all require firmware support `UE_HTTPC_ENABLE`; `n = 1…3`).

### 6.4 MQTT

MQTT is a lightweight publish/subscribe messaging protocol over TCP/IP; it allows message
payload masking with low overhead, effectively reducing network traffic.

- The DTU's MQTT function is a **transparent-like mechanism**: on connecting to the
  server it **automatically subscribes to a preset topic** and **presets one topic for
  publishing**. The DTU internally handles MQTT subscribe/publish, so **the user terminal
  device only needs to receive and send message content**.
- MQTT has many parameters; configure them per your own requirements.

Related commands: `AT+MQCONFn`, `AT+MQAUTHn`, `AT+MQSUBn`, `AT+MQPUBn`, `AT+MQWILLn`,
`AT+MQMDn`.

### 6.5 Modbus gateway function

- **Multiple polling commands** can be pre-stored in the device; the device sends them
  to the serial port in sequence. This reduces server-side load and lowers traffic usage.
- Supports conversion between **Modbus RTU and Modbus TCP**.
- **Multiple polling commands must be separated by `|`.**

**Edge-collection / JSON mode** (`JSON模式`) options:

- `ALL` — bidirectional transparent transmission (Modbus ⇄ JSON both ways)
- `POLL` — transparent transmission disabled
- `OFF` — Modbus-to-JSON conversion disabled

*Tool example:* polling enabled, polling command interval 1000 ms, polling cycle 60 s,
polling command `010100000002BDCB` (hex).

Related commands: `AT+MBCFGn`, `AT+MBCMDn`.

### 6.6 Heartbeat packets and registration packets

Heartbeat and registration packets are **additional features of transparent
transmission**. **Each serial port can independently enable its heartbeat and
registration function; both are disabled by default.**

#### 6.6.1 Heartbeat packet

An application-layer keep-alive mechanism that periodically sends data to the network or
the serial port so the user can confirm the device is working normally.

- The **network heartbeat packet** supports **TCP / UDP Client** modes.
- The **serial heartbeat packet** is not restricted by network mode; **when the device
  enters AT command state, heartbeat sending is suspended.**

**Example — enable serial heartbeat on serial port 1, one packet every 30 s:**

```
AT+HEARTMD1=UART     # heartbeat mode = serial port
AT+HEARTTM1=30       # heartbeat interval = 30 s
AT+REBOOT            # take effect after reboot
```

#### 6.6.2 Registration packet

When the device communicates with the user's server it can **actively send specific data**
so the server can identify the client. Registration packets support **TCP / UDP Client**
and offer three sending methods:

- **First only (`FIRST`)** — TCP Client: reported after **every** successful connection;
  UDP Client: reported **once** after getting on the network.
- **Data-carried (`EVERY`)** — sent as the **data packet header** together with the data.
- **First + data-carried (`ALL`)** — both of the above.

**Example — heartbeat/registration on power-up on serial port 1, content = IMEI, sent once:**

```
AT+REGTP1=IMEI       # registration packet type = IMEI
AT+REGMD1=FIRST      # enable registration packet, send only the first time
AT+REBOOT            # take effect after reboot
```

---

## 7. Advanced functions

### 7.1 Cell-tower (base-station) positioning

- Users with low accuracy requirements may use cell-tower positioning; it also works
  **indoors**. Accuracy depends on local tower density and the tower database:
  - Urban: typically **50–150 m**
  - Suburban: **100–300 m**
  - Rural: **200–2000 m**
- The device does **not** output latitude/longitude directly: you read the tower
  information first and then resolve coordinates through a third party
  (the manual's example service is `http://www.cellocation.com/`).

**Procedure:**

1. Read the tower information by AT command:
   - Send `AT+LBS\r\n`
   - Device returns CID and LAC, e.g. `+LBS:144426439,21269`
2. Use the returned `<cid>,<lac>` with a third-party service to obtain lat/lon.

### 7.2 Offline caching

When enabled, if the device goes offline the data is **temporarily stored** on the device
and forwarded to the server once the device is online again.

- **Cache size: 20 KB; maximum 50 data items.**

**Example:**

```
AT+CACHE1=ON     # enable offline caching on serial port 1
AT+REBOOT        # take effect after reboot
```

### 7.3 Data format conversion

Converts data formats in the **serial→network** or **network→serial** direction.
Currently supports **binary ⇄ string** conversion.

*Example scenario:* backend personnel handle data as **strings**, while the terminal
device produces **Modbus RTU** data.

- Related command: `AT+DTCVTn` (`RAW` / `BTS` / `STB` per direction).

### 7.4 Enhanced AT commands (transparent AT)

Allows sending AT commands to the device **while it is in transparent mode**. Supported
data sources: **serial port, network, SMS**.

When enabled, the device inspects incoming data; if it is preceded by a **specific
keyword**, the data is executed as an AT command and the result is returned to the sender.

**Example:**

```
AT+EXAT=NET,NET@     # enable network AT only, keyword "NET@"
AT+REBOOT            # take effect after reboot
```

Then the server sends `NET@AT+VER`; the device executes `AT+VER` and returns the version
information to the server.

You may enable **network AT only**, **serial AT only**, or both. **The device enables both
network and serial AT by default.**

### 7.5 Firmware upgrade

Two upgrade methods are supported. **Upgrading normally does not affect the device's
existing parameter configuration.**

#### 7.5.1 Local USB interface upgrade

> Contact customer service to obtain the driver before upgrading over USB.

#### 7.5.2 HTTP upgrade

The device supports upgrade over the HTTP protocol; the user triggers the upgrade with an
AT command. Procedure:

1. Put the firmware in an HTTP server directory. Assume server port is 8080 and firmware
   URI is `update.xxx.com:8080/xxx.bin`.
2. Send the AT command over serial or network:
   `AT+DOWNLOAD=update.xxx.com:8080/xxxl.bin`
3. On successful download the device replies `OK`, otherwise `FAIL`.
4. **After a successful download the device reboots automatically.**

### 7.6 Exception handling — no-data reboot

The device supports a **no-data reboot** function: if it receives **no network-downlink
data** for a configured period, it **automatically reboots**. This is **enabled by default
with a 24-hour timeout**. Setting command:

```
AT+SOCKRTO=time        # 0 ≤ time ≤ 4320, 0 = disable, unit: minutes
```

---

## 8. AT command reference

### 8.1 Command conventions

AT commands can be used to query and set parameters. Both **serial AT** and **network AT**
are supported. (For switching to serial AT mode see §5.2.1; for network AT entry see
§7.4.)

**Rules:**

1. Commands start with `AT+` and end with `\r` or `\n`.
2. Commands are **case-insensitive**; uppercase is recommended.
3. A line may contain **only one** AT command; a single command is at most **256 bytes**.
4. Multiple parameters are separated by an English half-width comma `,`.
5. **Wait for the previous command's result before sending a new one**
   (maximum command timeout **12 s**).

**Three command forms:**

| Form | Send | Response |
|---|---|---|
| Query | `AT+CMD\r\n` or `AT+CMD?\r\n` | `\r\n+CMD:value\r\nOK\r\n` |
| Set | `AT+CMD=value1,value2…\r\n` | `\r\nOK\r\n` |
| Help *(only for commands with settable parameters; returns ranges/formats)* | `AT+CMD=?\r\n` | `\r\n+CMD:(param1:range),(param2:range)…\r\nOK\r\n` |

Unless stated otherwise, command descriptions below **omit the `\r\n`**.

### 8.2 Error codes

On failure the device returns an error code in the format:

```
\r\n+ERROR:Error_Code\r\n
```

| Error Code | Error type | Cause |
|---|---|---|
| `ARGS` | Invalid parameter | Parameter length, size or format invalid |
| `ARGC` | Invalid parameter count | Wrong number of parameters |
| `CMD_UNKNOWN` | Unknown command | The command does not exist |
| `CMD_FORMAT` | Format error | Does not begin with `AT+` |
| `CMD_LENGTH` | Length error | Exceeds maximum command length |
| `DEV_MEMORY` | Memory error | Memory error |
| `DEV_SAVE` | Save failed | Save failed |

> The manual also mentions a §3.2 "AT command quick mastery" section that claims to show
> a handful of commonly used commands, but **the table itself is absent from the source
> document** (only the introductory sentence is present). The full command list below is
> the complete, authoritative reference.

### 8.3 System & device management

| Command | Purpose | Query | Set | Parameters / notes |
|---|---|---|---|---|
| `AT+LIST` | Show command list | `AT+LIST` → `+LIST:AT+CMD1\n+LIST:AT+CMD2\n…\nOK` | — | — |
| `AT+EXIT` | Exit command mode | `AT+EXIT` → `OK` | — | — |
| `AT+VER` | Query firmware version | `AT+VER` → `+VER:<ver>\nOK` | — | `<ver>` e.g. `V1.3.0` |
| `AT+RDM` | Remote device management | `AT+RDM` → `+RDM:<state>` | `AT+RDM=<state>` → `OK` | `<state>`: `ON`/`OFF` (**default OFF**). *After enabling remote management you must also set EDP parameters to connect to the remote-management platform.* |
| `AT+DEVINFO` | Query device information | `AT+DEVINFO` → `+MODULE:`, `+VERSION:`, `+IMEI:`, `+MAC:`, `+DECRYPT:`, `+BUILD:`, `+PROTIME:`, `+SN:` | — | `<MODULE>` product model; `<VERSION>` FW version; `<IMEI>`; `<DECRYPT>` decrypt state; `<BUILD>` build time; `<PROTIME>` production time; `<SN>` serial number |
| `AT+SN` | Query product serial number | `AT+SN` → `+SN:<sn>\nOK` | — | `<sn>` product serial number |
| `AT+REBOOT` | Reboot device | `AT+REBOOT` → `OK` | — | — |
| `AT+RSTCFG` | Restore **backup** parameters and auto-reboot | `AT+RSTCFG` → `OK` | — | Effect identical to the **Reload** button |
| `AT+BKCFG` | Back up current running parameters | `AT+BKCFG` → `OK` | — | — |
| `AT+CLRCFG` | Restore **factory default** parameters and auto-reboot | `AT+CLRCFG` → `OK` | — | The device has **3 parameter zones**: working (running) / backup (for restore) / factory-cured |
| `AT+SOCKLK` | Query TCP Client connection state (UDP and TCP Server query result is `OFF`) | All: `AT+SOCKLK` → `+SOCKLK:ns,<state>` … `OK`; One: `AT+SOCKLK=ns` → `+SOCKLK:<state>\nOK` | — | `n` serial no, `s` = `A`…`D`; `<state>` `OFF` = disconnected, `ON` = connected |
| `AT+DOWNLOAD` | HTTP network firmware upgrade | — | `AT+DOWNLOAD=uri` → `<state>` | `<uri>` firmware address (port defaults to 80), e.g. `update.xxx.com/firmware.bin` or `192.168.1.56:8080/firmware.bin`. `OK` = download succeeded (**manual reboot required to upgrade**); `+ERROR: FAIL` = download failed |
| `AT+LOG` | Log output configuration | `AT+LOG` → `+LOG:<port>,<color>,<time>` | `AT+LOG=<port>,<color>,<time>` → `OK` | `<port>` 0 off / 1 serial / 2 network; `<color>` 0 off / 1 on; `<time>` 0 off / 1 on |
| `AT+BOOTINFO` | Boot start-up info | `AT+BOOTINFO` → `+BOOTINFO:<info>` | `AT+BOOTINFO=<info>` → `OK` | `<info>` 1–16 byte string, default `Start` |
| `AT+ECHO` | Command echo toggle | `AT+ECHO` → `+ECHO:<state>` | `AT+ECHO=<state>` → `OK` | `<state>` `ON`/`OFF` (**default OFF**) |
| `AT+EXAT` | Enhanced AT (transparent AT) | `AT+EXAT` → `+EXAT:<mode>,<key>` | `AT+EXAT=<sta>,<key>` → `OK` | `<mode>`: `OFF` / `NET` network AT / `UART` serial AT / `ALL` network+serial AT (**default ALL**). `<key>` command keyword, 1–16 bytes, default `NAT@` |
| `AT+SOCKRTO` | Timeout-based reboot when no network data is received | `AT+SOCKRTO` → `+SOCKRTO:<time>` | `AT+SOCKRTO=<time>` → `OK` | `<time>` 0–4320, **default 1440 (24 h)**, 0 = disabled, unit **minutes** |
| `AT+CCLK` | Query UTC time | `AT+CCLK` → `+CCLK:<time>\nOK` | — | `<time>` format `YY/MM/DD,hh:mm:ss+TZ` |
| `AT+LBS` | Query cell-tower location info | `AT+LBS` → `+LBS:<cid>,<lac>\nOK` | — | `<cid>` cell ID; `<lac>` location area code |
| `AT+SIMSEL` | Query SIM-select GPIO state | `AT+SIMSEL` → `+SIMSEL:<state>\nOK` | — | Requires firmware support `UE_SIM_ENABLE`; `<state>` 0/1 |

### 8.4 Network & access

| Command | Purpose | Query | Set | Parameters / notes |
|---|---|---|---|---|
| `AT+CSQ` | Signal strength | `AT+CSQ` → `+CSQ:<rssi>` | — | `<rssi>`: 0 ≤ −115 dBm; 1 = −111 dBm; 2–30 = −109…−53 dBm; 31 ≥ −51 dBm; 99 = no signal |
| `AT+CEREG` | Network registration state | `AT+CEREG` → `+CEREG:<sta>` | — | `<sta>` 0 = not registered / 1 = registered (can send/receive SMS) |
| `AT+CGATT` | Data network activation state | `AT+CGATT` → `+CGATT:<sta>` | — | `<sta>` 0 = inactive / 1 = active (data transfer possible) |
| `AT+IP` | Network IP address | `AT+IP` → `+IP:<ip>` | — | `<ip>` IPv4 address |
| `AT+IMEI` | Device IMEI | `AT+IMEI` → `+IMEI:<imei>` | — | 15-digit number |
| `AT+ICCID` | SIM ICCID | `AT+ICCID` → `+ICCID:<iccid>` | — | 20-byte string |
| `AT+IMSI` | SIM IMSI | `AT+IMSI` → `+IMSI:<imsi>` | — | 15-byte string |
| `AT+PING` | PING command | — | `AT+PING=<addr>` → `+PING:<result>` | `<addr>` IP or domain; `<result>` = `Network not available` / `Timeout` / `Unknown host` / `Number(ms)` |
| `AT+APN` | APN (VPDN) parameters | `AT+APN` → `+APN:<mode>,<apn>,<user>,<pwd>,<auth>` | `AT+APN=<mode>,<apn>,<user>,<pwd>,<auth>` → `OK` | `<mode>` 0 = manual (user APN) / 1 = auto (default APN, **default**); `<apn>` ≤ 64 bytes; `<user>` ≤ 32 bytes; `<pwd>` ≤ 32 bytes; `<auth>` 0 `NONE` / 1 `PAP` / 2 `CHAP` |
| `AT+NTPEN` | NTP time-sync toggle | `AT+NTPEN` → `+NTPEN:<state>` | `AT+NTPEN=<state>` → `OK` | `ON`/`OFF` (**default OFF**) |
| `AT+NTP` | NTP server configuration | `AT+NTP` → `+NTP:<server>,<period>` | `AT+NTP=<server>,<period>` → `OK` | `<server>` NTP server address; `<period>` sync interval in **minutes**, max 1440 (24 h) |

### 8.5 Socket / data transmission

| Command | Purpose | Query | Set | Parameters / notes |
|---|---|---|---|---|
| `AT+SOCKENns` | SocketA/B enable switch for serial port `n` | `AT+SOCKENns` → `+SOCKENns:<state>` | `AT+SOCKENns=<state>` → `OK` | `<state>` `ON` (**default: only serial port 1's Socket A is enabled**) / `OFF`; `n` = serial no, `s` = `A` or `B` |
| `AT+SOCKns` | Socket parameters | `AT+SOCKns` → `+SOCKns:<type>,<addr>,<port>` | `AT+SOCKns=<type>,<addr>,<port>` → `OK` | `<type>`: `TCPC` TCP client / `UDPC` UDP client / `HTPC` HTTP client (**SOCKnA only**) / `EDP` point-to-point (**SOCKnA only**) / `MQTT` MQTT transparent. `<addr>` remote server address or target phone number, ≤ 64 bytes; **SSL/TLS not currently supported**. `<port>` remote server port. In SMS transparent mode a non-zero port means only SMS from that target number is received; 0 means SMS from any number is received |
| `AT+CLINUMnA` | TCP maximum connection count | `AT+CLINUMnA` → `+CLINUMnA:<max>,<strategy>` | `AT+CLINUMnA=<max>,<strategy>` → `OK` | `<max>` max TCP connections; `<strategy>` `FIFO` / `LIFO`. `nA~nD` correspond to SocketA~SocketD; `CLINUM1B` / `CLINUM1C` / `CLINUM1D` use the same format |
| `AT+CACHEn` | Offline cache function | `AT+CACHEn` → `+CACHEn:<state>` | `AT+CACHEn=<state>` → `OK` | `<state>` `ON` (**default**) / `OFF` |
| `AT+UARTn` | Serial port parameters | `AT+UARTn` → `+UARTn:<baudrate>,<databits>,<stopbits>,<parity>,<fc>` | `AT+UARTn=<baudrate>,<databits>,<stopbits>,<parity>,<fc>` → `OK` | `<baudrate>` default 115200, selectable 2400–921600; `<databits>` 7/8 (default 8); `<stopbits>` 1 (default) / 2; `<parity>` `NONE` (default) / `EVEN` / `ODD`; `<fc>` `NFC` (default) |
| `AT+UARTTLn` | Serial packing interval and length | `AT+UARTTLn` → `+UARTTLn:<tm>,<len>` | `AT+UARTTLn=<tm>,<len>` → `OK` | `<tm>` packing interval 1–300 ms, default 5; `<len>` packing length 64–1024, default 1024 |
| `AT+DTCVTn` | Data ASCII/HEX conversion | `AT+DTCVTn` → `+DTCVTn:<up>,<down>` | `AT+DTCVTn=<up>,<down>` → `OK` | `<up>` serial→network: `RAW` (default) / `BTS` binary→string (e.g. `0x11 0xAB` → `'11AB'`) / `STB` string→binary (e.g. `'11AB'` → `0x11 0xAB`). `<down>` network→serial: `RAW`/`BTS`/`STB` as above |

### 8.6 MQTT

| Command | Purpose | Query | Set | Parameters / notes |
|---|---|---|---|---|
| `AT+MQCONFn` | MQTT connection parameters (`n` = serial no) | `AT+MQCONFn` → `+MQCONFn:<ver>,<clean>,<keepalive>` | `AT+MQCONFn=<ver>,<clean>,<keepalive>` → `OK` | `<ver>` MQTT version: 3 = 3.1, 4 = 3.1.1 (**default**); `<clean>` clear session: 0 = no, 1 = yes (**default**); `<keepalive>` keepalive interval 30–65535 s |
| `AT+MQAUTHn` | MQTT authentication | `AT+MQAUTHn` → `+MQAUTHn:<id>,<user>,<pass>` | `AT+MQAUTHn=<id>,<user>,<pass>` → `OK` | `<id>` client ID, 1–64 bytes, **must be unique per client on the same server**; `<user>` 1–64 bytes; `<pass>` 1–128 bytes |
| `AT+MQSUBn` | MQTT subscribe settings | `AT+MQSUBn` → `+MQSUBn:<enable>,<topic>,<qos>` | `AT+MQSUBn=<enable>,<topic>,<qos>` → `OK` | `<enable>` 1 enable (**default**) / 0 disable; `<topic>` 1–64 bytes; `<qos>` 0 QoS0 at-most-once / 1 QoS1 at-least-once / 2 QoS2 exactly-once |
| `AT+MQPUBn` | MQTT publish settings | `AT+MQPUBn` → `+MQPUBn:<enable>,<topic>,<qos>,<retain>` | `AT+MQPUBn=<enable>,<topic>,<qos>,<retain>` → `OK` | `<enable>` 1 enable (**default**) / 0 disable; `<topic>` 1–64 bytes; `<qos>` 0 QoS0 (**default**) / 1 / 2; `<retain>` 0 no (**default**) / 1 retain; `n` serial no |
| `AT+MQWILLn` | MQTT will message | `AT+MQWILLn` → `+MQWILLn:<enable>,<topic>,<qos>,<msg>,<retain>` | `AT+MQWILLn=<enable>,<topic>,<qos>,<msg>,<retain>` → `OK` | `<enable>` 1 enable / 0 disable (**default**); `<topic>` 1–64 bytes; `<qos>` 0 QoS0 (**default**) / 1 / 2; `<msg>` message content 1–64 bytes; `<retain>` 0 no (**default**) / 1 yes |
| `AT+MQMDn` | MQTT mode setting | `AT+MQMDn` → `+MQMDn:<mode>` | `AT+MQMDn=<mode>` → `OK` | `<mode>` `STD` standard MQTT (**default**) / `ALI` Alibaba Cloud mode (auto-computes Alibaba Cloud authentication info) |

### 8.7 HTTP client

> All HTTP commands require firmware support `UE_HTTPC_ENABLE`; `n = 1…3`.

| Command | Purpose | Query | Set | Parameters / notes |
|---|---|---|---|---|
| `AT+HTPURLn` | HTTP URL configuration | `AT+HTPURLn` → `+HTPURLn:<url>` | `AT+HTPURLn=<url>` → `OK` | `<url>` HTTP server URL |
| `AT+HTPHDn` | HTTP request header configuration | `AT+HTPHDn` → `+HTPHDn:<headers>` | `AT+HTPHDn=<headers>` → `OK` | `<headers>` custom HTTP headers; **multiple groups separated by the vertical bar `|`** |
| `AT+HTPFTn` | HTTP response filtering toggle | `AT+HTPFTn` → `+HTPFTn:<state>` | `AT+HTPFTn=<state>` → `OK` | `<state>` `ON` / `OFF` (**default OFF**) |
| `AT+HTPREQn` | HTTP request method | `AT+HTPREQn` → `+HTPREQn:<method>` | `AT+HTPREQn=<method>` → `OK` | `<method>` `GET` / `POST` / `AUTO` / `GETS` |
| `AT+HTPTOn` | HTTP timeout | `AT+HTPTOn` → `+HTPTOn:<timeout>` | `AT+HTPTOn=<timeout>` → `OK` | `<timeout>` 1–30 **seconds** |

### 8.8 Registration & heartbeat

> `n` = serial port number.

| Command | Purpose | Query | Set | Parameters / notes |
|---|---|---|---|---|
| `AT+REGTPn` | Registration packet type | `AT+REGTPn` → `+REGTPn:<type>` | `AT+REGTPn=<type>` → `OK` | `<type>`: `IMEI` (15-byte HEX) / `ICCID` (20-byte HEX) / `USER` (custom) |
| `AT+REGMDn` | Registration packet send mode | `AT+REGMDn` → `+REGMDn:<mode>` | `AT+REGMDn=<mode>` → `OK` | `<mode>`: `OFF` disabled (**default**) / `FIRST` first-send-only / `EVERY` data-carried / `ALL` = `FIRST` + `EVERY` |
| `AT+REGDATn` | Custom registration packet content | `AT+REGDATn` → `+REGDATn:<data>,<fmt>` | `AT+REGDATn=<data>,<fmt>` → `OK` | `<data>` custom content; `<fmt>` `HEX` (max 64 bytes) / `ASCII` (max 32 bytes) |
| `AT+HEARTMDn` | Heartbeat mode | `AT+HEARTMDn` → `+HEARTMDn:<mode>` | `AT+HEARTMDn=<mode>` → `OK` | `<mode>`: `OFF` disabled (**default**) / `UART` serial heartbeat / `NET` network heartbeat |
| `AT+HEARTTMn` | Heartbeat interval | `AT+HEARTTMn` → `+HEARTTMn:<time>` | `AT+HEARTTMn=<time>` → `OK` | `<time>` 1–86400 s, **default 60** |
| `AT+HEARTDATn` | Heartbeat packet content | `AT+HEARTDATn` → `+HEARTDATn:<data>,<fmt>` | `AT+HEARTDATn=<data>,<fmt>` → `OK` | `<data>` custom content; `<fmt>` `HEX` (max 64 bytes) / `ASCII` (max 32 bytes) |
| `AT+HEARTREGn` | Heartbeat-append registration-packet prefix | `AT+HEARTREGn` → `+HEARTREGn:<state>` | `AT+HEARTREGn=<state>` → `OK` | `<state>` `ON` / `OFF` (**default OFF**) |

### 8.9 Modbus gateway

| Command | Purpose | Query | Set | Parameters / notes |
|---|---|---|---|---|
| `AT+MBCFGn` | Serial polling configuration | `AT+MBCFGn` → `+MBCFGn:<enable>,<tv>,<period>` | `AT+MBCFGn=<enable>,<tv>,<period>` → `OK` | `<enable>` 0 disabled (**default**) / 1 enabled; `<tv>` interval between adjacent polling commands, **100–35535 ms**; `<period>` polling cycle, **1–2592000 s**; `n` serial no |
| `AT+MBCMDn` | Serial polling commands | `AT+MBCMDn` → `+MBCMDn:<cmd>` | `AT+MBCMDn=<cmd>` → `OK` | `<cmd>` polling command in **HEX format (no spaces)**; multiple commands separated by `|`; **total length ≤ 240 bytes**; `n` serial no |

### 8.10 Positioning

| Command | Purpose | Behaviour | Notes |
|---|---|---|---|
| `AT+GPS` | Query GNSS positioning coordinates | Success → coordinates output per `GPSCFG` format. Locating: `+GPS:Getting location` **or** JSON `{"type":"gps","success":false,"reason":"Getting location"}`. GPS not powered: `+GPS:Power off` **or** JSON `{"type":"gps","success":false,"reason":"Power off"}` | Requires firmware support `UE_GPS_ENABLE`; **only valid on modules that support GPS** |

---

## 9. Consolidated worked examples

| Goal | Commands |
|---|---|
| Enter AT command mode over serial | send `+++`, wait 500 ms, send `a`, wait for `+ok` |
| Leave AT command mode | `AT+EXIT` or reboot |
| Read firmware version | `AT+VER` |
| Read full device info | `AT+DEVINFO` |
| Set serial port 1 to 57600 8N1 | `AT+UART1=57600,8,1,NONE,NFC` then `AT+REBOOT` |
| Tune packing to 50 ms / 512 bytes | `AT+UARTTL1=50,512` then `AT+REBOOT` |
| Configure TCP client to server | `AT+SOCKEN1A=ON`, `AT+SOCK1A=TCPC,192.168.1.10,8080`, `AT+REBOOT` |
| Configure UDP client | `AT+SOCKEN1A=ON`, `AT+SOCK1A=UDPC,192.168.1.10,8080`, `AT+REBOOT` |
| Configure MQTT broker | `AT+SOCKEN1A=ON`, `AT+SOCK1A=MQTT,broker.example.com,1883`, `AT+MQCONF1=4,1,60`, `AT+MQAUTH1=dev001,user,pass`, `AT+MQSUB1=1,cmd/down,0`, `AT+MQPUB1=1,data/up,0,0`, `AT+REBOOT` |
| HTTP POST to an echo service | `AT+SOCKEN1A=ON`, `AT+SOCK1A=HTPC,www.rt-thread.com,80`, `AT+HTPREQ1=POST`, `AT+HTPURL1=/service/echo`, `AT+HTPHD1=Connection: close\|Accept: */*`, `AT+HTPTO1=6`, then send payload on the serial port |
| Enable serial heartbeat every 30 s | `AT+HEARTMD1=UART`, `AT+HEARTTM1=30`, `AT+REBOOT` |
| Send IMEI registration packet once on boot | `AT+REGTP1=IMEI`, `AT+REGMD1=FIRST`, `AT+REBOOT` |
| Enable offline caching | `AT+CACHE1=ON`, `AT+REBOOT` |
| Modbus polling every 60 s, one command | `AT+MBCFG1=1,1000,60`, `AT+MBCMD1=010100000002BDCB`, `AT+REBOOT` |
| Enable network AT with keyword `NET@` | `AT+EXAT=NET,NET@`, `AT+REBOOT`, then server sends `NET@AT+VER` |
| Disable the 24-hour no-data reboot | `AT+SOCKRTO=0` |
| HTTP firmware upgrade | `AT+DOWNLOAD=update.xxx.com:8080/firmware.bin` (reboots automatically on `OK`) |
| Restore factory defaults | `AT+CLRCFG` (or long-press **Reload** 5–10 s) |

---

## 10. Gotchas and constraints (important for integrators)

1. **Reboot is required** for almost every parameter change (`AT+REBOOT`). The tool's
   *Save* button alone is not enough; a device reboot also drops out of configuration state.
2. **Only one AT command per line, ≤ 256 bytes**, and **wait for the previous response**
   (up to 12 s timeout) before sending the next.
3. **Transparent mode is the default after reboot**; command mode never persists across
   a reboot.
4. **No Socket traffic while in AT configuration state** — network downlink data is
   silently discarded.
5. **TCP does not preserve packet boundaries** (coalescing). If your protocol needs frame
   boundaries, implement de-framing in the application layer, or tune `AT+UARTTLn`.
6. **UDP Client local port is random** — the device must send first so the peer learns the
   source port.
7. **HTTP shares address/port with the Socket parameters** — the corresponding Socket
   must be enabled before HTTP will work. `AT+HTPURLn` only applies to POST; in GET mode
   the serial data *is* the URL.
8. **Multiple values are `|`-separated**: HTTP headers, Modbus polling commands.
9. **`+ok` is mandatory** before you may issue AT commands after the `+++`/`a` handshake.
10. **Default settings to remember:** baud 115200 8N1, no flow control; packing 5 ms /
    1024 bytes; only Socket A of serial port 1 enabled; heartbeat OFF; registration OFF;
    offline cache ON; no-data reboot ON at 1440 min; EXAT `ALL` with keyword `NAT@`;
    MQTT 3.1.1 with clean session and no will message.
11. **Firmware-gated features** (the command will not work unless the firmware has the
    corresponding build flag): `AT+SIMSEL` (`UE_SIM_ENABLE`), HTTP commands
    (`UE_HTTPC_ENABLE`), `AT+GPS` (`UE_GPS_ENABLE`).
12. **SSL/TLS is not currently supported** for sockets.
13. **GPS is only present on some product variants.**
14. **Vendor supports remote (network) AT** — with `AT+EXAT` enabled a remote server can
    reconfigure the device, so protect the keyword / disable EXAT if that is a security
    concern.

---

## 11. Document provenance

- Source: vendor PDF manual for **YY-M200 (CAT1 模块)**, 42 pages, Chinese.
- Sections covered: 1.1 Overview, 1.2 Specifications, 1.3 Hardware interfaces,
  2.1 Configuration tool, 2.2 Serial port, 2.3 Data transmission, 2.4 Advanced functions,
  2.5 Exception handling, 3.1–3.3 AT commands (full command reference).
- Content that existed only as screenshots in the PDF (configuration-tool dialogs,
  block diagrams and the Modbus/edge-collection panel) has been transcribed where it
  carried information, and summarised otherwise.
- The §3.2 "AT command quick mastery" summary table referenced by the manual is missing
  from the source document; no content has been invented to fill it.
