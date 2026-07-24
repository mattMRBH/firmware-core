"""Local Server fire-and-forget action tests."""

from __future__ import annotations

import httpx
import pytest

import ago_local_api as api


def test_led_action_is_not_supported(ago_http_client: httpx.Client) -> None:
    response = ago_http_client.post(api.TEST_LEDS_PATH)
    api.assert_error(response, 404, "not_found", "not found")


@pytest.mark.interactive
def test_calibrate_co2_is_dispatched(
    ago_http_client: httpx.Client,
    require_calibration_opt_in: None,
) -> None:
    del require_calibration_opt_in
    api.assert_empty_response(ago_http_client.post(api.CALIBRATE_CO2_PATH), 200)
