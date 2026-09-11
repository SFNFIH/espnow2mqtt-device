# Architecture (ESP-Matter style)

```
Application
  └─ binds drivers ──► en2m_model (clusters) ──► en2m_mesh (ESP-NOW) ──► coordinator
```

- **Interaction layer** (`en2m_model`): endpoints, clusters, attribute reports, commands  
- **Transport layer** (`en2m_mesh`): tree mesh over ESP-NOW  
- **Driver layer**: out of tree (`firmware/drivers` are references only)

Downstream JSON uses **flat HA aliases** (`switch`, `temperature`, `brightness`, …)
plus `caps` and a short `clusters` name list. Nested endpoint dumps are omitted on-air
to stay within `EN2M_DATA_MAX` (160 bytes).
