"""Temporarily add a keep-open hook to the *checkout copy* of Test Lab.
The source repository is never changed. Fail if the reviewed anchor drifts."""
import argparse
from pathlib import Path

p=argparse.ArgumentParser()
p.add_argument("source",type=Path)
args=p.parse_args()
file=args.source
content=file.read_text(encoding="utf-8")
anchor="""        ShowWindow(window.get(), SW_SHOWNA);
        UpdateWindow(window.get());
        pump(75);
"""
if content.count(anchor)!=1:
    raise SystemExit(f"Expected exactly one Test Lab runView GUI show anchor, got {content.count(anchor)}")
replacement=anchor+"""        if (const char* hold = std::getenv("125A_SCALER_HOLD_MS")) {
            const int ms = std::clamp(std::atoi(hold), 0, 30000);
            std::cout << "125A_SCALER_REAL_EDITOR_READY\\\\n" << std::flush;
            pump(static_cast<DWORD>(ms));
        }
"""
content=content.replace(anchor,replacement)
content='#include <cstdlib>\\n'+content
file.write_text(content,encoding="utf-8")
print("Scoped Test Lab GUI hold hook installed in temporary checkout")
