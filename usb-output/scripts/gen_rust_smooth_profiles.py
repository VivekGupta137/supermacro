#!/usr/bin/env python3
"""Generate Rust v3 profiles from smoothing-example.txt weapon arrays."""

import json
import math
from pathlib import Path

# Reference tuning from WorkingRecoilV2_NoWiFi.ino defaults
SENSITIVITY = 0.78
ADS_SENSITIVITY = 1.0
FOV_REF = 90.0
PATTERN_GAIN = 2.2

WEAPONS = {
    "ak47": {
        "file": "rust-ak-smooth-v3.json",
        "prefix": "ak",
        "rx": [
            0, 0.194914926, 0.391704579, 0.511817958, 0.648471024, 0.764900073,
            0.80257069, 0.806110299, 0.907826148, 0.907167069, 0.869595948,
            0.901561429, 0.839903022, 0.902936872, 0.963032607, 0.874347588,
            0.825614937, 0.95416489, 0.914673816, 0.903444147, 0.883858707,
            0.943581636, 0.915063957, 0.857727099, 0.936576585, 0.91080919,
            0.936988434, 0.91679014, 0.936988434, 0.936576585,
        ],
        "ry": [
            -1.361448576, -1.387357074, -1.386754677, -1.362197502, -1.401340041,
            -1.336221468, -1.348643268, -1.337576409, -1.292502762, -1.346553873,
            -1.369026982, -1.329203754, -1.305987651, -1.313265861, -1.369361745,
            -1.293973479, -1.36908135, -1.385905402, -1.347744798, -1.349755803,
            -1.405980126, -1.352300463, -1.388740356, -1.333803438, -1.355569929,
            -1.378052181, -1.350748341, -1.339769017, -1.350748341, -1.355569929,
        ],
        "tbs_ms": 133.3,
        "holo_mul": 1.2,
        "x8_mul": 7.3,
        "automatic": True,
    },
    "lr300": {
        "file": "rust-lr300-smooth-v3.json",
        "prefix": "lr300",
        "rx": [
            0, 0.033517, -0.149077, -0.147054, 0.057723, 0.064947, 0.195907,
            -0.109226, 0.097927, -0.176122, 0.039039, -0.076399, -0.054613,
            0.039399, -0.119098, -0.130586, 0.025668, -0.039517, 0.052087,
            -0.058688, -0.12846, -0.010727, -0.027483, -0.032145, 0.033208,
            -0.0554, -0.020333, 0.160678, 0.047691, 0.020668,
        ],
        "ry": [
            -1.237727, -1.14431, -1.123523, -1.169826, -1.185951, -1.136969,
            -1.173873, -1.133846, -1.097525, -1.202949, -1.107805, -1.138392,
            -1.105178, -1.09091, -1.02865, -1.076927, -1.090242, -1.059847,
            -1.148118, -1.187485, -1.115724, -1.043713, -1.075563, -1.137415,
            -1.081225, -1.159009, -1.083084, -1.202353, -1.158462, -1.158462,
        ],
        "tbs_ms": 120.0,
        "holo_mul": 1.2,
        "x8_mul": 6.9,
        "automatic": True,
    },
    "mp5": {
        "file": "rust-mp5-smooth-v3.json",
        "prefix": "mp5",
        "rx": [
            0.075593, -0.060027, 0.016778, -0.00827, -0.004792, 0.057946,
            -0.0277, 0.020693, -0.002393, -0.00567, 0.08491, -0.009076, 0.057706,
            -0.162595, 0.000172, 0.011103, 0.059812, 0.120394, -0.049844,
            0.004116, 0.055301, -0.065532, 0.102276, -0.023061, -0.051564,
            0.082176, 0.118424, -0.091965, -0.12935, 0.020668,
        ],
        "ry": [
            -0.634625, -0.561723, -0.575319, -0.513457, -0.645559, -0.613864,
            -0.479012, -0.670909, -0.560814, -0.535767, -0.585397, -0.63107,
            -0.518866, -0.626454, -0.506808, -0.625991, -0.513576, -0.538504,
            -0.644775, -0.53154, -0.694026, -0.582204, -0.662998, -0.675411,
            -0.528804, -0.631696, -0.627106, -0.729202, -0.576859, -0.576859,
        ],
        "tbs_ms": 100.0,
        "holo_mul": 1.2,
        "x8_mul": 6.9,
        "automatic": True,
    },
    "thompson": {
        "file": "rust-thompson-smooth-v3.json",
        "prefix": "thompson",
        "rx": [
            -0.069091, 0.005238, 0.006218, 0.03909, 0.062757, -0.053135,
            0.054213, 0.022354, 0.107614, 0.020906, -0.049843, 0.015407,
            0.049696, -0.074353, 0.016983, -0.07076, -0.162073, -0.032011,
            0.025555, 0.025555,
        ],
        "ry": [
            -0.410423, -0.407989, -0.411751, -0.416881, -0.395338, -0.398239,
            -0.407135, -0.381472, -0.382746, -0.403674, -0.4009, -0.383888,
            -0.390212, -0.399249, -0.399399, -0.418165, -0.398657, -0.408528,
            -0.390164, -0.390164,
        ],
        "tbs_ms": 130.0,
        "holo_mul": 1.48,
        "x8_mul": 8.4,
        "automatic": True,
    },
    "sar": {
        "file": "rust-sar-smooth-v3.json",
        "prefix": "sar",
        "rx": [0, 0],
        "ry": [-0.90, -0.90],
        "tbs_ms": 175.0,
        "holo_mul": 1.2,
        "x8_mul": 7.35,
        "automatic": False,
    },
    "m39": {
        "file": "rust-m39-smooth-v3.json",
        "prefix": "m39",
        "rx": [0.54, 0.54],
        "ry": [-0.95, -0.95],
        "tbs_ms": 150.0,
        "holo_mul": 1.6,
        "x8_mul": 9.7,
        "automatic": False,
    },
    "customsmg": {
        "file": "rust-customsmg-smooth-v3.json",
        "prefix": "customsmg",
        "rx": [
            -0.069, 0.005, 0.006, 0.039, 0.062, -0.053, 0.054, 0.022, 0.107,
            0.020, -0.049, 0.015, 0.049, -0.074, 0.017, -0.070, -0.162, -0.032,
            0.002, 0.008, -0.006, 0.026, -0.026, 0.002,
        ],
        "ry": [
            -0.410, -0.408, -0.411, -0.416, -0.395, -0.398, -0.407, -0.381,
            -0.382, -0.403, -0.400, -0.383, -0.390, -0.399, -0.399, -0.418,
            -0.398, -0.408, -0.390, -0.332, -0.332, -0.348, -0.331, -0.331,
        ],
        "tbs_ms": 100.0,
        "holo_mul": 1.5,
        "x8_mul": 7.95,
        "automatic": True,
    },
    "hmlmg": {
        "file": "rust-hmlmg-smooth-v3.json",
        "prefix": "hmlmg",
        "rx": [0, -0.536] + [-0.556] * 58,
        "ry": [-1.047] * 60,
        "tbs_ms": 125.0,
        "holo_mul": 1.2,
        "x8_mul": 7.2,
        "automatic": True,
    },
    "m249": {
        "file": "rust-m249-smooth-v3.json",
        "prefix": "m249",
        "rx": [0, 0.393] + [0.525] * 93,
        "ry": [-0.81, -1.08] + [-1.08] * 93,
        "tbs_ms": 120.0,
        "holo_mul": 1.175,
        "x8_mul": 6.95,
        "automatic": True,
    },
    "python": {
        "file": "rust-python-smooth-v3.json",
        "prefix": "python",
        "rx": [0, 0],
        "ry": [-3.5, -3.5],
        "tbs_ms": 150.0,
        "holo_mul": 1.6,
        "x8_mul": 9.75,
        "automatic": False,
    },
}

