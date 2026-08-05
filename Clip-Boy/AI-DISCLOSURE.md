# AI & tooling transparency

Bryce designed the PCB and enclosure, defined the feature set, and made every
call about what ships and what gets cut. Everything was tested on physical
hardware (some of it gloriously flaky).

Tools used along the way:

- **Claude + Claude Code** — code review, PCB review, debugging partner
- **ESP32 Marauder** by justcallmekoko — base for the Wi-Fi/BLE security tooling,
  ported to Clip-Boy with significant AI-assisted refactoring
- **Gemini** — collectible artwork (prompts written and curated
  by Bryce)
- **Adobe Firefly** — image refinement and cleanup
- **ChatGPT** — a handful of static graphics, including Clippy
- **Suno** — instrumental music beds for the "SegFault-Tec FM" radio feature
  (commercial-tier license that grants ownership; original instrumental, never "in
  the style of" a named artist)
- **ElevenLabs** — the synthetic radio DJ voice (commercial tier; an owned,
  synthetic voice — never a clone of any real person)
- **Programmatic (sox / Python)** — radio tones, numbers-station digits, and static
  (fully original, no licensing)

AI was used as a tool, like Git or a multimeter. The decisions, the design, the
bugs, and the jokes are all Bryce's.

---

This disclosure is also surfaced on the badge itself (STATS → Settings → Legal →
*AI Disclosure*).
