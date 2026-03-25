"""Tests for the Measures characteristic (notify-only, CBOR payload)."""

from __future__ import annotations

import pytest
from bleak import BleakClient

import ago_protocol as proto
from conftest import NotificationCollector


class TestMeasures:
    """Verify Measures notifications arrive and contain valid CBOR payloads."""

    async def test_receives_notification(
        self,
        measures_notifications: NotificationCollector,
        ago_notify_timeout: float,
    ):
        """At least one Measures notification must arrive within the timeout.

        The device sends notifications on each SensorDataReady event while
        connected. The measurement interval is typically 10-60s depending on
        settings, so we wait up to the configured timeout.
        """
        data = await measures_notifications.wait_for(timeout=ago_notify_timeout)
        assert len(data) > 0, "Received empty notification"

    async def test_cbor_decode(
        self,
        measures_notifications: NotificationCollector,
        ago_notify_timeout: float,
    ):
        """The notification payload must be valid CBOR that decodes to a map."""
        data = await measures_notifications.wait_for(timeout=ago_notify_timeout)
        ok, result = proto.decode_cbor_safe(data)
        assert ok, f"CBOR decode failed: {result}"
        assert isinstance(result, dict), f"Expected CBOR map, got {type(result).__name__}"

    async def test_timestamp_always_present(
        self,
        measures_notifications: NotificationCollector,
        ago_notify_timeout: float,
    ):
        """Every Measures notification must contain the 'ts' key (unix epoch)."""
        data = await measures_notifications.wait_for(timeout=ago_notify_timeout)
        payload = proto.decode_cbor(data)
        assert "ts" in payload, f"'ts' key missing from Measures payload: {payload.keys()}"
        assert isinstance(payload["ts"], int), (
            f"'ts' must be uint, got {type(payload['ts']).__name__}: {payload['ts']}"
        )

    async def test_known_keys_only(
        self,
        measures_notifications: NotificationCollector,
        ago_notify_timeout: float,
    ):
        """All keys in the Measures payload must be from the known set."""
        data = await measures_notifications.wait_for(timeout=ago_notify_timeout)
        payload = proto.decode_cbor(data)
        unknown = set(payload.keys()) - proto.MEASURES_ALL_KEYS
        assert not unknown, f"Unknown keys in Measures payload: {unknown}"

    async def test_field_types(
        self,
        measures_notifications: NotificationCollector,
        ago_notify_timeout: float,
    ):
        """Each field in the Measures payload must have the expected CBOR type."""
        data = await measures_notifications.wait_for(timeout=ago_notify_timeout)
        payload = proto.decode_cbor(data)

        for key, value in payload.items():
            expected_types = proto.MEASURES_FIELD_TYPES.get(key)
            if expected_types is None:
                continue  # unknown key checked in separate test
            assert isinstance(value, expected_types), (
                f"Measures['{key}']: expected {expected_types}, "
                f"got {type(value).__name__} = {value!r}"
            )

    async def test_gps_field_grouping(
        self,
        measures_notifications: NotificationCollector,
        ago_notify_timeout: float,
    ):
        """GPS fields must follow the grouping rules from the protocol spec.

        - If any GPS field is present, 'fix' and 'sat' must also be present.
        - 'lat', 'lon', 'alt' are individually conditional but only appear
          when the fix/sat group is present.
        """
        data = await measures_notifications.wait_for(timeout=ago_notify_timeout)
        payload = proto.decode_cbor(data)

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
