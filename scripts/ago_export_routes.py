#!/usr/bin/env python3
"""Export all route sessions from an AirGradient Go device over BLE to CSV.

Connects to an AGo device, lists all stored route sessions, downloads each
one with gap recovery, and saves each session as a separate CSV file.

Requirements:
    pip install bleak cbor2

Usage:
    # Auto-scan for a device whose name starts with "AGo-"
    python scripts/ago_export_routes.py

    # Specify a device address explicitly
    python scripts/ago_export_routes.py --address AA:BB:CC:DD:EE:FF

    # Save CSV files to a specific directory
    python scripts/ago_export_routes.py -o ~/exports

    # Verbose output (shows decoded BLE notifications)
    python scripts/ago_export_routes.py -v
"""

from __future__ import annotations

import argparse
import asyncio
import csv
import logging
import struct
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import cbor2
from bleak import BleakClient, BleakScanner
from bleak.backends.device import BLEDevice

logger = logging.getLogger("ago_export")

# ---------------------------------------------------------------------------
# GATT UUIDs
# ---------------------------------------------------------------------------

SERVICE_UUID = "d1c0c0a0-6b48-4b2a-9b1d-59f9f2b0a1e1"
CHAR_HISTORY_UUID = "d1c0c0a4-6b48-4b2a-9b1d-59f9f2b0a1e1"

# ---------------------------------------------------------------------------
# History protocol constants
# ---------------------------------------------------------------------------

HISTORY_TAG_CBOR = 0x00
HISTORY_TAG_BINARY = 0x01

ROUTE_POINT_WIRE_SIZE = 56
MAX_FILL_INDICES = 50  # Device write buffer limits ~50 indices per fill request
MAX_POINTS_PER_CHUNK = 4

# struct format for RoutePointWire (little-endian, 56 bytes):
# uint32 timestamp | float64 lat | float64 lon | float32 alt | uint8 fix |
# float32 temp | float32 hum | float32 pm1 | float32 pm25 | float32 pm10 |
# int16 co2 | int16 tvoc | int16 nox | float32 pressure | uint8 battery_pct
_ROUTE_POINT_FMT = "<I d d f B f f f f f h h h f B"
_ROUTE_POINT_STRUCT = struct.Struct(_ROUTE_POINT_FMT)

assert _ROUTE_POINT_STRUCT.size == ROUTE_POINT_WIRE_SIZE

ROUTE_POINT_FIELDS = (
    "timestamp",
    "latitude",
    "longitude",
    "altitude",
    "gps_fix",
    "temperature",
    "humidity",
    "pm_01",
    "pm_25",
    "pm_10",
    "co2",
    "tvoc_index",
    "nox_index",
    "pressure",
    "battery_percentage",
)

# Invalid sentinel values — when a field equals its sentinel the reading is
# unavailable and should be written as an empty CSV cell.
SENTINELS: dict[str, Any] = {
    "latitude": 91.0,
    "longitude": 181.0,
    "altitude": -10000.0,
    "temperature": -1001.0,
    "humidity": -1.0,
    "pm_01": -1.0,
    "pm_25": -1.0,
    "pm_10": -1.0,
    "co2": -1,
    "tvoc_index": -1,
    "nox_index": -1,
    "pressure": -1001.0,
    "battery_percentage": 255,
}

# CSV column headers (human-readable)
CSV_HEADERS = [
    "timestamp",
    "datetime_utc",
    "latitude",
    "longitude",
    "altitude_m",
    "gps_fix",
    "temperature_c",
    "humidity_pct",
    "pm1_ugm3",
    "pm25_ugm3",
    "pm10_ugm3",
    "co2_ppm",
    "tvoc_index",
    "nox_index",
    "pressure_hpa",
    "battery_pct",
]

# ---------------------------------------------------------------------------
# CBOR encode helpers
# ---------------------------------------------------------------------------


def _encode_list() -> bytes:
    return cbor2.dumps({"op": "list"})


def _encode_start(session_id: int) -> bytes:
    return cbor2.dumps({"op": "start", "session": session_id})


def _encode_fill(point_indices: list[int]) -> bytes:
    return cbor2.dumps({"op": "fill", "pts": point_indices})


