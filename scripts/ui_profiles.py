"""Resolve shared layout/theme/motion layers into a validated build-time UI contract."""
import copy
import hashlib
import json
from pathlib import Path

CATALOG = Path(__file__).resolve().parents[1] / "firmware/ui_profiles/catalog.json"
FIELDS = {
    "layout": {"family", "safeInset"},
    "tokens": {"background", "surface", "text", "accent", "gap", "radius", "fontScale", "touchMin"},
    "motion": {"fps", "transitionMs", "modalMs", "ease", "modalEase", "tier"},
}


def _merge(target, layer, allowed):
    if not isinstance(layer, dict) or set(layer) - allowed:
        raise ValueError("unknown UI section")
    for section, values in layer.items():
        if not isinstance(values, dict) or set(values) - FIELDS[section]:
            raise ValueError(f"unknown UI fields: {section}")
        target.setdefault(section, {}).update(values)


def _integer(values, key, minimum, maximum):
    value = values.get(key)
    if type(value) is not int or not minimum <= value <= maximum:
        raise ValueError(f"invalid UI {key}")


def resolve(board, catalog=None):
    catalog = json.loads(CATALOG.read_text()) if catalog is None else catalog
    if (not isinstance(catalog, dict) or catalog.get("schemaVersion") != 1
            or set(catalog) != {"schemaVersion", "defaults", "layouts", "themes", "motions", "devices"}):
        raise ValueError("invalid UI catalog schema")
    try:
        binding = catalog["devices"][board["id"]]
        if set(binding) != {"layout", "theme", "motion", "overrides"}:
            raise ValueError("invalid UI device binding")
        result = {}
        _merge(result, catalog["defaults"], set(FIELDS))
        _merge(result, catalog["layouts"][binding["layout"]], {"layout", "tokens"})
        _merge(result, catalog["themes"][binding["theme"]], {"tokens"})
        _merge(result, catalog["motions"][binding["motion"]], {"motion"})
        _merge(result, binding["overrides"], set(FIELDS))
    except (KeyError, TypeError) as exc:
        raise ValueError(f"missing or invalid UI binding for {board['id']}") from exc
    for section, fields in FIELDS.items():
        if set(result.get(section, {})) != fields:
            raise ValueError(f"incomplete UI {section}")
    layout, tokens, motion = (result[s] for s in FIELDS)
    if layout["family"] not in ("landscape", "portrait", "round", "large"):
        raise ValueError("invalid layout family")
    display = board["display"]
    shortest = min(display["width"], display["height"])
    _integer(layout, "safeInset", 0, (shortest - 1) // 2)
    if (layout["family"] == "round") != (display.get("shape") == "round"):
        raise ValueError("layout/display shape mismatch")
    for color in ("background", "surface", "text", "accent"):
        _integer(tokens, color, 0, 65535)
    for size in ("gap", "radius"):
        _integer(tokens, size, 0, shortest // 4)
    _integer(tokens, "fontScale", 1, 4)
    _integer(tokens, "touchMin", 16, shortest // 2)
    _integer(motion, "fps", 1, 60)
    for duration in ("transitionMs", "modalMs"):
        _integer(motion, duration, 0, 1000)
    if (any(motion[k] not in ("linear", "out", "in-out") for k in ("ease", "modalEase"))
            or motion["tier"] not in ("light", "balanced", "rich")):
        raise ValueError("invalid motion preset")
    if not board["psramBytes"] and (motion["tier"] != "light" or motion["fps"] > 30):
        raise ValueError("no-PSRAM boards require light motion at <=30 FPS")
    # Copy nested data: modifying one resolved board never mutates another or the catalog.
    return copy.deepcopy(result)


def c_values(board):
    result = resolve(board)
    values = {f"{section}_{key}".upper(): value
              for section, fields in result.items() for key, value in fields.items()}
    canonical = json.dumps(result, sort_keys=True, separators=(",", ":"))
    values["PROFILE_SHA256"] = hashlib.sha256(canonical.encode()).hexdigest()
    return {f"UI_{key}": value for key, value in values.items()}
