"""Operator-gated Local Server behavior during a committed foreground OTA."""

from __future__ import annotations

from collections.abc import Callable

import httpx
import pytest

import ago_local_api as api

pytestmark = pytest.mark.ota


def test_cached_gets_remain_available(
    ago_http_client: httpx.Client,
    require_ota_active: None,
) -> None:
    del require_ota_active
    api.assert_json_response(ago_http_client.get(api.MEASURES_PATH))
    api.assert_json_response(ago_http_client.get(api.CONFIG_PATH))


def test_valid_put_is_forbidden(
    ago_http_client: httpx.Client,
    require_ota_active: None,
) -> None:
    del require_ota_active
    api.assert_error(
        ago_http_client.put(api.CONFIG_PATH, json={}),
        403,
        "forbidden",
        "forbidden",
    )


def test_malformed_put_still_reports_invalid_body(
    ago_http_client: httpx.Client,
    require_ota_active: None,
) -> None:
    del require_ota_active
    response = ago_http_client.put(
        api.CONFIG_PATH,
        content=b"{",
        headers={"Content-Type": "application/json"},
    )
    api.assert_error(response, 400, "invalid_body", "invalid request body")


def test_led_action_is_forbidden(
    ago_http_client: httpx.Client,
    require_ota_active: None,
) -> None:
    del require_ota_active
    api.assert_error(
        ago_http_client.post(api.TEST_LEDS_PATH),
        403,
        "forbidden",
        "forbidden",
    )


@pytest.mark.interactive
def test_calibration_action_is_forbidden(
    ago_http_client: httpx.Client,
    require_ota_active: None,
    require_calibration_opt_in: None,
) -> None:
    del require_ota_active, require_calibration_opt_in
    api.assert_error(
        ago_http_client.post(api.CALIBRATE_CO2_PATH),
        403,
        "forbidden",
        "forbidden",
    )


def test_mdns_remains_advertised(
    require_ota_active: None,
    discover_local_server: Callable[[str | None], api.DiscoveredService],
    measures_payload: dict[str, object],
) -> None:
    del require_ota_active
    service = discover_local_server(str(measures_payload["serialNumber"]))
    assert service.properties is not None
    assert service.properties.get("api") == "1"