def _encode_end() -> bytes:
    return cbor2.dumps({"op": "end"})


# ---------------------------------------------------------------------------
# Binary parsers
# ---------------------------------------------------------------------------


def _parse_route_point(data: bytes, offset: int = 0) -> dict[str, Any]:
    """Decode a single 56-byte RoutePointWire from *data* at *offset*."""
    values = _ROUTE_POINT_STRUCT.unpack_from(data, offset)
    return dict(zip(ROUTE_POINT_FIELDS, values))


def _parse_notification(data: bytes) -> tuple[int, Any]:
    """Parse a History characteristic notification.

    Returns (tag, payload) where:
      - tag == HISTORY_TAG_CBOR:   payload is the decoded CBOR dict
      - tag == HISTORY_TAG_BINARY: payload is (point_index, [route_point_dicts])
    """
    if len(data) < 1:
        raise ValueError("Empty history notification")

    tag = data[0]
    body = data[1:]

    if tag == HISTORY_TAG_CBOR:
        return tag, cbor2.loads(body)

    if tag == HISTORY_TAG_BINARY:
        if len(body) < 2:
            raise ValueError(f"Binary chunk too short: {len(body)} bytes")

        point_index = struct.unpack_from("<H", body, 0)[0]
        payload = body[2:]

        if len(payload) % ROUTE_POINT_WIRE_SIZE != 0:
            raise ValueError(
                f"Payload size {len(payload)} is not a multiple of "
                f"{ROUTE_POINT_WIRE_SIZE}"
            )

        points: list[dict[str, Any]] = []
        off = 0
        while off < len(payload):
            points.append(_parse_route_point(payload, off))
            off += ROUTE_POINT_WIRE_SIZE

        return tag, (point_index, points)

    raise ValueError(f"Unknown history tag: 0x{tag:02x}")


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
                tag, payload = _parse_notification(raw)
                if tag == HISTORY_TAG_CBOR:
                    logger.debug("NOTIFY CBOR  %s", payload)
                elif tag == HISTORY_TAG_BINARY:
                    idx, points = payload
                    logger.debug(
                        "NOTIFY BIN   idx=%d, %d point(s): %s",
                        idx,
                        len(points),
                        points,
                    )
            except Exception:
                logger.debug("NOTIFY RAW   %s", raw.hex())

    async def wait_for(self, timeout: float) -> bytes:
        return await asyncio.wait_for(self._queue.get(), timeout=timeout)


# ---------------------------------------------------------------------------
# BLE scanning
# ---------------------------------------------------------------------------


async def _scan_for_ago(timeout: float) -> BLEDevice:
    """Scan for a BLE device whose name starts with 'AGo-'."""
    logger.info("Scanning for AGo device (timeout=%.0fs) ...", timeout)

    found: BLEDevice | None = None
    event = asyncio.Event()

    def _on_detect(device: BLEDevice, adv: Any) -> None:
        nonlocal found
        name = adv.local_name or device.name or ""
        if name.startswith("AGo-") and found is None:
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
# Protocol operations
# ---------------------------------------------------------------------------


async def _list_sessions(
    client: BleakClient,
    collector: _NotificationCollector,
    timeout: float,
) -> list[dict]:
    """Send 'list' and return the sessions array."""
    await client.write_gatt_char(CHAR_HISTORY_UUID, _encode_list(), response=True)

    data = await collector.wait_for(timeout=timeout)
    tag, payload = _parse_notification(data)

    if tag != HISTORY_TAG_CBOR:
        raise RuntimeError(f"Expected CBOR response, got tag 0x{tag:02x}")
    if payload.get("type") == "error":
        raise RuntimeError(f"Device error on list: {payload.get('err')}")
    if payload.get("type") != "sessions":
        raise RuntimeError(f"Unexpected response type: {payload.get('type')}")

    return payload.get("sessions", [])


