#!/usr/bin/env python3
"""Push a firmware image to an AirGradient device over BLE (OTA happy path).

Drives the OtaBleService GATT flow from a host: connect + pair, write the
``start`` control command, wait for the device's ready NOTIFY, stream the image
to the Data characteristic without response (Write-Without-Response; pacing is
left to the link / controller buffers), write the ``end`` control command, and
wait for the terminal Done / Failed NOTIFY.

The OTA Control/Data characteristics require an authenticated, bonded link
(WRITE_AUTHEN with BOND | MITM | SC, DISPLAY_ONLY on the device). The script
calls ``pair()`` and relies on the OS pairing agent: the device shows a passkey
and you enter it when prompted. Pass ``--no-pair`` if already bonded.

Requirements:
    pip install bleak cbor2

Usage:
    # Scan for a device by advertised name and flash it
    python scripts/ago_ota_ble.py firmware.bin --name "AirGradient OTA"

    # Connect to a known address directly
    python scripts/ago_ota_ble.py firmware.bin --address AA:BB:CC:DD:EE:FF

    # Tag the update with an informational firmware version string
    python scripts/ago_ota_ble.py firmware.bin --name "AirGradient OTA" --fw 3.1.21

    # Skip pairing (already bonded), smaller chunks, verbose logging
    python scripts/ago_ota_ble.py firmware.bin --name "AirGradient OTA" \\
        --no-pair --chunk-size 256 -v
"""

from __future__ import annotations

import argparse
import asyncio
import logging
import os
import sys
from typing import Any

import cbor2
from bleak import BleakClient, BleakScanner
from bleak.backends.device import BLEDevice

logger = logging.getLogger("ago_ota")

# ---------------------------------------------------------------------------
# GATT UUIDs — mirror components/airgradient-ota/services/ota_ble_service.cpp
# (placeholders pending allocation; keep in sync with the firmware).
# ---------------------------------------------------------------------------

OTA_SERVICE_UUID = "ab9a0001-1e3c-4f5a-9b6d-0a1b2c3d4e5f"
OTA_CONTROL_CHAR_UUID = "ab9a0002-1e3c-4f5a-9b6d-0a1b2c3d4e5f"
OTA_DATA_CHAR_UUID = "ab9a0003-1e3c-4f5a-9b6d-0a1b2c3d4e5f"
OTA_STATUS_CHAR_UUID = "ab9a0004-1e3c-4f5a-9b6d-0a1b2c3d4e5f"

# ---------------------------------------------------------------------------
# Protocol constants — mirror services/ota_ble_protocol.h.
# ---------------------------------------------------------------------------

OP_START = "start"
OP_END = "end"
OP_ABORT = "abort"

# Device single-Data-write cap (CONFIG_AG_OTA_BLE_DATA_MAX_BYTES); larger
# writes are rejected by the device.
MAX_CHUNK_SIZE = 512

# Inter-chunk delay (seconds). With Write-Without-Response there is no ATT ACK
# to pace on, so a small gap keeps the local TX queue from overrunning.
INTER_CHUNK_DELAY_S = 0.001

# OtaState wire values (device -> phone).
STATE_DOWNLOADING = 0x01
STATE_APPLYING = 0x02
STATE_DONE = 0x03
STATE_FAILED = 0x04

_STATE_NAMES = {
    STATE_DOWNLOADING: "Downloading",
    STATE_APPLYING: "Applying",
    STATE_DONE: "Done",
    STATE_FAILED: "Failed",
}

# OtaStatus wire values (device -> phone).
RESULT_OK = 0x00
RESULT_FLASH_ERROR = 0x01
RESULT_INVALID_IMAGE = 0x02
RESULT_TRANSPORT_ERROR = 0x03
RESULT_ABORTED = 0x04
RESULT_INVALID_ARGUMENT = 0x05

_RESULT_NAMES = {
    RESULT_OK: "Ok",
    RESULT_FLASH_ERROR: "FlashError",
    RESULT_INVALID_IMAGE: "InvalidImage",
    RESULT_TRANSPORT_ERROR: "TransportError",
    RESULT_ABORTED: "Aborted",
    RESULT_INVALID_ARGUMENT: "InvalidArgument",
}

