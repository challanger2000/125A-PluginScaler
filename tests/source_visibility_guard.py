# Regression gate for the 2026-11-16 white-mirror incident.
# It is a SOURCE-LEVEL safeguard, NOT a substitute for a real screenshot test.
from pathlib import Path
import re
import sys

source = Path("src/vst3_window_scaler.cpp").read_text(encoding="utf-8")
forbidden = [
    r"kShieldClass", r"shield_proc", r"sync_shield", r"remove_shield",
    r"maskOriginal", r"shieldCheck", r"update_exclusions",
    r"\\bSW_HIDE\\b", r"\\bShowWindow\\s*\\(\\s*g\\.source",
    r"\\bSetWindowRgn\\s*\\(\\s*g\\.source"
]
def check(code):
    return [expr for expr in forbidden if re.search(expr, code, re.IGNORECASE)]

actual = check(source)
if actual:
    sys.exit("FAIL: Forbidden source-blocking code found: " + ", ".join(actual))
if not re.search(r"MagSetWindowFilterList\\s*\\(", source):
    sys.exit("FAIL: Magnifier capture filter is missing")
if not check(source + "\\nvoid sync_shield() {}"):
    sys.exit("FAIL: source guard does not catch the known mutation")
if not check(source + "\\nShowWindow(g.source, SW_HIDE);"):
    sys.exit("FAIL: source guard does not catch source hiding")
print("PASS: no source-masking code; injected mutations were detected")
