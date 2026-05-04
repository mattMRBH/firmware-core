"""Tests for the Config characteristic (read/write/notify, CBOR payload).

TestConfigRead uses a module-scoped fixture that reads Config once — all read
tests are synchronous validators of that shared payload.

TestConfigWrite and TestConfigCommand are async because they perform interactive
write/notify cycles that genuinely need per-test BLE I/O.
"""

from __future__ import annotations

import pytest_asyncio
from bleak import BleakClient

import ago_protocol as proto
from conftest import NotificationCollector


# ---------------------------------------------------------------------------
# Module-scoped fixture: read Config once
# ---------------------------------------------------------------------------

@pytest_asyncio.fixture(scope="module")
async def config_payload(ago_client: BleakClient) -> dict:
    """Read the Config characteristic once and return the decoded CBOR map."""
    data = await ago_client.read_gatt_char(proto.CHAR_CONFIG_UUID)
    return proto.decode_cbor(bytes(data))


# ---------------------------------------------------------------------------
# Read tests — pure data validation, no async
# ---------------------------------------------------------------------------

class TestConfigRead:
    """Verify reading the Config characteristic returns a valid 9-key map."""

    def test_read_config(self, config_payload: dict):
        """Reading Config must return valid CBOR map."""
        assert isinstance(config_payload, dict), (
            f"Expected CBOR map, got {type(config_payload).__name__}"
        )

    def test_all_keys_present(self, config_payload: dict):
        """Config read must contain exactly the 9 expected keys."""
        missing = proto.CONFIG_READ_KEYS - set(config_payload.keys())
        extra = set(config_payload.keys()) - proto.CONFIG_READ_KEYS
        assert not missing, f"Missing Config keys: {missing}"
        assert not extra, f"Unexpected Config keys: {extra}"

    def test_field_types(self, config_payload: dict):
        """Each Config field must have the expected type."""
        for key in proto.CONFIG_READ_KEYS:
            assert key in config_payload, f"Config key '{key}' missing"
            value = config_payload[key]
            expected_types = proto.CONFIG_FIELD_TYPES[key]
            assert isinstance(value, expected_types), (
                f"Config['{key}']: expected {expected_types}, "
                f"got {type(value).__name__} = {value!r}"
            )

    def test_gps_mode_valid(self, config_payload: dict):
        """'gps_mode' must be one of the known enum strings."""
        gps_mode = config_payload.get("gps_mode")
        assert gps_mode in proto.GPS_MODES, (
            f"Unknown gps_mode: '{gps_mode}'. Expected one of: {proto.GPS_MODES}"
        )

    def test_operating_mode_valid(self, config_payload: dict):
        """'op_mode' must be one of the known enum strings."""
        op_mode = config_payload.get("op_mode")
        assert op_mode in proto.OPERATING_MODES, (
            f"Unknown op_mode: '{op_mode}'. Expected one of: {proto.OPERATING_MODES}"
        )


# ---------------------------------------------------------------------------
# Write tests — async, interactive BLE I/O per test
# ---------------------------------------------------------------------------

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
        the 'type' discriminator plus all 9 config keys (10 total).
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

        # Must have all 10 keys (9 config + type)
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


# ---------------------------------------------------------------------------
# Command tests — async, interactive BLE I/O
# ---------------------------------------------------------------------------

class TestConfigCommand:
    """Verify command execution via Config writes."""

    async def test_command_progress_and_result_format(
        self,
        ago_client: BleakClient,
        config_notifications: NotificationCollector,
        ago_notify_timeout: float,
    ):
        """Writing a long-running command must trigger cmd_progress then cmd_result.

        We use 'co2_cal' as the test command. The device sends a cmd_progress
        notification immediately (acknowledging the command), followed by a
        cmd_result notification when calibration completes. The result may be
        success or failure (sensor may not support calibration), but both
        notification formats must be correct.
        """
        write_data = proto.encode_command("co2_cal")
        await ago_client.write_gatt_char(
            proto.CHAR_CONFIG_UUID, write_data, response=True,
        )

        # --- First notification: cmd_progress ---
        progress_data = await config_notifications.wait_for(timeout=ago_notify_timeout)
        progress = proto.decode_cbor(progress_data)

        assert progress.get("type") == "cmd_progress", (
            f"Expected type='cmd_progress', got '{progress.get('type')}'"
        )
        assert set(progress.keys()) == proto.CMD_PROGRESS_KEYS, (
            f"cmd_progress keys mismatch.\n"
            f"  Expected: {proto.CMD_PROGRESS_KEYS}\n"
            f"  Got:      {set(progress.keys())}"
        )
        assert progress["cmd"] == "co2_cal", (
            f"cmd mismatch: expected 'co2_cal', got '{progress['cmd']}'"
        )
        assert "ok" not in progress, "cmd_progress must not contain 'ok' key"
        assert "err" not in progress, "cmd_progress must not contain 'err' key"

        # --- Second notification: cmd_result ---
        result_data = await config_notifications.wait_for(timeout=ago_notify_timeout)
        payload = proto.decode_cbor(result_data)

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
