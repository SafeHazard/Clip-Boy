# AI Transparency — Clip-Boy

How we talk about AI use in Clip-Boy: what we say, where we say it, and why.

This is a *positioning* document, not a license or legal disclosure. The legal disclaimers live in the on-badge `Settings > Legal` screen and the website footer. This document covers the marketing/communication surface around AI use in the design and production of Clip-Boy.

## TL;DR

- Be specific about what AI did and what Bryce did. Vagueness reads as cover; specificity reads as competence.
- Lead with the human craft (PCB, enclosure, hardware drivers, product vision) before naming AI tools by name and role.
- Disclose proactively in three places: on-badge Credits, website About, and the Reddit launch post body. Don't disclose in the video itself.
- Avoid the term **"vibe coded"** — it's a tripwire term in 2025–26 hacker culture. Use **"AI-assisted"**.
- Bryce's prior delivered Kickstarter (DC33 Space Badge, 219% funded, 100% delivery, before AI was in his workflow) is the load-bearing trust signal. Reference it whenever AI is mentioned.

## Why this framing

### The DEF CON tolerance test isn't tool use — it's ownership

DEF CON's audience has used IDA, Ghidra, Burp, Metasploit, Wireshark for decades. They don't object to power tools. They object to people who don't understand what they shipped. The hostility around AI in 2025–26 is *specifically* at "vibe coders" — people who chained prompts until something ran and then sold it. It's not at engineers who used AI as a force multiplier and stayed in the driver's seat.

Specificity is the difference. *"Claude Code reviewed and audited the firmware; I made all hardware, PCB, BOM, and product decisions; 95 collectible artworks were generated with Gemini"* reads as a real engineer who knows what each tool did. Vague *"AI-powered"* reads as cover.

### Disclosure failure modes

| Failure mode | Outcome |
|---|---|
| AI use hidden, then discovered | Catastrophic. Every documented backlash (Terraforming Mars KS, Awaken Realms Puerto Rico, WotC) followed this pattern. |
| AI listed as a feature without specifics | Reads as marketing-speak. Audience smells it. |
| AI acknowledged but human craft buried | Reads as "AI did the work, human pressed go." |
| Human craft front-and-center, AI tools named with specific role | Wins. Industry standard for legitimate AI-assisted engineering. |

Kickstarter has required AI disclosure since August 2023; non-disclosure → suspension. So even if you wanted to hide it, you couldn't.

### Why the "Bryce shipped before AI" line is load-bearing

The single most important credibility move is anchoring AI as a *current tool* of an *already-shipping engineer*, not as the thing that made shipping possible. The DC33 Space Badge proves Bryce can build, ship, fulfill, and execute hardware before AI was in his workflow. Mentioning AI without that anchor invites the question *"did the AI build it?"*. Mentioning AI *with* that anchor reframes the question to *"how does he use AI now?"* — which is a friendly question, not a hostile one.

## Three pieces of disclosure copy

### A. On-badge `Settings > Credits` screen

Most authentic placement — this is the page DEF CON people will actually look at. Tone: matter-of-fact, no marketing fluff, hacker-respectful.

```
CREDITS

Clip-Boy is built and maintained by Bryce (17).

Hardware: custom PCB, enclosure, and full electronics
integration done by hand. Every driver — touch, display,
NeoPixels, I2S audio, VL53L5CX sensor — was hand-tuned
and debugged on real hardware.

AI tools used:
  Claude Code — code review, refactoring, and rubber-duck
    debugging across most of the firmware. Every suggestion
    was read, tested, and either kept, rewritten, or thrown
    out.
  Gemini — generated the 95 collectible artworks from
    prompts I wrote and curated.
  Adobe Firefly, ChatGPT — image cleanup and a few static
    graphics (including Clippy).
  Suno — instrumental music beds for the SegFault-Tec FM
    radio (commercial-tier, owned).
  ElevenLabs — synthetic radio DJ voices (owned; never a
    clone of a real person).
  Claude — copy editing on legal and marketing text.

Architecture, design choices, and the bugs are mine.

Inspired by Fallout. Not affiliated with Bethesda,
Microsoft, or Clippy.
```

