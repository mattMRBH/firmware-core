"""Tests for the Config characteristic (read/write/notify, CBOR payload)."""

from __future__ import annotations

import asyncio

import pytest
from bleak import BleakClient

import ago_protocol as proto
from conftest import NotificationCollector


class TestConfigRead:
    """Verify reading the Config characteristic returns a valid 12-key map."""

    async def _read_config(self, client: BleakClient) -> dict:
        data = await client.read_gatt_char(proto.CHAR_CONFIG_UUID)
        return proto.decode_cbor(bytes(data))

    async def test_read_config(self, ago_client: BleakClient):
        """Reading Config must return valid CBOR."""
        data = await ago_client.read_gatt_char(proto.CHAR_CONFIG_UUID)
        ok, result = proto.decode_cbor_safe(bytes(data))
        assert ok, f"CBOR decode failed: {result}"
        assert isinstance(result, dict), f"Expected CBOR map, got {type(result).__name__}"

    async def test_all_keys_present(self, ago_client: BleakClient):
        """Config read must contain exactly the 12 expected keys."""
        payload = await self._read_config(ago_client)
        missing = proto.CONFIG_READ_KEYS - set(payload.keys())
        extra = set(payload.keys()) - proto.CONFIG_READ_KEYS
        assert not missing, f"Missing Config keys: {missing}"
        assert not extra, f"Unexpected Config keys: {extra}"

    async def test_field_types(self, ago_client: BleakClient):
        """Each Config field must have the expected type."""
        payload = await self._read_config(ago_client)

        for key in proto.CONFIG_READ_KEYS:
            assert key in payload, f"Config key '{key}' missing"
            value = payload[key]
            expected_types = proto.CONFIG_FIELD_TYPES[key]
            assert isinstance(value, expected_types), (
                f"Config['{key}']: expected {expected_types}, "
                f"got {type(value).__name__} = {value!r}"
            )

    async def test_gps_mode_valid(self, ago_client: BleakClient):
        """'gps_mode' must be one of the known enum strings."""
        payload = await self._read_config(ago_client)
        gps_mode = payload.get("gps_mode")
        assert gps_mode in proto.GPS_MODES, (
            f"Unknown gps_mode: '{gps_mode}'. Expected one of: {proto.GPS_MODES}"
        )

    async def test_operating_mode_valid(self, ago_client: BleakClient):
        """'op_mode' must be one of the known enum strings."""
        payload = await self._read_config(ago_client)
        op_mode = payload.get("op_mode")
        assert op_mode in proto.OPERATING_MODES, (
            f"Unknown op_mode: '{op_mode}'. Expected one of: {proto.OPERATING_MODES}"
        )