VARIANTS = [
    ("base", "scope_base", 1.0),
    ("ho", "scope_holo", 1.0),
    ("ls", "scope_x8", 1.0),
    ("ho-ls", "scope_holo", 0.8),
]

PROFILE_HEADER = {
    "v": 3,
    "game": "rust",
    "customprops": {"fov": 70, "ads-sens": 1},
    "activeWeapon": 0,
    "eDPI": 640,
    "patternEDPI": 624,
    "humanize": {
        "timingPct": 0,
        "jitterUs": [0, 0],
        "mouse": 0,
        "dripMs": 4,
    },
    "macrosOn": True,
    "nextWeapon": {"mouse": {"b": 0}},
    "toggleMacros": {"mouse": {"b": 6}},
}


def shot_scale(scope_mult: float) -> float:
    move_mult = (
        0.03 * SENSITIVITY * ADS_SENSITIVITY * scope_mult * 3.6 * (FOV_REF / 100.0)
    )
    if move_mult < 1e-4:
        move_mult = 1e-4
    return PATTERN_GAIN / move_mult


def scope_mult_for(w: dict, variant_kind: str) -> float:
    if variant_kind == "scope_base":
        return 1.0
    if variant_kind == "scope_holo":
        return w["holo_mul"]
    if variant_kind == "scope_x8":
        return w["x8_mul"]
    return 1.0


def build_steps(w: dict, scope: float, extra_scale: float) -> list:
    scale = shot_scale(scope) * extra_scale
    us = int(round(w["tbs_ms"] * 1000))
    steps = []
    for rx, ry in zip(w["rx"], w["ry"]):
        x = int(round((-rx) * scale))
        y = int(round((-ry) * scale))
        steps.append({"us": us, "mouse": {"x": x, "y": y}})
    return steps


def build_mode(w: dict, steps: list) -> dict:
    mode = {
        "comment": "ADS LMB+RMB (REQUIRE_RIGHT_HELD)",
        "name": "ads",
        "modeScale": 1.0,
        "press": {"mouse": {"b": 3}},
        "n": len(steps),
        "steps": steps,
    }
    if w["automatic"]:
        mode["loop"] = True
    else:
        mode["triggerMode"] = "tap"
        mode["loop"] = False
    return mode


def build_profile(w: dict) -> dict:
    profile = dict(PROFILE_HEADER)
    profile["name"] = w["file"].replace(".json", "")
    weapons = []
    for suffix, scope_kind, extra in VARIANTS:
        scope = scope_mult_for(w, scope_kind)
        steps = build_steps(w, scope, extra)
        weapons.append(
            {
                "name": f"{w['prefix']}-{suffix}",
                "scale": 1.0,
                "modes": [build_mode(w, steps)],
            }
        )
    profile["weapons"] = weapons
    return profile


def main() -> None:
    out_dir = Path(__file__).resolve().parents[1] / "profiles"
    out_dir.mkdir(parents=True, exist_ok=True)
    for w in WEAPONS.values():
        path = out_dir / w["file"]
        profile = build_profile(w)
        with path.open("w", encoding="utf-8", newline="\n") as f:
            json.dump(profile, f, indent=2)
            f.write("\n")
        print(f"wrote {path} ({len(profile['weapons'])} weapons)")


if __name__ == "__main__":
    main()
