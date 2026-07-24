"""Local Server parsing, validation, and provider error tests."""

from __future__ import annotations

import httpx
import pytest

import ago_local_api as api

JSON_HEADERS = {"Content-Type": "application/json"}


@pytest.mark.parametrize("body", [b"", b"{", b"[]", b"{} trailing"])
def test_invalid_bodies(ago_http_client: httpx.Client, body: bytes) -> None:
    response = ago_http_client.put(api.CONFIG_PATH, content=body, headers=JSON_HEADERS)
    api.assert_error(response, 400, "invalid_body", "invalid request body")


def test_unknown_field(ago_http_client: httpx.Client) -> None:
    response = ago_http_client.put(api.CONFIG_PATH, json={"temperatureUnits": "c"})
    api.assert_error(
        response,
        400,
        "unknown_field",
        "unknown field",
        field="temperatureUnits",
    )


def test_invalid_enum(ago_http_client: httpx.Client) -> None:
    response = ago_http_client.put(api.CONFIG_PATH, json={"temperatureUnit": "kelvin"})
    api.assert_error(
        response,
        400,
        "invalid_value",
        "invalid value",
        field="temperatureUnit",
    )


def test_unknown_nested_correction_field(ago_http_client: httpx.Client) -> None:
    response = ago_http_client.put(
        api.CONFIG_PATH,
        json={"corrections": {"temp": {"bogus": 1}}},
    )
    api.assert_error(
        response,
        400,
        "unknown_field",
        "unknown field",
        field="corrections.temp.bogus",
    )


def test_known_unsupported_field(
    ago_http_client: httpx.Client,
    config_payload: dict[str, object],
) -> None:
    if config_payload["configurationControl"] == "cloud":
        pytest.skip("configuration source policy takes precedence over product support")
    response = ago_http_client.put(api.CONFIG_PATH, json={"country": "US"})
    api.assert_error(response, 404, "not_found", "not found", field="country")
