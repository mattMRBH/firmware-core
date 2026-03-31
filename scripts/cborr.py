#!/usr/bin/env python3
"""Encode JSON to CBOR hex, or decode CBOR hex to JSON.

Usage:
    python cborr.py encode '{"op": "cmd", "cmd": "co2_cal"}'
    python cborr.py decode a2626f7063636d6463636d6467636f325f63616c
    python cborr.py decode 'A4 64 74 79 70 65 6A 63 6D 64 5F ...'
"""

import json
import re
import sys

import cbor2


def encode(json_str: str) -> str:
    """Encode a JSON string to CBOR, return hex."""
    obj = json.loads(json_str)
    return cbor2.dumps(obj).hex()


def _normalize_hex(raw: str) -> str:
    """Strip spaces, colons, '0x' prefixes, and newlines from a hex string."""
    return re.sub(r"[\s:]+|0x", "", raw, flags=re.IGNORECASE)


def decode(hex_str: str) -> str:
    """Decode a CBOR hex string to JSON.

    Accepts contiguous hex, space-separated bytes, colon-separated bytes,
    and mixed case (e.g. from BLE sniffers or serial monitors).
    """
    obj = cbor2.loads(bytes.fromhex(_normalize_hex(hex_str)))
    return json.dumps(obj, indent=2)


def main() -> None:
    if len(sys.argv) != 3:
        print(f"Usage: python {sys.argv[0]} {{encode|decode}} {{json|hexstring}}")
        sys.exit(1)

    command = sys.argv[1]
    value = sys.argv[2]

    if command == "encode":
        print(encode(value))
    elif command == "decode":
        print(decode(value))
    else:
        print(f"Unknown command: {command}")
        print(f"Usage: python {sys.argv[0]} {{encode|decode}} {{json|hexstring}}")
        sys.exit(1)


if __name__ == "__main__":
    main()
