# Architecture (ESP-Matter style)

```
       application + hardware drivers
              ↑                    ↓
        callbacks         en2m_attribute_set / _write
              │                    │
       en2m_model   endpoints, clusters, attributes, commands, reporting
              │
       en2m_mesh    ESP-NOW tree, routing, retries
              │
         coordinator (espnow2mqtt-host) → MQTT → Home Assistant
```

- **Interaction layer** (`en2m_model`, `en2m_attr`): the attribute store, the
  command decoder, the reporting policy and NVS persistence.
- **Transport layer** (`en2m_mesh`): the tree mesh, route learning, forwarding
  and acknowledged downlinks.
- **Driver layer**: out of tree. `drivers/` at the repository root are
  references only; the component never touches a peripheral itself.

## Thread model

The component creates exactly one task, named `en2m`.

| Runs on | What |
|---|---|
| Wi-Fi task (ESP-NOW receive callback) | copies the frame into the en2m queue and returns. Nothing else. |
| **en2m task** | frame handling, parent selection, forwarding, downlink retries, command dispatch, `attribute_read` / `attribute_write` / `command` / `identify` callbacks, report serialization, NVS write-back |
| Default event loop task | `en2m_event_*` handlers |
| Caller's task | `en2m_attribute_set` / `_get` / `_write` are mutex protected and may be called from anywhere; `attribute_changed` runs on the calling task |

One queue carries received frames, ISR-deferred work (`en2m_schedule_from_isr`)
and ISR attribute updates (`en2m_attribute_set_from_isr`), which keeps their
relative order and means an application needs no task of its own. Maintenance
runs on a 100 ms tick inside the same task, so there is no separate timer
thread either.

Because callbacks run on the en2m task and not in the ESP-NOW receive
callback, they may block, use I2C, write to flash or send frames.

## Reporting

Downstream JSON uses **flat Home Assistant aliases** (`switch`, `temperature`,
`brightness`, …) plus a `caps` list, built from the attribute store rather
than from driver getters.

An ESP-NOW frame carries at most `EN2M_DATA_MAX` (160) payload bytes. The
component builds the report at decreasing detail — full, then without
diagnostics, then without `caps` — and sends the first version that fits, so
it never puts a truncated object on the air. The host merges successive
reports for a device, so dropping optional fields loses nothing.

Flat keys are global, so the lowest endpoint that owns a cluster wins. A
multi-endpoint node should therefore expose distinct clusters per endpoint.

## Reliability

A downlink with a non-zero transaction id is retried until the device
acknowledges it; the device side acknowledges every such frame automatically
and then reports the resulting state. `EN2M_EVENT_ACK_RECEIVED` and
`EN2M_EVENT_ACK_TIMEOUT` make the outcome observable, and the coordinator
forwards a timeout to the host as `{"ok": false, "error": "timeout"}`.

## Persistence

Attributes created with `persist = true` — which is the default for actuator
state such as OnOff, level, colour temperature, lock state, cover position,
fan mode and thermostat setpoints — are written back to NVS at most every five
seconds and restored during `en2m_start`. The restored values are pushed
through the write callback, so the hardware matches the reported state before
the first report leaves the node.
