#!/usr/bin/env python3
"""Send A-GNSS aiding data to an AirGradient Go device over BLE.

Injects approximate position and/or UTC time into the device's GPS module to
reduce cold-start time-to-first-fix (TTFF).

By default the script injects the current system UTC time automatically.
Pass ``--no-auto-time`` to disable this, or ``--epoch`` to override with a
specific value.

Requirements:
    pip install bleak cbor2

Usage:
    # Auto system time only (default)
    python scripts/ago_send_aiding.py

    # Position + auto system time
    python scripts/ago_send_aiding.py --lat 47.376887 --lon 8.541694

    # Position with accuracy, explicit epoch
    python scripts/ago_send_aiding.py --lat 47.376887 --lon 8.541694 \\
        --pos-acc 50 --epoch 1711234567 --time-acc 2000

    # Position only, no time injection
    python scripts/ago_send_aiding.py --lat 47.376887 --lon 8.541694 --no-auto-time

    # Specify device address explicitly
    python scripts/ago_send_aiding.py --address AA:BB:CC:DD:EE:FF

    # Verbose output
    python scripts/ago_send_aiding.py -v
"""

from __future__ import annotations

import argparse
import asyncio
import logging
import sys
import time
from typing import Any

import cbor2
from bleak import BleakClient, BleakScanner
from bleak.backends.device import BLEDevice

logger = logging.getLogger("ago_aiding")

# ---------------------------------------------------------------------------
# GATT UUIDs
# ---------------------------------------------------------------------------

SERVICE_UUID = "d1c0c0a0-6b48-4b2a-9b1d-59f9f2b0a1e1"
CHAR_CONFIG_UUID = "d1c0c0a3-6b48-4b2a-9b1d-59f9f2b0a1e1"

# ---------------------------------------------------------------------------
# CBOR encode helper
# ---------------------------------------------------------------------------


def _encode_set_aiding(
    lat: float | None,
    lon: float | None,
    alt: float | None,
    pos_acc: float | None,
    epoch: int | None,
    time_acc: int | None,
) -> bytes:
    """Build the ``set_aiding`` CBOR command with only non-None fields."""
    payload: dict[str, Any] = {"op": "cmd", "cmd": "set_aiding"}

    if lat is not None:
        payload["lat"] = lat
    if lon is not None:
        payload["lon"] = lon
    if alt is not None:
        payload["alt"] = alt
    if pos_acc is not None:
        payload["pos_acc"] = pos_acc
    if epoch is not None:
        payload["epoch"] = epoch
    if time_acc is not None:
        payload["time_acc"] = time_acc

    return cbor2.dumps(payload)


# ---------------------------------------------------------------------------
# Async notification collector
# ---------------------------------------------------------------------------


class _NotificationCollector:
    """Async-safe container that accumulates BLE notifications."""

    def __init__(self) -> None:
        self._queue: asyncio.Queue[bytes] = asyncio.Queue()

    def callback(self, _sender: Any, data: bytearray) -> None:
        raw = bytes(data)
        self._queue.put_nowait(raw)

        if logger.isEnabledFor(logging.DEBUG):
            try:
                decoded = cbor2.loads(raw)
                logger.debug("NOTIFY CBOR  %s", decoded)
            except Exception:
                logger.debug("NOTIFY RAW   %s", raw.hex())

    async def wait_for(self, timeout: float) -> bytes:
        return await asyncio.wait_for(self._queue.get(), timeout=timeout)


# ---------------------------------------------------------------------------
# BLE scanning
# ---------------------------------------------------------------------------


_AGO_NAME_PREFIX = "AirGradient Go "


async def _scan_for_ago(timeout: float) -> BLEDevice:
    """Scan for a BLE device whose name matches the AGo prefix."""
    logger.info("Scanning for AGo device (timeout=%.0fs) ...", timeout)

    found: BLEDevice | None = None
    event = asyncio.Event()

    def _on_detect(device: BLEDevice, adv: Any) -> None:
        nonlocal found
        name = adv.local_name or device.name or ""
        if found is None and name.startswith(_AGO_NAME_PREFIX):
            logger.info("Found device: %s [%s]", name, device.address)
            found = device
            event.set()

    scanner = BleakScanner(detection_callback=_on_detect)
    await scanner.start()
    try:
        await asyncio.wait_for(event.wait(), timeout=timeout)
    except asyncio.TimeoutError:
        pass
    finally:
        await scanner.stop()

    if found is None:
        raise RuntimeError(
            f"No AGo device found after {timeout}s scan. "
            "Make sure the device is advertising in Portable mode, "
            "or pass --address explicitly."
        )
    return found


# ---------------------------------------------------------------------------
# Main logic
# ---------------------------------------------------------------------------


def _describe_payload(
    lat: float | None,
    lon: float | None,
    alt: float | None,
    pos_acc: float | None,
    epoch: int | None,
    time_acc: int | None,
) -> str:
    """Build a human-readable summary of what will be sent."""
    parts: list[str] = []

    if lat is not None and lon is not None:
        pos = f"  Position : lat={lat}, lon={lon}"
        if alt is not None:
            pos += f", alt={alt}m"
        if pos_acc is not None:
            pos += f", acc={pos_acc}m"
        parts.append(pos)

    if epoch is not None:
        t = f"  Time     : epoch={epoch}"
        if time_acc is not None:
            t += f", acc={time_acc}ms"
        parts.append(t)

    return "\n".join(parts) if parts else "  (no data)"


