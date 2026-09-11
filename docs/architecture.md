# Architecture (ESP-Matter style)

```
Application
  └─ binds drivers ──► en2m_model (clusters) ──► en2m_mesh (ESP-NOW) ──► coordinator
```

- **Interaction layer** (`en2m_model`): endpoints, clusters, attribute reports, commands  
- **Transport layer** (`en2m_mesh`): tree mesh over ESP-NOW  
- **Driver layer**: out of tree (`firmware/drivers` are references only)

Downstream JSON still includes flat aliases (`switch`, `temperature`, …) so the HA integration keeps working, plus structured `endpoints[].clusters.*`.
