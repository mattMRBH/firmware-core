"""Tests for the Measures characteristic (notify-only, CBOR payload).

A single module-scoped fixture subscribes to Measures notifications once,
waits for one notification, then unsubscribes.  All tests validate different
aspects of that same captured payload — no per-test BLE I/O.
"""

from __future__ import annotations

import pytest_asyncio
from bleak import BleakClient

import ago_protocol as proto
from conftest import NotificationCollector


# ---------------------------------------------------------------------------
# Module-scoped fixture: subscribe once, capture one notification
# ---------------------------------------------------------------------------

@pytest_asyncio.fixture(scope="module")
async def measures_payload(
    ago_client: BleakClient,
    ago_notify_timeout: float,
) -> bytes:
    """Subscribe to Measures, wait for one notification, unsubscribe."""
    collector = NotificationCollector()
    await ago_client.start_notify(proto.CHAR_MEASURES_UUID, collector.callback)
    data = await collector.wait_for(timeout=ago_notify_timeout)
    await ago_client.stop_notify(proto.CHAR_MEASURES_UUID)
    return data


# ---------------------------------------------------------------------------
# Tests — pure data validation, no async
# ---------------------------------------------------------------------------

class TestMeasures:
    """Verify Measures notifications arrive and contain valid CBOR payloads."""

    def test_receives_notification(self, measures_payload: bytes):
        """At least one Measures notification must arrive within the timeout.

        The device sends notifications on each SensorDataReady event while
        connected.  The measurement interval is typically 10-60 s depending
        on settings, so we wait up to the configured timeout (in fixture).
        """
        assert len(measures_payload) > 0, "Received empty notification"

    def test_cbor_decode(self, measures_payload: bytes):
        """The notification payload must be valid CBOR that decodes to a map."""
        ok, result = proto.decode_cbor_safe(measures_payload)
        assert ok, f"CBOR decode failed: {result}"
        assert isinstance(result, dict), f"Expected CBOR map, got {type(result).__name__}"

    def test_timestamp_always_present(self, measures_payload: bytes):
        """Every Measures notification must contain the 'ts' key (unix epoch)."""
        payload = proto.decode_cbor(measures_payload)
        assert "ts" in payload, f"'ts' key missing from Measures payload: {payload.keys()}"
        assert isinstance(payload["ts"], int), (
            f"'ts' must be uint, got {type(payload['ts']).__name__}: {payload['ts']}"
        )

    def test_known_keys_only(self, measures_payload: bytes):
        """All keys in the Measures payload must be from the known set."""
        payload = proto.decode_cbor(measures_payload)
        unknown = set(payload.keys()) - proto.MEASURES_ALL_KEYS
        assert not unknown, f"Unknown keys in Measures payload: {unknown}"

    def test_field_types(self, measures_payload: bytes):
        """Each field in the Measures payload must have the expected CBOR type."""
        payload = proto.decode_cbor(measures_payload)

        for key, value in payload.items():
            expected_types = proto.MEASURES_FIELD_TYPES.get(key)
            if expected_types is None:
                continue  # unknown key checked in separate test
            assert isinstance(value, expected_types), (
                f"Measures['{key}']: expected {expected_types}, "
                f"got {type(value).__name__} = {value!r}"
            )

    def test_gps_field_grouping(self, measures_payload: bytes):
        """GPS fields must follow the grouping rules from the protocol spec.

        - If any GPS field is present, 'fix' and 'sat' must also be present.
        - 'lat', 'lon', 'alt' are individually conditional but only appear
          when the fix/sat group is present.
        """
        payload = proto.decode_cbor(measures_payload)

        present_gps = set(payload.keys()) & proto.MEASURES_GPS_ALL

        if not present_gps:
            # No GPS fields at all is valid (device idle or GPS off)
            return

        # If any GPS field is present, the fix/sat group must be present
        missing_group = proto.MEASURES_GPS_FIX_GROUP - set(payload.keys())
        assert not missing_group, (
            f"GPS fields present ({present_gps}) but fix/sat group incomplete: "
            f"missing {missing_group}"
        )

        # Position fields should not appear without the fix/sat group
        present_position = set(payload.keys()) & proto.MEASURES_GPS_POSITION
        if present_position:
            assert proto.MEASURES_GPS_FIX_GROUP <= set(payload.keys()), (
                f"GPS position fields {present_position} present without "
                f"fix/sat group"
            )
