"""GET /api/v1/measures integration tests."""

from __future__ import annotations

import ago_local_api as api


def test_measures_schema(measures_payload: dict[str, object]) -> None:
    api.validate_measures(measures_payload)


def test_measures_identity(
    measures_payload: dict[str, object],
    ago_expected_serial: str | None,
) -> None:
    assert measures_payload["model"] == api.MODEL
    if ago_expected_serial is not None:
        assert measures_payload["serialNumber"] == ago_expected_serial


def test_measures_omit_invalid_values(measures_payload: dict[str, object]) -> None:
    assert all(value is not None for value in measures_payload.values())
    assert measures_payload.keys() <= api.MEASURES_KEYS
