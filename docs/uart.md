# UART and diagnostic tools

The BT3L and FT61E0A5 communicate using 3.3 V UART at 9600 baud, 8N1. The pin numbers are in the [hardware guide](hardware.md).

## Browser tools

Download the HTML files and open them in a Chromium browser with the relevant API enabled. A local secure context such as `http://localhost` can be used when the API is unavailable for local files.

- [UART monitor](../tools/uart-monitor.html) uses Web Serial to record both directions as a timestamped CSV. Connect only RX and GND on each adapter for passive capture.
- [UART valve test](../tools/uart-valve-test.html) sends status, start for ten seconds, or stop. Disconnect the water supply. Verify that the BT3L TX output is high-impedance or physically isolated from the MCU input before connecting the adapter TX. Two active outputs must never drive the same line.
- [Original BLE control](../tools/original-ble.html) uses Web Bluetooth with the original firmware for local pairing, status, start, and stop. The first-pairing path needs access to BLE advertisement data. Credential exports stay on your machine and should be kept private.

## Frame format

Header `55 AA`, version byte, command byte, two-byte big-endian payload length, payload, then the sum of all preceding bytes modulo 256. The version byte in the examples is `00`.

| Command | Meaning |
| --- | --- |
| `06` | Write data points to the valve controller |
| `07` | Report data points or acknowledge a report |
| `08` | Query data points |
| `09` with payload `00` | Long button press and pairing request |
| `E2` with payload `06` | Set the original BLE advertising interval to 600 ms |
| `E5` with payload `01` | Enable low-power operation |

| Data point | Type | Meaning |
| --- | --- | --- |
| 1 (`01`) | Boolean | Valve state |
| 7 (`07`) | 32-bit big-endian value | Battery level in percent |
| 11 (`0B`) | 32-bit big-endian value | Watering duration in seconds |

Start a ten-second run (BT3L → FT61E0A5):

```text
55 AA 00 06 00 0D 01 01 00 01 01 0B 02 00 04 00 00 00 0A 31
```

Stop a run early (BT3L → FT61E0A5):

```text
55 AA 00 06 00 05 01 01 00 01 00 0D
```

The valve controller closes the valve at the end of the timed run and reports the new state. The 32-bit field describes the wire encoding. The controller's maximum accepted duration is still to be measured.
