"""mDNS discovery and advertised identity tests."""

from __future__ import annotations

import pytest

import ago_local_api as api


def test_mdns_profile(ago_service: api.DiscoveredService) -> None:
    if not ago_service.via_mdns:
        pytest.skip("mDNS assertions require discovery without --ago-url")

    properties = ago_service.properties
    assert properties is not None
    assert properties["vendor"] == "AirGradient"
    assert properties["model"] == api.MODEL
    assert properties["api"] == "1"
    assert properties["serialno"]
    assert properties["fw_ver"]
    assert ago_service.hostname == f"airgradient_{properties['serialno']}.local"
    assert 1 <= ago_service.port <= 65535


def test_mdns_identity_matches_measures(
    ago_service: api.DiscoveredService,
    measures_payload: dict[str, object],
) -> None:
    if not ago_service.via_mdns:
        pytest.skip("mDNS assertions require discovery without --ago-url")

    properties = ago_service.properties
    assert properties is not None
    assert properties["serialno"] == measures_payload["serialNumber"]
    assert properties["model"] == measures_payload["model"]
    assert properties["fw_ver"] == measures_payload["firmware"]