async def _run(args: argparse.Namespace) -> None:
    # --- Resolve aiding fields ---
    lat: float | None = args.lat
    lon: float | None = args.lon
    alt: float | None = args.alt
    pos_acc: float | None = args.pos_acc
    epoch: int | None = args.epoch
    time_acc: int | None = args.time_acc

    # Validate lat/lon pair
    if (lat is None) != (lon is None):
        raise RuntimeError(
            "Position injection requires both --lat and --lon. "
            "Provide both or neither."
        )

    # Auto-time: inject current system UTC unless disabled or explicit epoch
    if epoch is None and not args.no_auto_time:
        epoch = int(time.time())
        logger.info("Auto-time: using current UTC epoch=%d", epoch)

    # Only include time_acc when we actually have an epoch
    if epoch is None:
        time_acc = None

    # Final check: must have something useful
    has_position = lat is not None and lon is not None
    has_time = epoch is not None
    if not has_position and not has_time:
        raise RuntimeError(
            "No aiding data to send. Provide --lat/--lon for position, "
            "--epoch for time, or let auto-time inject the system clock "
            "(enabled by default)."
        )

    logger.info("Aiding data to send:\n%s", _describe_payload(lat, lon, alt, pos_acc, epoch, time_acc))

    # --- Resolve device ---
    if args.address:
        logger.info("Using device address: %s", args.address)
        device = BLEDevice(address=args.address, name=args.address, details={})
    else:
        device = await _scan_for_ago(args.scan_timeout)

    # --- Connect ---
    logger.info("Connecting to %s ...", device.address)
    client = BleakClient(device)
    await client.connect(timeout=15.0)
    logger.info("Connected (MTU=%d)", client.mtu_size)

    try:
        # Subscribe to Config notifications (cmd_result comes here)
        collector = _NotificationCollector()
        await client.start_notify(CHAR_CONFIG_UUID, collector.callback)

        # Build and send the aiding command
        payload = _encode_set_aiding(lat, lon, alt, pos_acc, epoch, time_acc)
        logger.info("Sending set_aiding command (%d bytes) ...", len(payload))
        await client.write_gatt_char(CHAR_CONFIG_UUID, payload, response=True)

        # Wait for cmd_result notification
        try:
            data = await collector.wait_for(timeout=args.notify_timeout)
        except asyncio.TimeoutError:
            raise RuntimeError(
                f"No response from device within {args.notify_timeout}s. "
                "The command may not have been processed."
            )

        result = cbor2.loads(data)
        logger.debug("Response: %s", result)

        msg_type = result.get("type")
        if msg_type != "cmd_result":
            # Could be a config notification; try once more for the cmd_result
            logger.debug("Got type=%s, waiting for cmd_result ...", msg_type)
            try:
                data = await collector.wait_for(timeout=args.notify_timeout)
                result = cbor2.loads(data)
            except asyncio.TimeoutError:
                raise RuntimeError(
                    "Received a config notification but no cmd_result. "
                    f"Last response: {result}"
                )

        if result.get("type") != "cmd_result":
            raise RuntimeError(f"Unexpected response type: {result}")

        if result.get("ok"):
            logger.info("Success: aiding data accepted by device.")
        else:
            err = result.get("err", "unknown")
            raise RuntimeError(f"Device rejected aiding data: {err}")

    finally:
        await client.stop_notify(CHAR_CONFIG_UUID)
        if client.is_connected:
            await client.disconnect()
        logger.info("Disconnected.")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def main() -> None:
    parser = argparse.ArgumentParser(
        description=(
            "Send A-GNSS aiding data (position / time) to an AirGradient Go "
            "device via BLE to reduce GPS cold-start TTFF."
        ),
    )

    # Aiding fields
    pos_group = parser.add_argument_group("position aiding")
    pos_group.add_argument(
        "--lat",
        type=float,
        default=None,
        help="Approximate latitude in decimal degrees.",
    )
    pos_group.add_argument(
        "--lon",
        type=float,
        default=None,
        help="Approximate longitude in decimal degrees.",
    )
    pos_group.add_argument(
        "--alt",
        type=float,
        default=None,
        help="Approximate altitude in meters MSL.",
    )
    pos_group.add_argument(
        "--pos-acc",
        type=float,
        default=None,
        help="Position accuracy estimate in meters (1-sigma).",
    )

    time_group = parser.add_argument_group("time aiding")
    time_group.add_argument(
        "--epoch",
        type=int,
        default=None,
        help=(
            "UTC time as POSIX seconds. Overrides auto-time when provided."
        ),
    )
    time_group.add_argument(
        "--time-acc",
        type=int,
        default=1000,
        help="Time accuracy estimate in milliseconds (default: 1000).",
    )
    time_group.add_argument(
        "--no-auto-time",
        action="store_true",
        help=(
            "Disable automatic system time injection. By default the script "
            "sends the current UTC time unless --epoch is given."
        ),
    )

    # Connection
    conn_group = parser.add_argument_group("connection")
    conn_group.add_argument(
        "--address",
        default=None,
        help="BLE address of the AGo device. Auto-scans if omitted.",
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
        default=10.0,
        help="Max seconds to wait for a BLE notification (default: 10).",
    )

    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="Enable verbose (DEBUG) logging.",
    )

    args = parser.parse_args()

    # Configure logging
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
