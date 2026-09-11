# `door_lock` — 门锁

**最短的执行器示例（45 行），专门用来讲持久化。**

门锁有一个别的设备没有的要求：**断电重启之后，锁必须回到断电前的状态。**
你不希望一次跳闸把家门解锁。这个示例展示这件事是**白拿的**——
一行恢复代码都不用写。

| | |
|---|---|
| Cluster | Door Lock (`0x0101`) |
| 设备类型 | `EN2M_DEVICE_TYPE_DOOR_LOCK` |
| 回调 | 只有 `attribute_write` |
| 驱动 | 无（示例里 `bolt_drive()` 是一行日志） |
| 默认名 / slug | `lock1` |
| HA 实体 | `lock.lock1` |

---

## 接线

**这个示例不接任何硬件。** `bolt_drive()` 只打一行日志，
因为锁体的驱动方式差别太大（电磁锁、电机锁、舌片锁），示例里不预设。
"换成真硬件"一节给了电磁锁和电机锁的写法。

---

## 编译烧写

```bash
. ~/esp/esp-idf-v5.5.5/export.sh
cd examples/door_lock
idf.py set-target esp32c3
idf.py build flash monitor -p /dev/ttyACM0
```

---

## 跑起来应该看到什么

```
espnow2mqtt/lock1/availability online
espnow2mqtt/lock1/state         {"lock":"UNLOCKED","caps":["lock"],"hop":1}
```

手动控制：

```bash
mosquitto_pub -t espnow2mqtt/lock1/set -m '{"lock":"LOCK"}'
mosquitto_pub -t espnow2mqtt/lock1/set -m '{"lock":"UNLOCK"}'
```

串口：

```
I (8104) ex_lock: bolt extended
```

**然后把设备断电、重新上电**：

```
I (598) ex_lock: bolt extended        ← 恢复，在第一条上报之前
I (615) ex_lock: ready
```

这一行就是这个示例的全部重点。

---

## 开机恢复的完整链路

```
1. drv 初始化（这个示例里没有，真硬件里是 gpio_config）
2. en2m_endpoint_create_device(1, DOOR_LOCK)
       └→ 建 DoorLock cluster，LOCK_STATE 属性标记为 persisted
3. en2m_start(&cfg)
       ├→ 从 NVS 读出所有 persisted 属性
       ├→ 对每一个都调一次 cfg.attribute_write    ← 你的 on_write() 在这里被调
       │      └→ bolt_drive(locked) 把锁体驱动到位
       ├→ 提交到数据模型
       └→ 入网，发第一条上报
```

三件事因此自动成立：

1. **锁体的物理状态和上报的状态一致**，因为两者都来自同一次 `on_write()`
2. **HA 看到的第一条状态就是真实状态**，不是一个默认值
3. **你没写任何恢复代码**

### 为什么驱动初始化必须在 `en2m_start()` 之前

第 3 步会调你的 `on_write()`，而 `on_write()` 要操作硬件。
GPIO 还没 `gpio_config` 的话这次操作会失败，
组件就会认为"恢复失败"，把属性留在默认值上。

**通用顺序**（这条规则对所有示例都适用）：

```c
void app_main(void)
{
    my_driver_init();                  // 1. 硬件先就绪
    en2m_endpoint_create_device(...);  // 2. 建数据模型
    en2m_start(&cfg);                  // 3. 启动（会触发恢复）
    my_event_source_start();           // 4. 事件源最后开
}
```

### 哪些属性是持久化的

不是全部。默认持久化的是**执行器的目标状态**：

| 持久化 | 不持久化 |
|---|---|
| OnOff 的开关状态 | 温度、湿度等测量值 |
| Level 的亮度、ColorControl 的色温 | 功率、电量 |
| DoorLock 的锁状态 | 门磁、人在 |
| Thermostat 的模式和设定点 | Thermostat 的实测温度 |
| Switch 的按键计数 | — |

判断标准很简单：**这个值是"别人告诉我的"还是"我测出来的"**。
前者要存（重启后没人会再告诉你一遍），后者不用（重启后再测一次就行）。

