# RL78/F13 Custom Transfer Reference Target

This CS+ project is the first target-side reference for the PC tool's
non-destructive `Custom Transfer` workflow. It verifies the complete
PC -> F24 host -> CAN -> MCU device request/response path without erasing or
writing flash.

## Public examples

- [This CAN Transfer benchmark](https://github.com/released/RL78_F13_CAN_Transfer).
- [CAN Boot project](https://github.com/released/RL78_F13_Boot_loader_CAN).
- [Matching CAN App project](https://github.com/released/RL78_F13_Boot_loader_CAN_app).
- [Tool and bridge operating guide](https://released.github.io/slide_simple_renesas_isp_tool/).
- [Boot/App porting guide](https://released.github.io/slide_boot_loader_for_RL78/#boot-app-porting).

Transfer is the standalone non-destructive transport test, not the Boot/App
update image. For firmware updates, obtain both Boot and App as sibling
directories using their repository names; see the linked porting guide.

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
3. In FirmwareUpgradeTool, select `Custom Transfer`, enable
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

## User code entry

`custom_func.c` provides `hardware_init()` for initialization and `loop()` for
foreground service. Add application initialization and foreground work through
these callbacks, retaining the existing communication service calls.
The selected main/startup adapter owns entry and handoff. Generated peripheral
files remain under `src/`; retain the project's MTPJ source selection when using
Code Generator or Smart Configurator, then rebuild to validate the image and vectors.

## Project layout and standalone build

This project includes its own source files, configuration headers and build tools.
A sibling Boot, App or Transfer checkout is not required to compile it.
Keep the project directory name aligned with its `.mtpj` filename; remove a ZIP download suffix when necessary.
Build with CS+ Rebuild, or run the local PowerShell entry:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
```

| Location | Responsibility |
| --- | --- |
| `custom_func.c` | Project initialization in `hardware_init()` and recurring work in `loop()`; runtime orchestration stays here |
| `hal/can` | CAN service, controller driver and CAN adapter, when used by this project |
| `hal/device` | Device, clock, peripheral and CPU handoff APIs |
| `hal/image` | Metadata parser/profile, image validation and software CRC |
| `hal/flash` | Flash access/geometry APIs, when required by the selected image |
| `hal/isp` | Update/Transfer protocol and transport processing APIs |
| Root configuration headers | Local MCU/Flash, handoff and communication contract |
| `tools` and root PowerShell helpers | Local metadata/IRQ generation and linked-image checks |
| `src` | Code Generator or Smart Configurator peripheral sources |

Only the sources registered in this project's CS+ configuration are linked.
Generated main/vector implementations must not become a second active entry.
Retain the `custom_func.c` callbacks and the selected startup adapter after code generation.

Boot and App each carry a local contract. Keep their MCU, Flash layout, image format,
IRQ ABI, handoff and communication settings compatible when installing a pair.
App releases are edited in `app_image_config.h` and `app_irq_handlers.h` in the App project.
Rebuild the selected App to regenerate its metadata, vector table, update BIN and timing BINs.
The App build does not read or rewrite a Boot checkout.
