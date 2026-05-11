#!/usr/bin/env python3
"""Emit macro profile JSON (v1) from Python data — edit GROUPS then run:
   python tools/macro_profile_emit.py > profiles/my_profile.json
   Key codes are USB HID usage IDs (same as TinyUSB HID_KEY_* values)."""

from __future__ import annotations

import json
from typing import Any

PROFILE: dict[str, Any] = {
    "v": 1,
    "name": "emit-example",
    "groups": [
        {
            "steps": [
                {"us": 1_000_000, "kbd": {"m": 0, "k": []}},
                {"us": 1_000_000, "kbd": {"m": 0, "k": [90]}},  # KEYPAD_2
                {"us": 0, "kbd": {"m": 0, "k": [94]}},  # KEYPAD_6
                {"us": 10_000, "kbd": {"m": 0, "k": []}},
                {"us": 1_000_000, "kbd": {"m": 0, "k": [90]}},
                {"us": 10_000, "kbd": {"m": 0, "k": []}},
            ],
            "n": 6,
            "loop": False,
            "press": {"kbd": {"m": 0, "k": [91]}},  # KEYPAD_3
        },
    ],
}


def main() -> None:
    print(json.dumps(PROFILE, indent=2))


if __name__ == "__main__":
    main()
