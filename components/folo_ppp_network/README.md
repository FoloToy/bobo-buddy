[简体中文](README.zh_CN.md)

# PPP transport

Prefer `bsp_4g_start(apn, sim_slot)` / `bsp_4g_wait_connected(timeout)` / `bsp_4g_stop()` for lifecycle ownership. APN is caller supplied. UART1 actual DTE TX5/RX4 comes from the BSP pin map. Raw UART and PPP are mutually exclusive. Console stays on USB. Successful initialization does not mean an IP connection.