**Design choices:**
- Hand-built hardware named *first* — the part skeptics will assume was AI is named first as human work, so the disclosure reads as confidence, not confession.
- "The bugs are mine" — hacker-credibility tell. Real engineers say this.
- AI tools named *by name* — Claude Code, Gemini. Specificity over hand-waving.

### B. Website `grab.brycebadges.com` "About the creator: Bryce"

Narrative paragraph. Tone: confident, specific, slightly dry. Should normalize AI use the way good carpenters normalize power tools.

```
Bryce is 17 and builds badges out of Texas. Clip-Boy is
his second hardware project; the first was a DEF CON 33
Kickstarter that funded at 219% and shipped 100% on time,
before he was using AI for much of anything.

He designed the PCB, modeled the enclosure, and wrote the
firmware that drives the touchscreen, NeoPixels, audio, and
LiDAR sensor. He uses Claude Code as a code reviewer and
debugging partner — roughly 70% of the firmware passed
through it at some point, and none of it shipped without
him reading, testing, and often rewriting it. The 95
collectible artworks were generated with Gemini from prompts
he wrote and curated, with Adobe Firefly and ChatGPT for
cleanup and a few static graphics. The SegFault-Tec
FM radio's instrumental music beds were made with Suno and its
synthetic DJ voices with ElevenLabs (both commercial-tier and
owned; the voices are synthetic, never clones of real people).
A few legal and marketing paragraphs were tightened with AI
copy editing.

Everything that touches the hardware was hand-tuned on real
silicon. Every design call — what goes on the badge, what
gets cut, what the jokes are — is his.

College fund, not a startup.
```

**Design choices:**
- Kickstarter line establishes "ships without AI" *before* AI is mentioned, so AI lands as a tooling choice, not a crutch.
- "70%" — concrete percentage reads as a real engineer who has measured, not a marketer making claims. **Adjust this number to whatever's actually accurate.**
- "Hand-tuned on real silicon" — the phrase real engineers use; signals competence.
- "College fund, not a startup" — disarms cynicism without begging.

### C. Reddit post body — pre-empt paragraph

For the bottom of the launch post. Designed to *pre-empt* the inevitable "did you just vibe code this" comment by naming the question first.

```
Pre-empting the obvious question: yes, I use AI. Claude Code
reviewed most of the firmware, Gemini generated the 95
collectible images from my prompts, and the radio's music and
DJ voices are Suno and ElevenLabs (commercial-tier, owned).
The PCB, the enclosure,
every hardware driver, and every design call are mine —
hand-tuned on real silicon. I don't ship code I can't
explain line by line, and AI doesn't fix bad architecture.
Ask me anything in the comments.
```

**Design choices:**
- Opens by *naming* the question instead of dodging it. Disarms the asker.
- Concrete tool names mean nobody has to dig.
- "I don't ship code I can't explain line by line, and AI doesn't fix bad architecture" — peer-to-peer signal that he understands what AI is and isn't good for. This sentence is the load-bearer.
- "Ask me anything" turns the comment section from interrogation into conversation.

## Placement summary

| Where | Disclosure level | Tone | Length |
|---|---|---|---|
| **On-badge `Settings > Credits`** | Most detailed | Engineering-log | ~150 words |
| **Website "About the creator"** | Narrative | Confident, slightly dry | ~180 words |
| **Reddit post body** | Concise + AMA invite | Peer-to-peer | ~70 words |
| **Promo video body** | None | — | (let the silicon-shots speak; the medium itself shows hand-craft) |
| **Promo video end card** | Optional one-frame factual line | Tiny | ~15 words |

Optional video end-card line, if used:

> *"AI tools: Claude Code (firmware), Gemini (art), Suno + ElevenLabs (radio). Hardware, design, and bugs by Bryce."*