async def _download_session(
    client: BleakClient,
    collector: _NotificationCollector,
    session_id: int,
    timeout: float,
) -> list[dict[str, Any]]:
    """Download all route points for a session, with gap recovery.

    Returns a list of route-point dicts sorted by index.
    """
    # --- Start download ---
    await client.write_gatt_char(
        CHAR_HISTORY_UUID, _encode_start(session_id), response=True
    )

    started, received_points, total = await _collect_stream(collector, timeout)

    if started is None:
        raise RuntimeError("No 'started' response received")

    total_pts = started.get("total", 0)
    pt_size = started.get("pt_size", 0)
    if pt_size != ROUTE_POINT_WIRE_SIZE:
        raise RuntimeError(
            f"Incompatible pt_size: expected {ROUTE_POINT_WIRE_SIZE}, got {pt_size}"
        )

    logger.info(
        "  Session %d: started (total=%d points)", session_id, total_pts
    )

    # --- Gap recovery ---
    max_fill_rounds = 5
    for fill_round in range(max_fill_rounds):
        missing = _find_missing(received_points, total_pts)
        if not missing:
            break

        logger.info(
            "  Recovering %d missing points (round %d) ...",
            len(missing),
            fill_round + 1,
        )

        # Send fill requests in batches of MAX_FILL_INDICES
        for batch_start in range(0, len(missing), MAX_FILL_INDICES):
            batch = missing[batch_start : batch_start + MAX_FILL_INDICES]
            await client.write_gatt_char(
                CHAR_HISTORY_UUID, _encode_fill(batch), response=True
            )
            _, fill_points, _ = await _collect_stream(collector, timeout)
            received_points.update(fill_points)

    # --- End download ---
    await client.write_gatt_char(
        CHAR_HISTORY_UUID, _encode_end(), response=True
    )

    # Consume the 'ended' response
    try:
        end_data = await collector.wait_for(timeout=timeout)
        tag, payload = _parse_notification(end_data)
        if tag == HISTORY_TAG_CBOR and payload.get("type") == "ended":
            logger.debug("  Download ended cleanly")
    except asyncio.TimeoutError:
        logger.debug("  No 'ended' response (non-critical)")

    # Check final coverage
    final_missing = _find_missing(received_points, total_pts)
    if final_missing:
        logger.warning(
            "  %d points still missing after recovery: %s",
            len(final_missing),
            final_missing[:20],
        )

    # Build sorted list
    points_sorted = [received_points[i] for i in sorted(received_points.keys())]
    return points_sorted


async def _collect_stream(
    collector: _NotificationCollector,
    timeout: float,
) -> tuple[dict | None, dict[int, dict[str, Any]], dict | None]:
    """Collect notifications from a download/fill stream.

    Returns (started_msg, {index: route_point_dict}, done_msg).
    """
    started: dict | None = None
    received: dict[int, dict[str, Any]] = {}
    done: dict | None = None

    deadline = asyncio.get_event_loop().time() + timeout

    while True:
        remaining = deadline - asyncio.get_event_loop().time()
        if remaining <= 0:
            break

        try:
            data = await collector.wait_for(timeout=remaining)
        except asyncio.TimeoutError:
            break

        tag, payload = _parse_notification(data)

        if tag == HISTORY_TAG_CBOR:
            msg_type = payload.get("type")
            if msg_type == "started":
                started = payload
            elif msg_type == "done":
                done = payload
                break
            elif msg_type == "error":
                raise RuntimeError(f"Device error: {payload.get('err')}")
        elif tag == HISTORY_TAG_BINARY:
            point_index, points = payload
            for i, pt in enumerate(points):
                received[point_index + i] = pt

    return started, received, done


def _find_missing(received: dict[int, Any], total: int) -> list[int]:
    """Return sorted list of missing point indices."""
    return sorted(set(range(total)) - set(received.keys()))


# ---------------------------------------------------------------------------
# CSV writing
# ---------------------------------------------------------------------------


def _clean_value(field: str, value: Any) -> Any:
    """Return empty string if the value matches the field's sentinel."""
    sentinel = SENTINELS.get(field)
    if sentinel is not None and value == sentinel:
        return ""
    return value