class TestConfigWrite:
    """Verify writing config changes and receiving notifications."""

    async def test_set_config_notification(
        self,
        ago_client: BleakClient,
        config_notifications: NotificationCollector,
        ago_notify_timeout: float,
    ):
        """Writing a 'set' op must trigger a Config notification with type='config'.

        We toggle temp_f and then restore it. The notification must contain
        the 'type' discriminator plus all 12 config keys (13 total).
        """
        # Read current config to know original value
        raw = await ago_client.read_gatt_char(proto.CHAR_CONFIG_UUID)
        original = proto.decode_cbor(bytes(raw))
        original_temp_f = original["temp_f"]

        # Write the opposite value
        write_data = proto.encode_config_set(temp_f=not original_temp_f)
        await ago_client.write_gatt_char(
            proto.CHAR_CONFIG_UUID, write_data, response=True,
        )

        # Wait for the config-changed notification
        notif_data = await config_notifications.wait_for(timeout=ago_notify_timeout)
        payload = proto.decode_cbor(notif_data)

        assert payload.get("type") == "config", (
            f"Expected type='config', got '{payload.get('type')}'"
        )

        # Must have all 13 keys (12 config + type)
        assert set(payload.keys()) == proto.CONFIG_NOTIFY_KEYS, (
            f"Config notify keys mismatch.\n"
            f"  Expected: {proto.CONFIG_NOTIFY_KEYS}\n"
            f"  Got:      {set(payload.keys())}"
        )

        # The changed value should be reflected
        assert payload["temp_f"] == (not original_temp_f), (
            f"temp_f not updated in notification: "
            f"expected {not original_temp_f}, got {payload['temp_f']}"
        )

        # Restore original value
        restore_data = proto.encode_config_set(temp_f=original_temp_f)
        await ago_client.write_gatt_char(
            proto.CHAR_CONFIG_UUID, restore_data, response=True,
        )
        # Wait for the restore notification to clear the queue
        await config_notifications.wait_for(timeout=ago_notify_timeout)

    async def test_set_config_roundtrip(
        self,
        ago_client: BleakClient,
        config_notifications: NotificationCollector,
        ago_notify_timeout: float,
    ):
        """Setting a config value and re-reading must reflect the change."""
        # Read current config
        raw = await ago_client.read_gatt_char(proto.CHAR_CONFIG_UUID)
        original = proto.decode_cbor(bytes(raw))
        original_temp_f = original["temp_f"]
        new_value = not original_temp_f

        # Write new value
        write_data = proto.encode_config_set(temp_f=new_value)
        await ago_client.write_gatt_char(
            proto.CHAR_CONFIG_UUID, write_data, response=True,
        )

        # Consume the notification before re-reading
        await config_notifications.wait_for(timeout=ago_notify_timeout)

        # Re-read and verify
        raw2 = await ago_client.read_gatt_char(proto.CHAR_CONFIG_UUID)
        updated = proto.decode_cbor(bytes(raw2))
        assert updated["temp_f"] == new_value, (
            f"Config roundtrip failed: wrote temp_f={new_value}, "
            f"read back {updated['temp_f']}"
        )

        # Restore
        restore_data = proto.encode_config_set(temp_f=original_temp_f)
        await ago_client.write_gatt_char(
            proto.CHAR_CONFIG_UUID, restore_data, response=True,
        )
        await config_notifications.wait_for(timeout=ago_notify_timeout)

    async def test_config_notify_field_types(
        self,
        ago_client: BleakClient,
        config_notifications: NotificationCollector,
        ago_notify_timeout: float,
    ):
        """Config notification fields must have the correct types."""
        # Read and write back the same value to trigger a notification
        raw = await ago_client.read_gatt_char(proto.CHAR_CONFIG_UUID)
        original = proto.decode_cbor(bytes(raw))

        write_data = proto.encode_config_set(temp_f=original["temp_f"])
        await ago_client.write_gatt_char(
            proto.CHAR_CONFIG_UUID, write_data, response=True,
        )

        notif_data = await config_notifications.wait_for(timeout=ago_notify_timeout)
        payload = proto.decode_cbor(notif_data)

        for key in proto.CONFIG_NOTIFY_KEYS:
            assert key in payload, f"Config notify key '{key}' missing"
            value = payload[key]
            expected_types = proto.CONFIG_FIELD_TYPES[key]
            assert isinstance(value, expected_types), (
                f"Config notify['{key}']: expected {expected_types}, "
                f"got {type(value).__name__} = {value!r}"
            )


class TestConfigCommand:
    """Verify command execution via Config writes."""

    async def test_command_result_format(
        self,
        ago_client: BleakClient,
        config_notifications: NotificationCollector,
        ago_notify_timeout: float,
    ):
        """Writing a command must trigger a cmd_result notification.

        We use 'co2_cal' as the test command. The result may be success or
        failure (sensor may not be ready), but the notification format must
        be correct either way.
        """
        write_data = proto.encode_command("co2_cal")
        await ago_client.write_gatt_char(
            proto.CHAR_CONFIG_UUID, write_data, response=True,
        )

        notif_data = await config_notifications.wait_for(timeout=ago_notify_timeout)
        payload = proto.decode_cbor(notif_data)

        # Must have type discriminator
        assert payload.get("type") == "cmd_result", (
            f"Expected type='cmd_result', got '{payload.get('type')}'"
        )

        # Must have cmd and ok keys
        assert "cmd" in payload, f"'cmd' key missing from cmd_result: {payload}"
        assert "ok" in payload, f"'ok' key missing from cmd_result: {payload}"

        assert payload["cmd"] == "co2_cal", (
            f"cmd mismatch: expected 'co2_cal', got '{payload['cmd']}'"
        )
        assert isinstance(payload["ok"], bool), (
            f"'ok' must be bool, got {type(payload['ok']).__name__}"
        )

        # If failure, 'err' key may be present
        if not payload["ok"]:
            assert set(payload.keys()) <= proto.CMD_RESULT_KEYS_FAILURE, (
                f"Failure cmd_result has unexpected keys: "
                f"{set(payload.keys()) - proto.CMD_RESULT_KEYS_FAILURE}"
            )
            if "err" in payload:
                assert isinstance(payload["err"], str), (
                    f"'err' must be str, got {type(payload['err']).__name__}"
                )
        else:
            assert set(payload.keys()) == proto.CMD_RESULT_KEYS_SUCCESS, (
                f"Success cmd_result keys mismatch: "
                f"expected {proto.CMD_RESULT_KEYS_SUCCESS}, "
                f"got {set(payload.keys())}"
            )
