# 并发与线程模型

这篇回答三个问题：**我的代码跑在哪个上下文？我能调什么？会不会死锁？**

## 1. 有哪些执行上下文

组件自己只创建**一个**任务。加上 ESP-IDF 本来就有的，一个设备固件里实际涉及这些上下文：

| 上下文 | 谁创建 | 在这里发生什么 |
|---|---|---|
| `main` 任务 | ESP-IDF | `app_main`：建硬件、建数据模型、`en2m_start`，然后返回 |
| **`en2m` 任务** | `en2m_mesh_init` | 帧处理、选父、转发、下行重传、命令派发、读刷新、上报序列化、NVS 刷盘、绝大多数应用回调 |
| Wi-Fi 任务 | `esp_wifi_start` | ESP-NOW 收包回调。**只做一次入队，别的什么都不干** |
| `esp_timer` 任务 | ESP-IDF | 你自己建的 `esp_timer` 回调（示例里的电机行程、PIR 模拟） |
| 事件循环任务 | `esp_event_loop_create_default` | `en2m_event_handler_register` 注册的处理函数 |
| 中断 | 硬件 | 你的 GPIO ISR |

`en2m` 任务的参数：

| | 默认 | 怎么改 |
|---|---|---|
| 名字 | `"en2m"` | 不可改 |
| 栈 | 4096 字节 | `en2m_device_config_t.task_stack_size` |
| 优先级 | 5 | `en2m_device_config_t.task_priority` |

栈要够大是因为**你的回调也跑在这个栈上**。如果回调里做 I2C 事务、
浮点格式化或者递归，把 `task_stack_size` 调到 6144–8192。
上报序列化本身用 cJSON，会走堆而不是栈。

## 2. 统一队列

`en2m` 任务的输入只有一个 `QueueHandle_t`，元素是带 tag 的 `en2m_item_t`：

| kind | 谁投递 | 任务收到后做什么 |
|---|---|---|
| `EN2M_ITEM_RX` | ESP-NOW 收包回调 | `en2m_dispatch_rx()` |
| `EN2M_ITEM_WORK` | `en2m_schedule` / `en2m_schedule_from_isr` | 直接调 `fn(arg)` |
| `EN2M_ITEM_ATTR` | `en2m_attribute_set_from_isr` | 调 `en2m_attribute_set()` |

**只用一个队列是为了保序**。如果按键中断走一条队列、远程命令走另一条，
两者的先后就没有定义了。同一个队列保证"先按的键先处理"这件事在库里是真的。

代价：每个槽位要放一个完整的 `en2m_pkt_t`，约 264 字节。所以
`EN2M_QUEUE_LEN` 默认只有 8（约 2.1 KB）。

### 队列满了会怎样

| 投递者 | 超时 | 满了的后果 |
|---|---|---|
| 收包回调 | 0（绝不阻塞 Wi-Fi 任务） | `rx_dropped++`，维护 tick 报 `EN2M_EVENT_RX_DROPPED` + WARN |
| `en2m_schedule` | 0 | 返回 `ESP_ERR_NO_MEM` |
| `en2m_schedule_from_isr` | ISR 版本 | 返回 `ESP_ERR_NO_MEM` |
| `en2m_attribute_set_from_isr` | ISR 版本 | 返回 `ESP_ERR_NO_MEM` |

**丢包不是静默的**。`en2m_schedule*` 的返回值该检查——中断风暴（比如没消抖的
按键）确实能填满队列。

## 3. 两把锁

组件里有两把互斥锁，各管一块，**从不互相嵌套**：

| 锁 | 位置 | 保护什么 |
|---|---|---|
| 数据模型锁 | `en2m_datamodel.c` 的静态互斥锁 | `s_endpoints[]` 整棵树：endpoint / cluster / attribute 槽位和回调指针 |
| mesh 锁 | `en2m_ctx_t.lock` | 邻居表、路由表、pending 表、父节点状态、序号 |

还有一个极小的自旋锁：

| 锁 | 保护什么 |
|---|---|
| `s_report_mux`（`portMUX_TYPE`） | 上报调度的两个 `int64_t` 时间戳 |

需要它是因为 `en2m_report_schedule()` 可以从**任意任务**被间接调到
（`en2m_attribute_set` → `on_change` → `report_schedule`），
而 32 位目标上 64 位读写不是原子的。临界区里只有几条指令。