def _point_to_row(point: dict[str, Any]) -> list[Any]:
    """Convert a route point dict to a CSV row, applying sentinel filtering."""
    ts = point["timestamp"]

    # Human-readable UTC datetime
    try:
        dt_str = datetime.fromtimestamp(ts, tz=timezone.utc).strftime(
            "%Y-%m-%d %H:%M:%S"
        )
    except (OSError, ValueError):
        dt_str = ""

    return [
        ts,
        dt_str,
        _clean_value("latitude", point["latitude"]),
        _clean_value("longitude", point["longitude"]),
        _clean_value("altitude", point["altitude"]),
        point["gps_fix"],
        _clean_value("temperature", point["temperature"]),
        _clean_value("humidity", point["humidity"]),
        _clean_value("pm_01", point["pm_01"]),
        _clean_value("pm_25", point["pm_25"]),
        _clean_value("pm_10", point["pm_10"]),
        _clean_value("co2", point["co2"]),
        _clean_value("tvoc_index", point["tvoc_index"]),
        _clean_value("nox_index", point["nox_index"]),
        _clean_value("pressure", point["pressure"]),
        _clean_value("battery_percentage", point["battery_percentage"]),
    ]


def _write_csv(
    session_id: int,
    points: list[dict[str, Any]],
    output_dir: Path,
) -> Path:
    """Write points to a CSV file. Returns the full output path."""
    filepath = output_dir / f"AGo_session_{session_id}.csv"

    with open(filepath, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(CSV_HEADERS)
        for pt in points:
            writer.writerow(_point_to_row(pt))

    return filepath


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


async def _run(args: argparse.Namespace) -> None:
    # Resolve output directory
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    # Resolve device
    if args.address:
        logger.info("Using device address: %s", args.address)
        device = BLEDevice(address=args.address, name=args.address, details={})
    else:
        device = await _scan_for_ago(args.scan_timeout)

    # Connect
    logger.info("Connecting to %s ...", device.address)
    client = BleakClient(device)
    await client.connect(timeout=15.0)
    logger.info("Connected (MTU=%d)", client.mtu_size)

    try:
        # Subscribe to History notifications
        collector = _NotificationCollector()
        await client.start_notify(CHAR_HISTORY_UUID, collector.callback)

        # List sessions
        sessions = await _list_sessions(client, collector, args.notify_timeout)

        if not sessions:
            logger.info("No route sessions on device. Nothing to export.")
            return

        logger.info("Found %d session(s) on device:", len(sessions))
        for s in sessions:
            ts_str = ""
            try:
                ts_str = datetime.fromtimestamp(
                    s["ts"], tz=timezone.utc
                ).strftime("%Y-%m-%d %H:%M:%S UTC")
            except (OSError, ValueError, KeyError):
                pass
            logger.info(
                "  Session %d: %d points, started %s",
                s["id"],
                s["pts"],
                ts_str,
            )

        # Download each session
        for i, session in enumerate(sessions):
            sid = session["id"]
            total_pts = session["pts"]
            logger.info(
                "Downloading session %d (%d/%d) ...",
                sid,
                i + 1,
                len(sessions),
            )

            if total_pts == 0:
                logger.info("  Session %d has 0 points, skipping.", sid)
                continue

            # Generous timeout: base + proportional to point count
            download_timeout = max(args.notify_timeout, total_pts * 0.1 + 10)

            points = await _download_session(
                client, collector, sid, download_timeout
            )

            filepath = _write_csv(sid, points, output_dir)
            logger.info(
                "  Saved %d points to %s",
                len(points),
                filepath,
            )

        logger.info("Export complete.")

    finally:
        await client.stop_notify(CHAR_HISTORY_UUID)
        if client.is_connected:
            await client.disconnect()
        logger.info("Disconnected.")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Export route history from an AirGradient Go device via BLE.",
    )
    parser.add_argument(
        "--address",
        default=None,
        help="BLE address of the AGo device. Auto-scans if omitted.",
    )
    parser.add_argument(
        "--scan-timeout",
        type=float,
        default=10.0,
        help="BLE scan timeout in seconds (default: 10).",
    )
    parser.add_argument(
        "--notify-timeout",
        type=float,
        default=30.0,
        help="Max seconds to wait for a BLE notification (default: 30).",
    )
    parser.add_argument(
        "-o",
        "--output-dir",
        default=".",
        help="Directory to save CSV files (default: current directory).",
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
    # Suppress noisy bleak internals
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
