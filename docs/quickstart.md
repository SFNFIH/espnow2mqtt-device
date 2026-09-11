# 快速开始

目标：**半小时内在 Home Assistant 里点亮一个 C3 继电器。**

这套系统横跨三个仓库，缺一个都不行。本篇按顺序走完，每一步都有"怎么确认成功"。

```
[ C3 设备 ]  --ESP-NOW-->  [ S3 协调器 ]  --USB-->  [ Bridge ]  --MQTT-->  [ HA 集成 ]
     ↑                          ↑                      ↑                     ↑
  本仓库                  espnow2mqtt-host        espnow2mqtt-host      espnow2mqtt-ha
```

---

## 0. 你需要的东西

| | 说明 |
|---|---|
| 一块 **ESP32-S3** | 做协调器，插在 HA 主机的 USB 上。**必须是原生 USB / Serial-JTAG** 的板子 |
| 一块 **ESP32-C3** | 做设备。任何 C3 开发板都行 |
| 一个 **MQTT broker** | HA 里的 Mosquitto add-on 最省事 |
| **ESP-IDF 5.x** | 本项目在 **v5.5.5** 上验证过 |
| 一个继电器模块（可选） | 没有的话看日志也能确认通路 |

装 ESP-IDF：

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5.5 --recursive https://github.com/espressif/esp-idf.git esp-idf-v5.5.5
cd esp-idf-v5.5.5 && ./install.sh esp32c3,esp32s3
```

之后每开一个新终端都要：

```bash
. ~/esp/esp-idf-v5.5.5/export.sh
```

> Debian/Ubuntu 上 `install.sh` 可能报 `ensurepip is not available`，
> 先 `sudo apt-get update && sudo apt-get install -y python3-venv python3-pip`。

---

## 1. 决定信道（现在就定，别拖）

**全网必须同一个信道**，ESP-NOW 不跨信道，不一致时完全静默——不报错、不打日志、
设备永远找不到父节点。这是最常见的"死活不上线"原因。

选一个和家里 Wi-Fi AP 错开的：AP 在 1 就选 6，AP 在 6 就选 1 或 11。

记住这个数字，第 2 步和第 3 步都要用。理由和选择策略见
[kconfig.md](kconfig.md#2-信道)。

---

## 2. 烧 S3 协调器

在 **[espnow2mqtt-host](https://github.com/SFNFIH/espnow2mqtt-host)** 仓库：

```bash
git clone https://github.com/SFNFIH/espnow2mqtt-host.git
cd espnow2mqtt-host/firmware/coordinator

echo 'CONFIG_EN2M_WIFI_CHANNEL=6' >> sdkconfig.defaults   # ← 第 1 步选的信道

idf.py set-target esp32s3
idf.py build flash monitor
```

**确认成功**：串口上出现

```
I (xxx) en2m: mesh init
```

然后 S3 就安静地待着了（它是树根，不需要找父节点）。
`Ctrl+]` 退出 monitor，让串口空出来给 Bridge 用。

主机上应该能看到串口设备：

```bash
ls -l /dev/serial/by-id/         # 推荐用 by-id，重插不会变
```

---

## 3. 跑 Bridge

还在 host 仓库：

```bash
cd ../..                 # 回到仓库根
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt

python -m espnow2mqtt \
  --port /dev/ttyACM0 \
  --mqtt-host homeassistant.local \
  --mqtt-port 1883 \
  -v