### 锁顺序规则

**回调永远在锁外调用。** 这是整个组件最重要的一条内部纪律，因为回调几乎肯定
会调回 en2m 的 API，不这么做必然死锁。库里用两种手法保证它：

**手法一：出参 + 解锁后执行。** `en2m_handle_rx` 在持 mesh 锁的状态下分析帧，
但它不调应用代码，而是把"需要通知什么"写进 `en2m_rx_outcome_t`：

```c
en2m_lock();
en2m_handle_rx(src, &pkt, rssi, &outcome);   /* 只改内部状态 */
en2m_unlock();

if (outcome.parent_found)    { en2m_event_post(...); en2m_model_on_link_change(true); }
if (outcome.ack_resolved)    { en2m_event_post(...); }
if (outcome.uplink)          { config.on_uplink(...); }
if (outcome.deliver_command) { en2m_model_on_command_frame(...); }
```

**手法二：持锁取快照，解锁再调。** `en2m_dm_refresh` 和
`en2m_attribute_write` 都是先在锁内把需要的东西（路径、当前值、回调指针、ctx）
抄到栈上，解锁，然后才调回调：

```c
en2m_dm_lock();
/* ...抄下 path / value / cluster->read_cb / read_ctx... */
en2m_dm_unlock();

err = cb(&path, &value, ctx);      /* 锁外，回调可以随便调 en2m API */
if (err == ESP_OK) {
    en2m_attribute_set(path.endpoint_id, ...);   /* 会重新拿锁 */
}
```

这也是 `en2m_dm_refresh` 对每个属性都要重新查一遍槽位的原因——回调期间
别的任务可能改过这棵树。

## 4. 每个回调跑在哪个上下文

这张表要记准，它决定了你在回调里能干什么。

| 回调 | 运行上下文 |
|---|---|
| `attribute_write` | **调用 `en2m_attribute_write()` 的那个任务** |
| `attribute_read` | 总是 `en2m` 任务 |
| `attribute_changed` | **调用 `en2m_attribute_set/_write` 的那个任务** |
| `command` | 总是 `en2m` 任务 |
| `identify` | 总是 `en2m` 任务 |
| `en2m_event_*` 处理函数 | 总是事件循环任务 |
| `on_uplink`（协调器） | 总是 `en2m` 任务 |
| `on_command`（裸帧） | 总是 `en2m` 任务 |
| `on_log` | `en2m` 任务（也可能是调用 `en2m_set_pairing` 等的任务） |

`attribute_write` 和 `attribute_changed` 那两条要展开说，因为它们**取决于谁调的**：

| 触发来源 | `attribute_write` / `attribute_changed` 跑在 |
|---|---|
| 远程命令 | `en2m` 任务 |
| `en2m_schedule` 里的工作函数 | `en2m` 任务 |
| `en2m_attribute_set_from_isr` | `en2m` 任务 |
| `en2m_start` 的持久化回放 | **调用 `en2m_start` 的任务**（通常是 `main`） |
| 你自己在某个任务里调 `en2m_attribute_write` | **你那个任务** |
| 你自己在 `esp_timer` 回调里调 `en2m_attribute_set` | **`esp_timer` 任务** |

最后一条是个实际的坑：**`esp_timer` 回调里不要做耗时操作**。
如果在 `esp_timer` 回调里调 `en2m_attribute_set`，那么
`attribute_changed` 回调也会在 `esp_timer` 任务上跑，而那个任务是
ESP-IDF 的共享资源。想避开就用 `en2m_schedule()` 把活儿丢到 en2m 任务：

```c
static void pir_timer_cb(void *arg)          /* esp_timer 任务 */
{
    en2m_schedule(sample_pir, NULL);          /* 交给 en2m 任务 */
}
```

`examples/occupancy_sensor` 就是这么写的。

### 回调里能做什么

因为回调不在 ESP-NOW 收包回调、也不在中断里，跑在 en2m 任务上时它们可以：

- 阻塞（`vTaskDelay`、等信号量）
- 走 I2C / SPI / 一线时序
- 写 flash（NVS）
- 调任意 `en2m_*` API，包括 `en2m_report_now()`、`en2m_attribute_write()`
- 打日志