## Words and phrases to avoid

| ❌ Avoid | ✅ Prefer | Why |
|---|---|---|
| "vibe coded" | "AI-assisted" | Tripwire term in 2025–26. "Vibe coded" specifically means *shipped without comprehension*. |
| "Made with ChatGPT" | "Reviewed/audited by Claude Code" | "Made with X" reads as the AI shipped it. "Reviewed by X" reads as a process step. |
| "AI-powered firmware" | "Custom firmware" | "AI-powered" suggests AI on the device. The badge has no AI on it. |
| "Revolutionary AI integration" | (delete) | Marketing-speak invites scrutiny. |
| "Leveraged AI to..." | "Used Claude Code to..." | Corporate-speak triggers the sniff test. |

## Things to NOT do

1. **Don't lead with AI** anywhere except the Reddit pre-empt. Lead with hardware. AI mentions are second-order.
2. **Don't list AI as a product feature.** The badge has no AI capability on-device. AI is in the workflow, not the BOM.
3. **Don't preach about AI ethics** in marketing copy. Stay matter-of-fact.
4. **Don't apologize.** Good engineers use power tools. State it; move on.
5. **Don't claim more than is true.** If 70% of the firmware passed through Claude Code, say 70%. If it's 40%, say 40%. Engineers will sniff out exaggeration faster than understatement.

## Things you might add later

- **Proactive blog post:** *"How a 17-year-old built a DEF CON badge with AI as a co-pilot"*. Risky if not written well. Strong if it is. Defer until after the launch lands well.
- **Open-sourcing parts of the code post-launch.** Demonstrates that the firmware is real and review-able, not an AI-generated black box. Strongest possible trust signal — but don't promise this until you're ready to follow through.
- **A "what AI got wrong" appendix** to the credits screen. One or two short anecdotes about AI suggestions that were wrong and how Bryce caught them. Massive credibility move because it shows him as the editor, not the typist.

## Reference: research notes

Key sources used in shaping these recommendations:

- **"Vibe coded" pejorative status:** [Hackaday: Ask Hackaday — Vibe Coding](https://hackaday.com/2025/04/09/ask-hackaday-vibe-coding/), [HN: Vibe Code is Legacy Code](https://news.ycombinator.com/item?id=47566491), [Karpathy/Osmani — vibe coding ≠ AI-assisted engineering](https://medium.com/@addyosmani/vibe-coding-is-not-the-same-as-ai-assisted-engineering-3f81088d5b98)
- **Disclosure-then-trust pattern:** [MIT Sloan: AI Disclosures Build Trust](https://sloanreview.mit.edu/article/artificial-intelligence-disclosures-are-key-to-customer-trust/), [Kickstarter AI disclosure policy (Aug 2023)](https://help.kickstarter.com/hc/en-us/articles/16848396410267)
- **AI-art-as-deception backlash:** [CBR: KS bans projects with concealed AI art](https://www.cbr.com/kickstarter-bans-project-involving-ai-generated-art/), [Board Game Wire: Awaken Realms / Ravensburger](https://boardgamewire.com/index.php/2024/03/02/awaken-realms-pulls-ai-art-from-deluxe-puerto-rico-kickstarter-after-ravensburger-steps-in/)
- **DEF CON cultural context:** No documented "AI-shaming" of an indie *hardware* project surfaced. The famous AI backlash incidents are all in tabletop / illustration spaces. Hardware communities have been more tolerant because AI-as-tool for code is normalized.
- **Comparable-product positioning data:** [Tiiny AI Pocket Lab](https://www.startuphub.ai/ai-news/startup-news/2026/tiiny-ai-pocket-lab-hits-1m-on-kickstarter), [Particle Tachyon](https://www.kickstarter.com/projects/particle-iot/tachyon-powerful-5g-single-board-computer-w-ai-accelerator), [Space Badge / DC33 / Owen](https://www.kickstarter.com/projects/o-n/space-badge-the-next-generation-of-defcon-badges)
