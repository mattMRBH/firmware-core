# Serial Command Service

`SerialCommandService` provides the manufacturing-only `#AG` command protocol
over the native USB Serial/JTAG connection. It owns USB input, line parsing,
request admission, and response formatting; the orchestrator owns the typed
operations against Go settings and factory reset.

## Files

| File | Purpose |
|---|---|
| [`serial_command.h`](../main/serial_command/serial_command.h) | Queue-copyable request/result types, transport interface, and service declaration |
| [`serial_command.cpp`](../main/serial_command/serial_command.cpp) | Parser, command task, one-in-flight state, and event/result bridge |
| [`serial_command_usb.cpp`](../main/serial_command/serial_command_usb.cpp) | USB Serial/JTAG driver, VFS routing, RX, and atomic VFS response writes |
| [`go_orchestrator.cpp`](../main/go_orchestrator.cpp) | Settings, board serial, and factory-reset command completion |
| [`ago_serial_command.py`](../../../scripts/ago_serial_command.py) | Host CLI that sends one command and filters interleaved USB logs |

## Dependencies

| Dependency | Source | Usage |
|---|---|---|
| `RTOS` | `airgradient-common` (`rtos.h`) | Command task and fixed-size event/result queues |
| USB Serial/JTAG | ESP-IDF (`esp_driver_usb_serial_jtag`) | Native USB RX and the secondary-console VFS output path |
| `Orchestrator` | product (`go_orchestrator.cpp`) | Applies typed correction requests and factory reset |
| `GoSettings` | product (`go_settings.h`) | Existing validation, persistence, and correction activation path |

## Public API

| Method | Returns | Purpose |
|---|---|---|
| `SerialCommandService(event_queue, channel)` | — | Binds the central event queue and serial transport. |
| `start()` | `bool` | Initializes the transport, creates the one-item result queue, and starts the command task. |
| `complete(result)` | `void` | Delivers the orchestrator result for the accepted command. |

See [`serial_command.h`](../main/serial_command/serial_command.h) for full
signatures and protocol payload types.

## Behavior

### Lifecycle

The service is constructed during normal Go composition but remains inactive.
The orchestrator calls `start()` only when the boot-button manufacturing path
enters manufacturing mode. The mode and service remain active until reboot or
power-off, including after `FACTORY_RESET`; factory reset returns the device to
Portable/Home without rebooting. Because serial commands run only in
manufacturing mode, `FACTORY_RESET` retains active measurement corrections
while clearing all other reset state.

```mermaid
stateDiagram-v2
    [*] --> Inactive
    Inactive --> Active: manufacturing mode entry
    Active --> Active: FACTORY_RESET completes
    Active --> Inactive: reboot or power off
```

On first activation, the USB channel installs the USB Serial/JTAG driver with
256-byte RX/TX rings, routes the existing VFS through that driver, and retains a
write-only `/dev/secondary` descriptor. Each response is emitted by one VFS
`write()` call, so normal mirrored logs cannot split its bytes. The channel is
not installed at normal boot, never uses UART0, and is not uninstalled.

The task uses a 3072-byte stack at priority 3 and waits up to 50 ms per USB RX
read. This finite wait lets it poll the one-item result queue. A command is
marked in flight only after central-event admission succeeds; a second valid
command receives `#AG ERROR BUSY` until the prior result is emitted.

### Protocol

Messages are UTF-8 ASCII tokens terminated by LF. CRLF is accepted. Commands
and responses begin with `#AG` followed by one ASCII space; non-prefixed input is ignored. The receiver
buffers at most 128 bytes per line and discards an overlong line through its
next LF. Responses are bounded to 128 bytes.

| Request | Successful Response | Other Error |
|---|---|---|
| `#AG HELP` | `#AG OK COMMANDS HELP GET_SERIAL SET_SLR <PM\|TEMP\|HUM> <scale> <intercept> GET_SLR <PM\|TEMP\|HUM> FACTORY_RESET` | `INVALID_ARGUMENT` |
| `#AG GET_SERIAL` | `#AG OK SERIAL <serial>` | `INVALID_ARGUMENT` |
| `#AG SET_SLR <target> <scale> <intercept>` | `#AG OK SLR <target> <scale> <intercept>` | `INVALID_ARGUMENT`, `OPERATION_FAILED` |
| `#AG GET_SLR <target>` | `#AG OK SLR <target> <scale> <intercept>` | `INVALID_ARGUMENT`, `SLR_NOT_SET` |
| `#AG FACTORY_RESET` | `#AG OK RESET` | `INVALID_ARGUMENT`, `OPERATION_FAILED` |

`target` is exactly `PM`, `TEMP`, or `HUM`. Numeric values must fully parse to
finite `float` values. SLR responses always render scale and intercept with six
decimal places. The board serial comes unchanged from the existing Go board
serial source.

### Settings Operations

The parser carries `Pm25Correction` or `LinearCorrection` in the typed request.
The orchestrator copies its complete settings, selects the requested custom
algorithm, validates the merged candidate, and uses
`activate_settings_candidate()` for persistence and runtime activation. No
serial-specific preferences or direct NVS writes exist.

For PM, `SET_SLR` selects `CustomViaPm25Raw` and preserves `use_epa2021` when
the current PM correction is already custom; otherwise it initializes that flag
to `false`. Temperature and humidity select the linear `Custom` algorithm.

## Edge Cases / Errors

The only protocol errors are `EMPTY_COMMAND`, `INVALID_COMMAND`,
`INVALID_ARGUMENT`, `SLR_NOT_SET`, `OPERATION_FAILED`, and `BUSY`. Extra
arguments, unknown targets, invalid numbers, and arguments supplied to
argument-free commands are `INVALID_ARGUMENT`. A valid request that cannot be
queued, persisted, or completed is `OPERATION_FAILED`. A failed or short VFS
write is not retried because retrying could interleave with a log message.