但别忘了**阻塞就是阻塞整个 en2m 任务**：收包、重传、上报都会跟着延后。
一个 200 ms 的 DHT22 时序是可以的；一个 5 秒的网络请求不行——那种活儿
自己建任务去做，做完用 `en2m_attribute_set` 把结果推进来。

## 5. 中断里的规则

中断上下文只能用三个函数：

```c
esp_err_t en2m_schedule_from_isr(en2m_work_fn_t fn, void *arg, BaseType_t *woken);
esp_err_t en2m_attribute_set_from_isr(uint8_t ep, uint16_t cluster, uint16_t attr,
                                      en2m_value_t value, BaseType_t *woken);
/* 以及 en2m_value_t 的那些 inline 构造器（en2m_bool / en2m_u8 / ...），
   它们只是结构体赋值，没有副作用 */
```

标准写法：

```c
static void IRAM_ATTR_OR_NOT on_gpio(void *arg)
{
    BaseType_t woken = pdFALSE;

    en2m_attribute_set_from_isr(1, EN2M_CLUSTER_BOOLEAN_STATE, EN2M_ATTR_STATE_VALUE,
                               en2m_bool(gpio_get_level(PIN) == 0), &woken);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}
```

### 关于 `IRAM_ATTR`

示例里的 ISR **故意没有**标 `IRAM_ATTR`。原因是：

1. `en2m_schedule_from_isr` / `en2m_attribute_set_from_isr` 本身不在 IRAM 里
2. 示例用 `gpio_install_isr_service(0)` 安装，flag 是 0，即**不要求 IRAM 安全**

标了 `IRAM_ATTR` 反而会给人"这个 ISR 在 flash cache 关闭时也能跑"的错觉，
而它其实不能。如果你的场景真的需要（比如用了 `ESP_INTR_FLAG_IRAM`），
那就不能在 ISR 里调 en2m 的函数——改成自己往一个 IRAM 安全的队列里塞，
再由一个普通任务转发。

### 千万不要在 ISR 里调

```c
en2m_attribute_set(...)     /* ✗ 会拿互斥锁 */
en2m_attribute_write(...)   /* ✗ 会拿互斥锁，还会调你的 write 回调 */
en2m_attribute_get(...)     /* ✗ 会拿互斥锁 */
en2m_report_now(...)        /* ✗ */
en2m_schedule(...)          /* ✗ 用 _from_isr 版本 */
ESP_LOGI(...)               /* ✗ */
```

## 6. 每个公开 API 的可调用上下文

| 函数 | 中断 | en2m 任务 | 其他任务 | 备注 |
|---|:-:|:-:|:-:|---|
| `en2m_endpoint_create` / `_get` | ✗ | ✗ | ✓ | 必须在 `en2m_start` 之前 |
| `en2m_endpoint_create_device` / `_add_device_type` | ✗ | ✗ | ✓ | 同上 |
| `en2m_cluster_create` / `_get` | ✗ | ✗ | ✓ | 同上 |
| `en2m_attribute_create` | ✗ | ✗ | ✓ | 同上 |
| `en2m_cluster_set_write_cb` / `_read_cb` / `_command_cb` | ✗ | ✓ | ✓ | 有锁，运行时改也安全 |
| `en2m_start` / `en2m_stop` | ✗ | ✗ | ✓ | 不要在 en2m 任务里调（`en2m_stop` 会等它退出） |
| `en2m_is_started` | ✗ | ✓ | ✓ | |
| `en2m_attribute_get` | ✗ | ✓ | ✓ | |
| `en2m_attribute_set` | ✗ | ✓ | ✓ | `attribute_changed` 在调用者任务上跑 |
| `en2m_attribute_set_from_isr` | ✓ | ✓ | ✓ | 提交推迟到 en2m 任务 |
| `en2m_attribute_write` | ✗ | ✓ | ✓ | write 回调在调用者任务上跑 |
| `en2m_report_*`（16 个包装） | ✗ | ✓ | ✓ | 等价于 `en2m_attribute_set` |
| `en2m_report_now` | ✗ | ✓ | ✓ | 非 en2m 任务调用时会降级成 `en2m_report_schedule(0)` |
| `en2m_report_schedule` | ✗ | ✓ | ✓ | |
| `en2m_schedule` | ✗ | ✓ | ✓ | 在 en2m 任务里调也可以，会排到队尾 |
| `en2m_schedule_from_isr` | ✓ | ✓ | ✓ | |
| `en2m_event_handler_register` / `_unregister` | ✗ | ✓ | ✓ | 可以在 `en2m_start` 之前调 |
| `en2m_send_uplink` / `en2m_send_downlink` | ✗ | ✓ | ✓ | 有锁 |
| `en2m_next_cmd_id` | ✗ | ✓ | ✓ | 有锁 |
| `en2m_set_pairing` | ✗ | ✓ | ✓ | 有锁 |
| `en2m_get_pairing` / `_has_parent` / `_get_path_cost` / `_get_role` | ✗ | ✓ | ✓ | 无锁读，单字节，天然安全 |
| `en2m_get_parent_mac` / `_get_self_mac` | ✗ | ✓ | ✓ | |
| `en2m_lookup_route` / `_forget_route` / `_clear_routes` | ✗ | ✓ | ✓ | **调用者需自己持 mesh 锁才严格安全**；实际只在协调器固件的 en2m 上下文里用 |
| `en2m_set_name` | ✗ | ✓ | ✓ | 有锁 |
| `en2m_mac_to_str` / `_from_str` 及 inline MAC 辅助 | ✓ | ✓ | ✓ | 纯函数 |
| `en2m_value_as_int` / `_equal` / `en2m_bool` 等构造器 | ✓ | ✓ | ✓ | 纯函数 |

