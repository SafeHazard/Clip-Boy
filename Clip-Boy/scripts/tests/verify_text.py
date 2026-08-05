#!/usr/bin/env python3
"""verify_text.py — confirm each new theme APPLIES and the UI renders content
(via the lightweight 'text' command; screenshot crashes on this build). Live-
switches themes and dumps visible text so we can see the Tools list rendered."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Harness

THEMES = [(3, "Overseer"), (4, "Space Badge"), (0, "Mojave")]

def main():
    h = Harness(port="COM8")
    try:
        for _ in range(15):
            h.skip_boot()
            s = h.state()
            if s.get("ok") and not s.get("boot_visible"):
                break
            h.wait(900)
        h.nav(1, 0); h.wait(800)   # ITEMS > Tools
        for idx, name in THEMES:
            r = h.theme_set(idx)
            h.wait(1500)
            t = h.text()
            texts = t.get("texts") or t.get("text") or t
            # show a few visible strings to confirm the screen rendered
            sample = texts[:8] if isinstance(texts, list) else texts
            st = h.state()
            print(f"theme {idx} {name}: set_ok={r.get('ok')} changed={r.get('changed')} "
                  f"responsive={st.get('ok')} tab={st.get('tab_name')}")
            print(f"    visible: {sample}")
    finally:
        h.close()

if __name__ == "__main__":
    main()
