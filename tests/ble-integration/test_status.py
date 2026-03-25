"""Tests for the Status characteristic (read-only, CBOR payload)."""

from __future__ import annotations

import pytest
from bleak import BleakClient

import ago_protocol as proto


class TestStatus:
    """Verify the Status characteristic returns a valid 10-key CBOR map."""

    async def _read_status(self, client: BleakClient) -> dict:
        data = await client.read_gatt_char(proto.CHAR_STATUS_UUID)
        return proto.decode_cbor(bytes(data))

    async def test_read_status(self, ago_client: BleakClient):
        """Reading the Status characteristic must return valid CBOR."""
        data = await ago_client.read_gatt_char(proto.CHAR_STATUS_UUID)
        ok, result = proto.decode_cbor_safe(bytes(data))
        assert ok, f"CBOR decode failed: {result}"
        assert isinstance(result, dict), f"Expected CBOR map, got {type(result).__name__}"

    async def test_all_keys_present(self, ago_client: BleakClient):
        """The Status payload must contain exactly the 10 expected keys."""
        payload = await self._read_status(ago_client)
        missing = proto.STATUS_ALL_KEYS - set(payload.keys())
        extra = set(payload.keys()) - proto.STATUS_ALL_KEYS
        assert not missing, f"Missing Status keys: {missing}"
        assert not extra, f"Unexpected Status keys: {extra}"

    async def test_field_types(self, ago_client: BleakClient):
        """Each Status field must have the expected type."""
        payload = await self._read_status(ago_client)

        for key, expected_types in proto.STATUS_FIELD_TYPES.items():
            assert key in payload, f"Status key '{key}' missing"
            value = payload[key]
            assert isinstance(value, expected_types), (
                f"Status['{key}']: expected {expected_types}, "
                f"got {type(value).__name__} = {value!r}"
            )

    async def test_charging_state_valid(self, ago_client: BleakClient):
        """The 'charging' field must be one of the known charging state strings."""
        payload = await self._read_status(ago_client)
        charging = payload.get("charging")
        assert charging in proto.CHARGING_STATES, (
            f"Unknown charging state: '{charging}'. "
            f"Expected one of: {proto.CHARGING_STATES}"
        )

    async def test_battery_percentage_range(self, ago_client: BleakClient):
        """The 'bat_pct' field must be in 0-100 range."""
        payload = await self._read_status(ago_client)
        bat_pct = payload.get("bat_pct")
        assert isinstance(bat_pct, int), f"bat_pct is not int: {bat_pct!r}"
        assert 0 <= bat_pct <= 100, f"bat_pct out of range: {bat_pct}"

    async def test_battery_voltage_non_negative(self, ago_client: BleakClient):
        """The 'bat_v' field must be non-negative (clamped to 0.0 by firmware)."""
        payload = await self._read_status(ago_client)
        bat_v = payload.get("bat_v")
        assert isinstance(bat_v, (int, float)), f"bat_v is not numeric: {bat_v!r}"
        assert bat_v >= 0, f"bat_v is negative: {bat_v}"

    async def test_firmware_version_format(self, ago_client: BleakClient):
        """The 'fw' field must be a non-empty string."""
        payload = await self._read_status(ago_client)
        fw = payload.get("fw")
        assert isinstance(fw, str), f"fw is not str: {fw!r}"
        assert len(fw) > 0, "fw is empty string"
