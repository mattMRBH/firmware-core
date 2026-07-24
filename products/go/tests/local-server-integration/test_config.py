"""GET and PUT /api/v1/config integration tests."""

from __future__ import annotations

import httpx
import pytest

import ago_local_api as api


def _require_local_control(config: dict[str, object]) -> None:
    if config["configurationControl"] == "cloud":
        pytest.skip("local config writes are disabled by configurationControl=cloud")


def test_config_schema(config_payload: dict[str, object]) -> None:
    api.validate_config(config_payload)


def test_empty_config_is_accepted(
    ago_http_client: httpx.Client,
    config_payload: dict[str, object],
) -> None:
    _require_local_control(config_payload)
    api.assert_empty_response(ago_http_client.put(api.CONFIG_PATH, json={}), 202)


@pytest.mark.config_write
def test_temperature_unit_round_trip(
    ago_http_client: httpx.Client,
    preserved_safe_config: dict[str, object],
    ago_convergence_timeout: float,
) -> None:
    original = preserved_safe_config["temperatureUnit"]
    updated = "f" if original == "c" else "c"
    api.put_and_wait(
        ago_http_client,
        "temperatureUnit",
        updated,
        ago_convergence_timeout,
    )
