[简体中文](BSP.zh_CN.md)

# Bobo M501 V1.5 / 博博

ESP32-S3, 16 MB Flash and 8 MB octal PSRAM. This is M501 V1.5, not
MagicBox C5 M301. PMU owns I2C1; all other I2C devices reuse I2C0. Native PCB
UART names are crossed: actual ESP DTE TX=GPIO5, RX=GPIO4. Keep console on USB.
The standalone demo copies NFC events to a queue and emits a local tone;
touch edges are logged. It never starts PPP or cloud services.
The BSP copies UID/NDEF into bounded fields and reports oversized records.
The callback receives temporary data; copy it before returning. One placement
emits one event; three absent polls rearm the same card. UID is not authentication.
NFC and touch stop must complete before restart. A timeout means a worker may
still be running; wait/retry stop, never start a second owner. Do not call stop
from its own callback. Haptic calls can block for their duration (maximum 1s).
For 4G, pass APN/SIM slot explicitly, call start/wait/stop from one owner. Raw
UART and PPP are mutually exclusive. Reconnect policy stays in the application.

| API | Hardware / ownership |
| --- | --- |
| bsp_i2c | I2C0 SDA41/SCL42; I2C1 SDA7/SCL16 dedicated to PMU |
| bsp_power | AXP2101 0x34 on I2C1, IRQ2 |
| bsp_audio | ES8311 0x18, I2S0 MCLK11/BCLK10/WS46/TX3/RX9; no PA GPIO |
| bsp_button | Active-low voice key GPIO21 |
| bsp_nfc | WS1850T 0x28, RESET18/IRQ17; UID and first NDEF record |
| bsp_touch | Front12/back13 active-low digital touch edges |
| bsp_led | WS2812 data6 |
| bsp_haptic | AW862XX 0x58, RESET47/IRQ48 |
| bsp_wakeup | GX8002 0x2F, IRQ14 |
| bsp_modem / bsp_4g | UART1 TX5/RX4, raw transport or opt-in PPP |

## Initialization, ownership and failures

Initialize I2C before PMU. GX8002 rails: core/IO off, IO=3.3V and core=1.0V, then IO → 10ms → core; disable in reverse order. Audio: init → open → read/write; volume is 0–100%; close before PMU sleep.

One BSP owns each bus; one application owner serializes lifecycle calls. Callbacks cannot perform networking/audio/storage; copy events into an application queue. Audio I/O can block. Propagate initialization errors; missing hardware is not success. The baseline starts no Wi-Fi, Bluetooth, cloud service or product state machine.

## Validation and device acceptance

`./tools/build.sh` compiles baseline and demo. Compiler/host results do not prove device behavior. Device acceptance must record physical revision/chip, Flash/PSRAM, pins, bus addresses, rails, nonzero microphone capture/audible playback, keys/touch/NFC where fitted, RGB and RTC/wake events. Motor, haptic, shutdown, sleep, 4G and wake-chip firmware upgrade are intentional opt-in tests.

Report `Build`, `Host tests`, `Device tests` and `Unverified` separately. Installing a development partition layout can overwrite production identity/data; confirm the exact board/image/data impact and obtain authorization before writing.
