# Acceptable Use — CANONICAL source of truth

**Version:** v0.2 · **Status:** counsel-reviewed 2026-06-18 (Jira **DC34-99**);
adopts counsel's approved wording (indemnity expanded per counsel's note,
owner-approved). · **This is not legal advice.**

> **Naming note:** counsel's returned draft referred to **Sn33k-Boy / L33t-Boy**
> (the pre-rename build names). This canonical uses the current product names
> **Sn34k-Boy / Res34rch-Boy** (rename finalized 2026-06-15) — a factual product-
> identifier swap, not a substantive change. Worth a one-line confirm to counsel.

This file is the single source of truth for Clip-Boy's acceptable-use / disclaimer
language. Every customer-facing surface — the **storefront**, the **web flasher**,
and the **badge firmware** (first-boot clickwrap + Tools banner + Legal page) —
renders the **same policy** at a length that fits the surface. When the policy
changes, bump the version here and re-sync each surface (see the sync table).
Reconciled from: Bryce's original draft + Tomas's edits, the `web_flash` and
`shop` agent drafts, and the ui_test 3-lens review panel (legal / firmware-UX /
web-flash-commerce).

> **Framing constraint (GPLv3 §7 — load-bearing, confirm with counsel):** this is
> an **assumption-of-risk / liability notice and a term of sale**, NOT a software
> license condition. Clip-Boy firmware is GPLv3; §7 forbids adding field-of-use
> restrictions, and such a restriction would be void and conflict with the GPL.
> Never reword any of this into "you are licensed to use this software ONLY IF…".
> "By using this you accept responsibility / consent to these terms" is fine;
> "you may only use this software for X" is not.

**Entities:** Sold by **Coruscant Productions LLC**. Surfaces:
`get`/`ks.brycebadges.com` (storefront), `flash.brycebadges.com` (web flasher),
on-device firmware.

---

## 1. Canonical core (the spine every surface derives from)

All products sold by **Coruscant Productions LLC** are intended for lawful,
ethical, and educational use only. Clip-Boy is a wearable ESP32-S3 badge for
**wireless analysis, detection, learning, and tinkering**. As shipped it operates
in a **passive, listen-only posture** (the default **Sn34k-Boy** firmware). A
separate research build (**Res34rch-Boy**) with additional, more active capabilities
can be loaded by the user; those capabilities are for **legitimate security
research and authorized testing only**. It is sold as an educational and research
tool — **not a weapon and not a service**.

It is the **sole responsibility of the user** (not only the purchaser — this binds
anyone who downloads, flashes, installs, or uses it, including secondhand or
gifted units) to comply with **all applicable laws and regulations — including but
not limited to local, state, federal, national, and radio-frequency / spectrum
rules** — when using these products. In the United States that includes, among
others, the Computer Fraud and Abuse Act (18 U.S.C. § 1030), the Wiretap Act and
related interception statutes (18 U.S.C. §§ 2511–2512), 47 U.S.C. § 333 (harmful
interference), and FCC rules — plus state and local equivalents and the laws of
your own country. Laws differ by place; knowing the ones that apply to you is your
responsibility, not ours.

By using Clip-Boy you agree that:

- You will use it **only on networks, devices, and radio environments you own, or
  that you have explicit, current authorization and consent to test.** Other people's devices
  and networks are off-limits without that permission — at a conference, a coffee
  shop, your building, anywhere.
- You will **not** use it to **disrupt, intercept, impersonate, or interfere
  with** communications or systems you are not authorized to touch.
- If you choose to load a research build with active capabilities, **you take on
  full responsibility for everything it can do** and for using it lawfully and
  with authorization.
- Where you are has its own rules (an event, venue, workplace, campus) — you will
  follow those too.

**Downloading, flashing, installing, or using** Clip-Boy, its firmware, and
derivative works **constitutes consent** to the above and acceptance of full
responsibility for their use.

Coruscant Productions LLC does not condone or support any unlawful activity and,
**to the fullest extent permitted by law, disclaims liability** for misuse or
unlawful application of the software, firmware, or hardware sold. Clip-Boy is
provided **"as is," without warranty of any kind**, to the maximum extent
permitted by law; nothing here removes consumer rights that your local law says
cannot be waived.

**Indemnification.** To the fullest extent
permitted by law, you agree to **indemnify, defend, and hold harmless** Coruscant
Productions, LLC and its members, managers, officers, employees, and agents from
and against any and all claims, demands, actions, liabilities, damages, losses,
costs, and expenses — **including reasonable attorneys' fees and court costs** —
arising out of or relating to (a) your use, misuse, or modification of Clip-Boy,
its firmware, or software; (b) your breach of these terms; or (c) your violation
of any applicable law or the rights of any third party.

*Open-source credit:* wireless tooling builds on the **ESP32 Marauder** project by
**JustCallMeKoko (MIT)**. Clip-Boy's firmware is **GPLv3**.

---

## 2. Per-surface renderings

> Public surfaces (storefront, web flasher) keep the active-capability language
> **generic** — no enumerated tool list — so the copy doesn't read like a how-to
> or trip keyword classifiers (shop-agent rationale). The **on-device** Res34rch
> acknowledgment may be slightly more concrete (it's past the public-classifier
> concern) but stays restrained.

