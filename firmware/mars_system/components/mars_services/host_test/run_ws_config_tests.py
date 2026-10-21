#!/usr/bin/env python3
"""Compile real config loader and exported query; simulate app stacks on other threads."""
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
SERVICE = HERE.parent
source = (SERVICE / "src/mars_ws.c").read_text()
query = source[source.index("int mars_ws_saved_config("):source.index("static void enqueue(")]
# No NVS call may remain on the application's open/query path.
assert '#include "nvs.h"' not in source and 'nvs_get_' not in source and 'nvs_open(' not in source
with tempfile.TemporaryDirectory(prefix="mars-ws-config-") as temp:
    work = Path(temp)
    unit = work / "saved_config.c"
    unit.write_text('#include "mars_ws.h"\n#include "mars_ws_config.h"\n' + query)
    exe = work / "test"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I", str(HERE / "stub"),
                    "-I", str(SERVICE / "include"), "-I", str(SERVICE / "src"),
                    str(HERE / "test_ws_config.c"), str(SERVICE / "src/mars_ws_config.c"),
                    str(unit), "-lpthread", "-o", str(exe)], check=True)
    for scenario in range(8):
        subprocess.run([str(exe), str(scenario)], check=True)