## 7. 已知的并发注意点

**① `en2m_report_now()` 从别的任务调不是"立刻"。**
它检测到自己不在 en2m 任务上时会转成 `en2m_report_schedule(0)`，
于是要受 `min_report_interval_ms` 的下限约束，最快也要等到
`last_report_ms + min_report_interval_ms`。想真正立刻发，
就在 `en2m_schedule()` 的工作函数里调它。

**② `attribute_changed` 可能重入。**
在 `attribute_changed` 里再调 `en2m_attribute_set` 写另一个属性是允许的
（`thermostat` 示例的控温闭环就这么干），但要自己保证不会写回同一个属性形成无限递归。
去重机制（值没变就不通知）通常会自动切断循环，但别依赖它。

**③ `en2m_lookup_route` 系列没有内部加锁。**
它们是给协调器固件用的，而协调器只在 `on_uplink`（en2m 任务）里用。
在别的任务里遍历路由表时，`en2m_expire_routes` 可能正在改它。

**④ `en2m_stop()` 最多等 1 秒。**
它置 `running = false` 然后最多等 `20 × 50 ms`。如果你的回调正阻塞在
一个更长的操作上，`esp_now_deinit()` 可能在任务还没退出时就执行了。
不要在回调里阻塞超过一秒。

## 8. 一张图总结

```
   [isr] GPIO 中断
      │  en2m_schedule_from_isr / en2m_attribute_set_from_isr
      ▼
   ┌───────────────────────┐          [wifi] ESP-NOW 收包回调
   │   统一队列 (8 槽)     │◄─────────  只 xQueueSend(超时 0)
   │  RX / WORK / ATTR     │
   └───────┬───────────────┘
           │
   ┌───────▼────────────────────────────────────────────┐
   │  [en2m] 任务（栈 4096，优先级 5）                  │
   │                                                     │
   │  排空队列                                           │
   │    RX   → dispatch_rx → (持锁分析) → 解锁 → 回调   │
   │    WORK → fn(arg)                                   │
   │    ATTR → en2m_attribute_set                        │
   │                                                     │
   │  每 100 ms tick                                     │
   │    en2m_maintenance  选父超时/路由老化/重传/beacon  │
   │    en2m_model_tick   identify/NVS 刷盘/周期上报     │
   │                        └→ dm_refresh (read 回调)    │
   │                        └→ report_transmit           │
   └───────┬─────────────────────────────────────────────┘
           │  en2m_event_post
           ▼
   [evt] 事件循环任务 → 你的 en2m_event 处理函数
```

## 相关文档

- 回调的返回值语义和 fall-through → [callbacks.md](callbacks.md)
- 各个状态机的时间线 → [state-flow.md](state-flow.md)
- 队列深度、任务栈的调参 → [kconfig.md](kconfig.md)
