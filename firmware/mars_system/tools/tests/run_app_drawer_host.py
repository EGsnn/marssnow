#!/usr/bin/env python3
"""No SDK or device required. Compile real source against display/slot mocks."""
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
UI = ROOT / "components/mars_ui"
COMMON = ROOT / "components/mars_common"
APPS = ROOT / "main/apps"
CJSON = ROOT.parents[1] / "third_party/components/espressif__cjson/cJSON"


def check_defaults():
    source = (UI / "ui_runtime.c").read_text().split("static const char *DEFAULT_DOCUMENT =", 1)[1].split("static char s_line", 1)[0]
    dock = "\n".join(line.strip().removesuffix(" \\") for line in (UI / "include/desktop_dock.h").read_text().splitlines() if line.strip().startswith('"'))
    source = source.replace("    MARS_NATIVE_DOCK_ELEMENTS", dock)
    for legacy in (False, True):
        active = True
        pieces = []
        for line in source.splitlines():
            if line.startswith("#ifndef CONFIG_BOARD_TYPE_MARS_ATK_BOX"):
                active = not legacy
            elif line.startswith("#else"):
                active = not active
            elif line.startswith("#endif"):
                active = True
            elif active and line.strip().startswith('"'):
                literal = line.strip().removesuffix(";")
                pieces.append(json.loads(literal))
        doc = json.loads("".join(pieces))
        elements = doc["pages"][0]["elements"]
        buttons = [e for e in elements if e["type"] == "button"]
        assert len(buttons) == (5 if legacy else 3)
        if not legacy:
            assert [e["onTap"]["actionId"] for e in buttons].count("app.apps.open") == 1
            assert not any(e["onTap"]["actionId"] in ("app.xiaozhi.open", "app.dyn.0.open") for e in buttons)
            assert buttons[0]["text"] == "APP"
            assert [e["onTap"]["actionId"] for e in buttons] == ["app.apps.open", "app.fluid_clock.open", "app.system.open"]
            assert buttons[1]["icon"] == "clock" and buttons[1]["iconSize"] >= 20
        for a, b in zip(buttons, buttons[1:]):
            assert a["x"] + a["width"] <= b["x"], "dock buttons overlap"
    print("PASS: native and legacy built-in desktop JSON and dock hit areas")


check_defaults()
with tempfile.TemporaryDirectory(prefix="mars-drawer-host-") as temp:
    executable = Path(temp) / "drawer_test"
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I" + str(ROOT / "components/mars_ui_adapt/include"), "-I" + str(APPS), "-I" + str(COMMON / "include"),
        "-I" + str(ROOT / "components/mars_app_loader/include"),
        "-I" + str(UI / "include"), "-I" + str(CJSON),
        str(Path(__file__).with_name("app_drawer_host.c")),
        str(APPS / "app_drawer.c"), str(COMMON / "anim.c"),
        str(UI / "desktop_apps_json.c"), str(CJSON / "cJSON.c"),
        "-lm", "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
