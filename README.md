# RL78/F13 Custom Transfer Reference Target

This CS+ project is the first target-side reference for the PC tool's
non-destructive `Custom Transfer` workflow. It verifies the complete
PC -> F24 host -> CAN -> MCU device request/response path without erasing or
writing flash.

## UART comparison trace

`CAN_TRANSFER_UART_TRACE_ENABLE` in `can_transfer_config.h` defaults to `1`.
The target prints one deterministic line when a CAN request is removed from the
RX FIFO and one line after its response is accepted by the CAN TX driver:

```text
TRACE CT RX id=0x710 dlc=8 data=[A5 01 01 02 12 34 56 78] seq=1 command=0x02 crc=OK
TRACE CT TX id=0x711 dlc=8 data=[5A 01 01 02 12 34 56 9A] seq=1 command=0x02 crc=OK
```

The final CRC bytes above are examples only. Each line contains the complete
eight-byte frame, not just the three command payload bytes.

Trace output is produced from `CAN_TransferProtocol_Process()` in the main
loop. CAN interrupt handlers do not call `printf` and do not wait for UART.
UART0 uses the board's P15/P16 debug connection at 115200 baud.

At startup, the target also reports the CAN initialization result, channel
status, bit rate, request/response IDs, exact receive filter, and transceiver
standby pin state. A working initialization starts with output similar to:

```text
CAN init: OK result=0x0000 channel_status=<raw status>
CAN config: Classical 500K request=0x710 response=0x711 filter=0x710/0x7FF STB=P12_LOW
hardware_init finish
```

`result=0x0000` is the initialization success condition. Preserve the actual
raw channel status in the test record; do not compare it to the placeholder.

The CAN error ISR only captures and clears hardware flags. The main loop emits
their decoded aggregate later, at most once per 250 ms while repeated flags are
coalesced, for example:

```text
TRACE CT CAN_ERROR flags=0x0403 decoded=[ BUS_ERROR ERROR_WARNING ACK_ERROR ]
```

If discovery times out, use these lines to separate initialization, bus, and
protocol failures:

- `CAN init: FAIL` means discovery cannot work; record the numeric result and
  channel status before changing filters or protocol settings.
- `CAN init: OK` with no `TRACE CT RX` means the target did not accept a valid
  `0x710` frame. Inspect CANH/CANL, common ground, transceiver standby, bus
  termination, and verify that both endpoints actually use Classical 500K.
- `TRACE CT CAN_ERROR` provides the controller's bus/bit/form/CRC/ACK state.
  It is printed from the main loop and is safe to capture through UART.
- `TRACE CT RX` without `TRACE CT TX` indicates target protocol or transmit
  processing. Both traces with no PC RX indicates the return path or host
  receive/filter configuration.

For a comparison capture:

1. Flash this project with CS+/E2 Lite, leave debug mode, and disconnect the
   programmer as required by the board setup.
2. Capture the F13 UART0 output to a text file.
3. In CANUpgradeTool, select `Custom Transfer`, enable
   `Detailed trace (slower)`, select `Start Log`, then run discovery and the
   transfer test.
4. Compare each PC `TRACE CAN TX id=0x710` line with the target
   `TRACE CT RX id=0x710` line, and each target `TRACE CT TX id=0x711` line
   with the PC `TRACE CAN RX id=0x711` line. The ID, DLC, all eight data bytes,
   sequence, command, and CRC result must match. Timestamps are local to each
   endpoint and are not comparison keys.

Set `CAN_TRANSFER_UART_TRACE_ENABLE` to `0` and rebuild for throughput, queue,
or timing stress measurements. Blocking text output at 115200 baud changes the
target's main-loop timing and therefore must not be present in a performance
measurement.

## Build

```powershell
& 'C:\Program Files (x86)\Renesas Electronics\CS+\CC\CubeSuite+.exe' `
  /br '.\RL78_F13_CAN_Transfer.mtpj'
```

Always inspect the final CS+ summary. A process exit code alone is not proof of
a successful CC-RL build.