```

**确认成功**：

```bash
mosquitto_sub -t 'espnow2mqtt/bridge/state' -v
# espnow2mqtt/bridge/state online
```

看到 `online` 就说明 USB 和 MQTT 两头都通了。
`offline` 或者没消息 → Bridge 没连上 broker，或者串口被 monitor 占着。

Docker / HA Add-on 的跑法见 host 仓库的 README。

---

## 4. 装 HA 集成

装 **[espnow2mqtt-ha](https://github.com/SFNFIH/espnow2mqtt-ha)**（HACS 或手动拷到
`config/custom_components/`），重启 HA，然后在"设备与服务"里添加集成。

`base_topic` 必须和 Bridge 的 `--base-topic` 一致（两边默认都是 `espnow2mqtt`）。

---

## 5. 烧 C3 设备

回到**本仓库**：

```bash
git clone https://github.com/SFNFIH/espnow2mqtt-device.git
cd espnow2mqtt-device/examples/relay_switch

echo 'CONFIG_EN2M_WIFI_CHANNEL=6' >> sdkconfig.defaults   # ← 和第 2 步一样！

idf.py set-target esp32c3
idf.py build flash monitor
```

**确认成功**：串口上 10 秒内出现

```
I (612)  en2m:      mesh init
I (615)  ex_switch: ready — the component owns the task from here
I (1843) en2m:      parent=aa:bb:cc:dd:ee:ff cost=1 rssi=-42
```

**`parent=` 这一行是关键**，它说明入网成功了。
一直不出现 → 99% 是信道不一致，回第 1 步核对；其余可能见
[troubleshooting.md 第 1 节](troubleshooting.md#1-设备完全不上线)。

---

## 6. 开配网窗口

设备第一次上线需要协调器"认识"它一次。不开窗口的话协调器会丢掉陌生设备的上行，
日志里是 `drop uplink (not pairing / unknown)`。

```bash
mosquitto_pub -t espnow2mqtt/bridge/request/permit_join -m 60
```

（HA 集成里通常也有一个 `permit_join` 的按钮/服务。）

60 秒内复位一下 C3，让它重新上行。

---

## 7. 在 HA 里确认

几秒之内应该出现一个 **switch 实体**。点一下：

- C3 串口上打出 `relay is now on`
- 有继电器的话能听到吸合声
- HA 里的开关保持在 on（不弹回）

反过来按 C3 板上的 BOOT 键（GPIO9），HA 里的开关也会跟着变——
因为本地按键走的是和远程下发**完全同一条路径**。

再试一次断电重启：**继电器会自己回到断电前的状态**，因为 OnOff 属性默认持久化。

---

## 出问题了

按症状查表：

| 症状 | 去哪 |
|---|---|
| C3 没有 `parent=` | [troubleshooting 1](troubleshooting.md#1-设备完全不上线)，先查信道 |
| 反复 `parent stale` | [troubleshooting 2](troubleshooting.md#2-设备上线又掉线) |
| HA 里没有实体 | [troubleshooting 3](troubleshooting.md#3-ha-里没有实体) |
| 有实体但状态不动 | [troubleshooting 4](troubleshooting.md#4-ha-里有实体但状态不更新) |
| 点了没反应 / 状态弹回 | [troubleshooting 5](troubleshooting.md#5-控制不生效--状态弹回) |
| `drop uplink (not pairing / unknown)` | 回第 6 步开配网 |
| 编译不过 | [troubleshooting 9](troubleshooting.md#9-编译和链接错误) |

三十秒定位法和全部日志的含义在
[troubleshooting.md](troubleshooting.md#0-三十秒定位)。

---

## 下一步

| 想做什么 | 看这篇 |
|---|---|
| 换一个别的示例（灯、传感器、窗帘…） | [examples.md](examples.md) — 10 个示例逐个详解 |
| **写自己的设备** | [usage.md](usage.md) — 五步法 + 四种配方 |
| 覆盖范围不够，加个中继 | [examples.md](examples.md#firmwarerouter) 里的 `firmware/router` |
| 调上报频率、省电 | [reporting.md](reporting.md) |
| 省 RAM、调超时 | [kconfig.md](kconfig.md) |
| 搞懂内部怎么跑的 | [architecture.md](architecture.md) → [state-flow.md](state-flow.md) |

接线和引脚见 [wiring.md](wiring.md)。