### 2a. Storefront — product page + checkout (`brycebadges.com`)
Product-page disclaimer (plain/serious, even though the rest of the store is
jokey) + a required **at-checkout tick**: *"I understand Clip-Boy is sold for
lawful security research and education only, as-is, with no warranty; I'm
responsible for how I use it and for following the laws where I am. I am 18+ or
have a parent/guardian's permission."* Seller of record = **Coruscant Productions
LLC** (never an individual). Ships hardware only; **no shipment to OFAC-embargoed
destinations/parties.**

### 2b. Web flasher — `flash.brycebadges.com` (`RES34RCH_ACK` in `app.js`)
**Sn34k-Boy** is the prominent default (single informed ack). **Res34rch-Boy** is a
secondary "advanced / active research" path behind one extra step, with a stronger
acknowledgment: that this build **transmits** and can disrupt/intercept; that it's
for authorized testing only; and — honestly — that **"this consent click is a
record of your agreement, not a safety feature; it does not limit what the
firmware can do."** No fake friction (CAPTCHA/email/ID) — it's theater and the
binaries are public anyway (see §3).

### 2c. Firmware — first-boot clickwrap (the load-bearing on-device consent)
Fixed title / **scrollable body** / fixed Accept button. The **Accept button is
disabled (dimmed) until the body is scrolled to the end** ("scroll-to-enable"),
then arms — this is the single highest-value enforceability hardening. Persist the
acceptance to NVS with a **policy version** + timestamp so re-flashing updated
terms re-prompts. **Two variants by SKU define:**
- **Sn34k-Boy:** passive framing; no active-tool wording (the tools aren't there).
- **Res34rch-Boy:** an explicit "this build can **transmit** — use only on targets you
  own or are authorized in writing to test" block, and a confirmatory button label
  (e.g. *"I Accept — Authorized Targets Only"*). Scroll-to-enable is **mandatory**
  here.

### 2d. Firmware — Tools banner + Legal page
- Banner (one line, by SKU): Sn34k `Authorized research only - lawful use is on you.`
  / Res34rch `Active TX tools - authorized targets ONLY.`
- Legal page (Help): an **Acceptable-Use section at the top** stating the same rule
  in the **same words** as every other layer; dedupe the existing
  misuse/warranty bullets so all layers are identical (the sameness is what proves
  it's one policy). Fallout-tone jokes stay only here (acceptance already happened).

---

## 3. Sync table — keep these identical to §1

| Surface | Where the string lives | Owner |
|---|---|---|
| Storefront disclaimer + checkout tick | `documents/shop/` → store CMS | shop |
| Web flasher Res34rch ack | `web_flash/app.js` → `RES34RCH_ACK` | web_flash |
| Firmware clickwrap / banner / Legal page | `ui_test/ui_nav.h` (SKU-`#ifdef`) | ui_test |
| This canonical doc | `ui_test/acceptable_use.md` | ui_test |

**Also post `acceptable_use.md` + `LICENSE` next to the public binaries** (the
GitHub release/repo). Because the binaries are publicly `curl`-able, putting the
terms beside them is the real mitigation — it converts "they snuck around the gate"
into "the terms were posted right there." The on-device clickwrap remains the only
consent step present on every path (web flash, curl + manual flash, re-flash).

---

## 4. Open items / recommendations (for the team + counsel)

- **DC34-99 item 7 (counsel):** if counsel provides exact disclaimer wording, it
  **overrides §1's warranty/liability language verbatim**. Confirm the GPLv3-§7
  framing above (notice, not license condition).
- **DC34-98 (counsel/LLC hygiene):** the load-bearing protection for a
  minor-built product is the **LLC being the contracting party on every surface**,
  with an adult member as authorized signatory and no commingling. Keep Bryce off
  the contract/warranty surface. ~30-min one-time check.
- **Flash-path disclosure (open, cross-team):** the store does **not** link the
  flasher — decide the canonical place the Res34rch flash path is disclosed
  (post-purchase email / in-box card / on-device / thanks page) and align §2a/§2b.
- **Recommended additions (huddle, counsel to confirm):** a short **indemnity**
  line ("if your use causes a claim against the seller, you cover the seller's
  costs"); **age self-attestation** (not ID verification); the **OFAC** no-embargo
  shipping line; keep the existing **liability cap = purchase price**.
- **No-misuse-encouragement standing rule:** no marketing copy, tool name,
  More-Info string, or boot flavor-text may instruct, encourage, or glamorize
  unauthorized use. (The de-weaponization pass enforced this in firmware; keep it
  as a rule for all surfaces.)

---

*Provenance: Bryce (orig) + Tomas (edits 1–4) + web_flash agent (5–7, GPLv3-§7
catch) + shop agent (behavior-based prohibitions, generic language, Marauder
attribution) + ui_test 3-lens panel (statutory citations, scroll-to-enable,
SKU-split, versioning, indemnity/age/OFAC). Reconciled into this canonical v0.1.*
