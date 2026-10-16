[English](README.md)

# PPP transport

建议使用 `bsp_4g_start(apn, sim_slot)` / `bsp_4g_wait_connected(timeout)` / `bsp_4g_stop()` 统一管理。APN 从应用传入。UART1 的实际 DTE TX5/RX4 由 BSP 引脚表提供。与原始 UART 互斥，禁止两个所有者。控制台使用 USB，初始化不代表获得 IP。