# Status NOTIFY CBOR keys.
KEY_STATE = "state"
KEY_RESULT = "result"
KEY_BYTES = "bytes"  # device's real accepted/flashed byte count


def _state_name(value: int) -> str:
    return _STATE_NAMES.get(value, f"0x{value:02x}")


def _result_name(value: int) -> str:
    return _RESULT_NAMES.get(value, f"0x{value:02x}")


# ---------------------------------------------------------------------------
# CBOR encode helpers
# ---------------------------------------------------------------------------


def _encode_control(op: str, **fields: Any) -> bytes:
    """Build a Control CBOR command map, dropping None-valued fields."""
    payload: dict[str, Any] = {"op": op}
    for key, value in fields.items():
        if value is not None:
            payload[key] = value
    return cbor2.dumps(payload)


# ---------------------------------------------------------------------------
# Status NOTIFY collector
# ---------------------------------------------------------------------------


class _StatusCollector:
    """Async-safe container that accumulates decoded Status notifications."""

    def __init__(self) -> None:
        self._queue: asyncio.Queue[tuple[int, int]] = asyncio.Queue()
        # Image size, set after START so device-reported progress can show a %.
        self.total = 0

    def callback(self, _sender: Any, data: bytearray) -> None:
        raw = bytes(data)
        try:
            decoded = cbor2.loads(raw)
            state = int(decoded[KEY_STATE])
            result = int(decoded[KEY_RESULT])
            device_bytes = int(decoded.get(KEY_BYTES, 0))
        except Exception:
            logger.warning("Undecodable Status NOTIFY: %s", raw.hex())
            return

        logger.debug(
            "NOTIFY state=%s result=%s bytes=%d",
            _state_name(state),
            _result_name(result),
            device_bytes,
        )
        # Device-reported (true) progress — the phone's own send count runs
        # ahead of the link, so this is the real on-device figure.
        if state == STATE_DOWNLOADING and device_bytes > 0:
            pct = (device_bytes * 100 // self.total) if self.total else 0
            logger.info(
                "Device flashed: %3d%% (%d/%d bytes)",
                pct,
                device_bytes,
                self.total,
            )
        self._queue.put_nowait((state, result))

    async def wait_for_state(
        self, state: int, timeout: float
    ) -> tuple[int, int]:
        """Wait until a NOTIFY with ``state`` arrives.

        Raises RuntimeError if the device reports Failed while waiting.
        """
        while True:
            got_state, got_result = await asyncio.wait_for(
                self._queue.get(), timeout=timeout
            )
            if got_state == state:
                return got_state, got_result
            if got_state == STATE_FAILED:
                raise RuntimeError(
                    f"Device reported Failed ({_result_name(got_result)})"
                )
            logger.debug(
                "Ignoring intermediate state=%s while awaiting %s",
                _state_name(got_state),
                _state_name(state),
            )

    def drain_failure(self) -> tuple[int, int] | None:
        """Return a pending Failed NOTIFY without blocking, if any."""
        while not self._queue.empty():
            state, result = self._queue.get_nowait()
            if state == STATE_FAILED:
                return state, result
        return None


# ---------------------------------------------------------------------------
# BLE scanning
# ---------------------------------------------------------------------------


async def _scan_for_device(name: str, timeout: float) -> BLEDevice:
    """Scan for a device whose advertised name matches ``name``.

    Prefers an exact local_name match; falls back to a prefix match.
    """
    logger.info("Scanning for '%s' (timeout=%.0fs) ...", name, timeout)

    exact: BLEDevice | None = None
    prefix: BLEDevice | None = None
    event = asyncio.Event()

    def _on_detect(device: BLEDevice, adv: Any) -> None:
        nonlocal exact, prefix
        adv_name = adv.local_name or device.name or ""
        if exact is None and adv_name == name:
            logger.info("Found exact match: %s [%s]", adv_name, device.address)
            exact = device
            event.set()
        elif prefix is None and adv_name.startswith(name):
            logger.debug(
                "Found prefix match: %s [%s]", adv_name, device.address
            )
            prefix = device

    scanner = BleakScanner(detection_callback=_on_detect)
    await scanner.start()
    try:
        await asyncio.wait_for(event.wait(), timeout=timeout)
    except asyncio.TimeoutError:
        pass
    finally:
        await scanner.stop()

    device = exact or prefix
    if device is None:
        raise RuntimeError(
            f"No device matching '{name}' found after {timeout}s scan. "
            "Make sure the device is advertising the OTA service, "
            "or pass --address explicitly."
        )
    return device


# ---------------------------------------------------------------------------
# OTA transfer
# ---------------------------------------------------------------------------


async def _acquire_mtu(client: BleakClient) -> int:
    """Negotiate / read the real ATT MTU.

    On BlueZ ``mtu_size`` returns a hard-coded 23 until the MTU is acquired
    (and only after bonding for encrypted characteristics). Trigger the
    backend acquisition when available, then return the negotiated value.
    Falls back to ``mtu_size`` on backends without the hook.
    """
    backend = getattr(client, "_backend", None)
    acquire = getattr(backend, "_acquire_mtu", None)
    if acquire is not None:
        try:
            await acquire()
        except Exception as exc:  # noqa: BLE001 - best-effort negotiation
            logger.debug("MTU acquisition failed: %s", exc)
    return client.mtu_size


def _resolve_chunk_size(requested: int | None, mtu: int) -> int:
    """Pick a chunk size that fits one ATT write and the device buffer."""
    # ATT write payload is MTU - 3 bytes; cap to the device chunk buffer.
    att_payload = max(mtu - 3, 20)
    chunk = min(att_payload, MAX_CHUNK_SIZE)
    if requested is not None:
        chunk = min(requested, MAX_CHUNK_SIZE)
    return max(chunk, 1)


async def _stream_image(
    client: BleakClient,
    image: bytes,
    chunk_size: int,
    collector: _StatusCollector,
) -> None:
    """Write the image to the Data characteristic, chunk by chunk."""
    total = len(image)
    sent = 0
    last_logged_pct = -1

    while sent < total:
        # A Failed NOTIFY may arrive mid-stream (stall/overflow on the device).
        failure = collector.drain_failure()
        if failure is not None:
            raise RuntimeError(
                f"Device aborted mid-stream ({_result_name(failure[1])})"
            )

        end = min(sent + chunk_size, total)
        chunk = image[sent:end]
        # response=False: Write-Without-Response. The device flashes each write
        # straight from its callback; there is no per-chunk ACK. Errors surface
        # only via the Status NOTIFY (drained above).
        await client.write_gatt_char(OTA_DATA_CHAR_UUID, chunk, response=False)
        sent = end

        pct = (sent * 100) // total
        if pct != last_logged_pct:
            logger.info("Progress: %3d%% (%d/%d bytes)", pct, sent, total)
            last_logged_pct = pct

        if sent < total:
            await asyncio.sleep(INTER_CHUNK_DELAY_S)


async def _run(args: argparse.Namespace) -> None:
    # --- Load and validate the image ---
    if not os.path.isfile(args.firmware):
        raise RuntimeError(f"Firmware file not found: {args.firmware}")
    with open(args.firmware, "rb") as fh:
        image = fh.read()
    if not image:
        raise RuntimeError(f"Firmware file is empty: {args.firmware}")
    total = len(image)
    logger.info("Image: %s (%d bytes)", args.firmware, total)

    # --- Resolve device ---
    if args.address:
        logger.info("Using device address: %s", args.address)
        device: BLEDevice | str = args.address
    else:
        if not args.name:
            raise RuntimeError("Provide --name or --address to select a device.")
        device = await _scan_for_device(args.name, args.scan_timeout)

    # --- Connect + pair ---
    logger.info("Connecting ...")
    client = BleakClient(device)
    await client.connect(timeout=15.0)
    logger.info("Connected.")

    transfer_started = False
    try:
        if not args.no_pair:
            logger.info("Pairing (enter the passkey shown on the device) ...")
            await client.pair()
            logger.info("Paired.")

        # Acquire the real MTU (after bonding) before sizing chunks.
        mtu = await _acquire_mtu(client)
        logger.info("Negotiated MTU: %d", mtu)
        chunk_size = _resolve_chunk_size(args.chunk_size, mtu)
        logger.info("Chunk size: %d bytes", chunk_size)

        # Subscribe to Status notifications before driving the flow.
        collector = _StatusCollector()
        collector.total = total  # lets device-reported progress show a %
        await client.start_notify(OTA_STATUS_CHAR_UUID, collector.callback)

        # START
        logger.info("Sending START (total=%d, fw=%s) ...", total, args.fw)
        start_cmd = _encode_control(OP_START, total=total, fw=args.fw)
        await client.write_gatt_char(
            OTA_CONTROL_CHAR_UUID, start_cmd, response=True
        )
        transfer_started = True

        # Wait for the ready (Downloading) NOTIFY before streaming.
        logger.info("Waiting for device ready ...")
        await collector.wait_for_state(STATE_DOWNLOADING, args.notify_timeout)
        logger.info("Device ready, streaming image ...")

        # Stream
        await _stream_image(client, image, chunk_size, collector)

        # END
        logger.info("Sending END ...")
        end_cmd = _encode_control(OP_END)
        await client.write_gatt_char(
            OTA_CONTROL_CHAR_UUID, end_cmd, response=True
        )

        # Wait for terminal Done.
        logger.info("Waiting for device to apply image ...")
        _, result = await collector.wait_for_state(
            STATE_DONE, args.notify_timeout
        )
        transfer_started = False
        if result == RESULT_OK:
            logger.info("OTA complete: device accepted the image (Done/Ok).")
        else:
            raise RuntimeError(
                f"Device reported Done with result {_result_name(result)}"
            )

    except (Exception, asyncio.CancelledError):
        # Best-effort abort so the device tears down its worker promptly.
        if transfer_started and client.is_connected:
            try:
                logger.info("Sending ABORT ...")
                await client.write_gatt_char(
                    OTA_CONTROL_CHAR_UUID, _encode_control(OP_ABORT), response=True
                )
            except Exception:
                logger.debug("ABORT write failed (link may be gone).")
        raise

    finally:
        try:
            await client.stop_notify(OTA_STATUS_CHAR_UUID)
        except Exception:
            pass
        if client.is_connected:
            await client.disconnect()
        logger.info("Disconnected.")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def main() -> None:
    parser = argparse.ArgumentParser(
        description=(
            "Push a firmware image to an AirGradient device over BLE using "
            "the OtaBleService GATT flow (happy path)."
        ),
    )

    parser.add_argument(
        "firmware",
        help="Path to the firmware binary (.bin) to flash.",
    )
    parser.add_argument(
        "--fw",
        default=None,
        help="Informational firmware version string sent in START (optional).",
    )

    conn_group = parser.add_argument_group("connection")
    conn_group.add_argument(
        "--name",
        default=None,
        help="Advertised BLE name to scan for (exact, then prefix match).",
    )
    conn_group.add_argument(
        "--address",
        default=None,
        help="BLE address of the device. Skips scanning when provided.",
    )
    conn_group.add_argument(
        "--no-pair",
        action="store_true",
        help="Skip pairing (assume the host is already bonded).",
    )
    conn_group.add_argument(
        "--scan-timeout",
        type=float,
        default=10.0,
        help="BLE scan timeout in seconds (default: 10).",
    )
    conn_group.add_argument(
        "--notify-timeout",
        type=float,
        default=30.0,
        help="Max seconds to wait for a Status NOTIFY (default: 30).",
    )

    xfer_group = parser.add_argument_group("transfer")
    xfer_group.add_argument(
        "--chunk-size",
        type=int,
        default=None,
        help=(
            "Bytes per Data write. Default min(MTU-3, 512); "
            f"capped at {MAX_CHUNK_SIZE}."
        ),
    )

    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="Enable verbose (DEBUG) logging.",
    )

    args = parser.parse_args()

    level = logging.DEBUG if args.verbose else logging.INFO
    logging.basicConfig(
        level=level,
        format="%(asctime)s %(levelname)-5s %(message)s",
        datefmt="%H:%M:%S",
    )
    logging.getLogger("bleak").setLevel(logging.WARNING)

    try:
        asyncio.run(_run(args))
    except KeyboardInterrupt:
        logger.info("Interrupted by user.")
        sys.exit(1)
    except RuntimeError as exc:
        logger.error("Error: %s", exc)
        sys.exit(1)


if __name__ == "__main__":
    main()
