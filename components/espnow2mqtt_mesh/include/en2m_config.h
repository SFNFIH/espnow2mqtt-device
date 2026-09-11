#pragma once

/**
 * Role is chosen in firmware at init time (see en2m_app_config_t.role).
 * Each firmware project hard-codes one role in app_main — that is the
 * "select at development time" switch.
 */

#define EN2M_ROLE_COORDINATOR 1
#define EN2M_ROLE_ROUTER      2
#define EN2M_ROLE_LEAF        3

#ifndef EN2M_WIFI_CHANNEL
#define EN2M_WIFI_CHANNEL 1
#endif

#ifndef EN2M_HOP_LIMIT
#define EN2M_HOP_LIMIT 8
#endif

#ifndef EN2M_MAX_ROUTES
#define EN2M_MAX_ROUTES 32
#endif

#ifndef EN2M_MAX_NEIGHBORS
#define EN2M_MAX_NEIGHBORS 16
#endif

#ifndef EN2M_BEACON_MS_DEFAULT
#define EN2M_BEACON_MS_DEFAULT 5000
#endif

#ifndef EN2M_PARENT_STALE_MS
#define EN2M_PARENT_STALE_MS 20000
#endif

#ifndef EN2M_ROUTE_STALE_MS
#define EN2M_ROUTE_STALE_MS 120000
#endif

#ifndef EN2M_OFFLINE_MS
#define EN2M_OFFLINE_MS 90000
#endif

#ifndef EN2M_DEVICE_MODEL
#define EN2M_DEVICE_MODEL "c3-node"
#endif

#ifndef EN2M_DEVICE_NAME
#define EN2M_DEVICE_NAME "node1"
#endif

#ifndef EN2M_FW_VERSION
#define EN2M_FW_VERSION "0.3.0-idf"
#endif