按键计数在这张表里是个特例，理由见
[`scene_switch`](../scene_switch/README.md#计数器为什么要持久化)。

改某个属性的持久化标记见 [docs/persistence.md](../../docs/persistence.md)。

---

## 锁状态是枚举，不是布尔

```c
typedef enum {
    EN2M_LOCK_UNLOCKED = 0,
    EN2M_LOCK_LOCKED = 1,
} en2m_lock_state_t;
```

所以 `on_write()` 里是：

```c
return bolt_drive(value->v.e8 == EN2M_LOCK_LOCKED);
```

注意用的是 `value->v.e8`（枚举），不是 `value->v.b`（布尔）。
**拿错会读到垃圾值。**

为什么不用布尔？因为真门锁有第三种状态：**`JAMMED`（卡住）**。
舌片伸出去但没到位、门没关严、有东西挡着——
这些情况下既不是"锁了"也不是"没锁"，用布尔表达不了。

这个示例的枚举里只有两个值，但类型留了扩展余地。
要加 `JAMMED` 的话在枚举里加一个值，序列化那边加一个分支
（现在的写法是 `v == EN2M_LOCK_LOCKED ? "LOCKED" : "UNLOCKED"`，
非锁定的一律当解锁），HA 的 `lock` 实体支持 `jammed` 状态。

---

## 换成真硬件

### 电磁锁（通电开 / 断电开）

**先搞清楚你的锁是哪一种**，这是安全问题：

| 类型 | 断电时 | 用在哪 |
|---|---|---|
| **通电开**（fail-secure） | 锁着 | 入户门、保险柜。断电时保持安全 |
| **断电开**（fail-safe） | 开着 | 消防通道。断电时保证能逃生 |

```c
static esp_err_t bolt_drive(bool locked)
{
    /* 通电开的锁：locked = 不给电 */
    return gpio_set_level(PIN_LOCK, locked ? 0 : 1);
}
```

**电磁锁的线圈电流很大（0.5–2 A）**，必须：

- 用 MOSFET 或者继电器驱动，**不能直连 GPIO**
- 线圈两端加**续流二极管**（1N4007 就行），不然断电瞬间的反电动势会打坏 MOSFET
- 锁的电源和 C3 的电源**分开**，或者至少加大电容。
  不然每次开锁 C3 都会重启（见 [docs/wiring.md](../../docs/wiring.md#供电)）

### 电机锁 / 智能锁锁体

这类锁体开关一次要一两秒，所以**不该用写回调**——
应该像 [`window_cover`](../window_cover) 那样用命令回调，
让锁体走完再上报。

折中的做法：`on_write()` 里只启动电机然后立刻返回 `ESP_OK`，
再用一个 `esp_timer` 在电机走完之后 `en2m_report_lock_state()` 报真实状态。
第一次上报可能"抢跑"，但很快会被纠正。

### 加一个到位检测

真产品应该有反馈，不然你不知道锁到底动了没：

```c
/* 舌片到位的微动开关 */
static void on_bolt_sensor_edge(void *arg)
{
    BaseType_t woken = pdFALSE;
    en2m_schedule_from_isr(publish_bolt_state, NULL, &woken);
    if (woken) { portYIELD_FROM_ISR(); }
}

static void publish_bolt_state(void *arg)
{
    bool in_place = gpio_get_level(PIN_BOLT_SENSE) == 0;
    en2m_report_lock_state(ENDPOINT, in_place ? EN2M_LOCK_LOCKED : EN2M_LOCK_UNLOCKED);
}
```

注意这里用 `en2m_report_lock_state()`（传感器路径，直接上报）
而不是 `en2m_attribute_write()`（执行器路径，会再驱动一次锁体）。
**混了会造成无限循环**：上报 → 触发写 → 驱动锁体 → 到位开关动 → 上报……

---

## 安全提醒

这套系统**不适合单独用来控制入户门锁**。理由：

- ESP-NOW 的加密是共享密钥的，一个设备被拆解就等于整网的密钥泄露
- 丢包意味着"锁门"的命令可能没送到，而你在 HA 里看到的是已发送
- 没有防重放（replay）保护

**合理用法**是：

- 作为已有机械锁的**辅助**（机械钥匙仍然有效）
- 只做"解锁需要本地确认"的单向逻辑（HA 只能加锁，不能远程解锁）
- 用在柜门、库房门这种失效代价低的地方

协议的安全边界写在
[docs/mesh.md](../../docs/mesh.md) 里，接强电和门锁之前值得读一遍。

---

## 常见坑

| 现象 | 原因 |
|---|---|
| 重启之后锁状态回到解锁 | NVS 分区表里没有 `nvs` 分区，或者被 `idf.py erase-flash` 清掉了 |
| `on_write()` 里读出来的值是乱的 | 用了 `value->v.b` 而不是 `value->v.e8` |
| 开锁时 C3 重启 | 线圈电流拉垮电源，见上文供电 |
| 开锁之后 MOSFET 发烫 / 烧了 | 没加续流二极管 |
| 锁一直在响 | 到位检测用了 `en2m_attribute_write()` 造成循环，改用 `en2m_report_lock_state()` |
| HA 里点了没反应 | 检查 Bridge 日志有没有收到 `lock1/set`；再检查 `permit_join` 配过没 |

---

## 延伸阅读

- [docs/examples.md#door_lock](../../docs/examples.md#door_lock) — 逐行精讲
- [docs/persistence.md](../../docs/persistence.md) — 哪些属性持久化、存在哪、怎么改
- [docs/callbacks.md](../../docs/callbacks.md) — `write` 路径和 `report` 路径的区别
