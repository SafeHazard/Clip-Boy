"""Generate Flux.1 Dev / SDXL image prompts from collectibles CSV.

Produces consistent 512x512 grayscale icon prompts suitable for conversion
to 80x80 A8 alpha-mask images (bright = opaque, black = transparent).

Usage:
    py -3 scripts/gen_image_prompts.py [--csv data/collectibles.csv] [--out prompts/] [--format flux|sdxl]

Output: one .txt per collectible (ID_slug.txt) + manifest.csv mapping ID -> prompt file.
"""

import argparse
import csv
import os
import re
import sys

# ─────────────────────── VISUAL SUBJECT MAPPING ─────────────────────────────
# Manual overrides for items where the title/source alone isn't enough to
# derive a clear single-object visual subject. Add entries as needed.
# Key = collectible ID, Value = concise visual subject description.

SUBJECT_OVERRIDES = {
    1:   "a decorated Black Forest chocolate cake with frosting writing",
    2:   "a cracked and damaged Weighted Companion Cube from Portal",
    3:   "a glowing mystical eye artifact with radiating light",
    4:   "a vintage reel-to-reel tape recorder",
    5:   "a small blue pill capsule",
    6:   "a bent spoon with warped metallic reflections",
    7:   "Master Chief's MJOLNIR helmet with a cracked visor",
    8:   "a military Warthog vehicle hood ornament, rugged metal",
    9:   "a small cylindrical drop pod stress ball with scorch marks",
    10:  "a robotic T-800 endoskeleton thumb giving a thumbs up, chrome metal",
    12:  "a retro corporate employee ID badge with Cyberdyne Systems logo",
    13:  "a glowing microchip on a circuit board, ominous red light",
    14:  "a large red arcade-style push button",
    15:  "a gold ring with glowing elvish script inscriptions",
    16:  "a wooden staff with a glowing crystal top, planted like a sign",
    17:  "an elven short sword glowing blue-orange, ornate blade",
    18:  "a red Starfleet uniform shirt, slightly worn",
    19:  "a round fluffy tribble creature, fuzzy and purring",
    20:  "a handheld universal translator device with a cracked screen",
    21:  "Thor's hammer Mjolnir, heavy stone and metal",
    22:  "a handwritten note on crumpled paper with doodles",
    23:  "a cracked red kyber crystal with energy fractures",
    24:  "Han Solo's DL-44 blaster pistol, worn metal",
    25:  "a flowing black cape draped over an invisible form",
    26:  "a row of six numbers (4 8 15 16 23 42) on a worn computer terminal",
    27:  "a mason jar with dark swirling smoke leaking from the lid",
    28:  "a Ghostbusters proton pack with dead power cell indicator",
    29:  "an official operating license document with a red SUSPENDED stamp",
    30:  "a thick lit cigar with curling smoke",
    31:  "a laminated press pass badge in Comic Sans font",
    32:  "a military dog tag with a question mark engraved",
    33:  "a plain wooden stick radiating dark energy",
    34:  "a pair of retro 90s inline rollerblades",
    35:  "a 3.5-inch floppy disk with a skull label",
    36:  "a small fairy companion trapped under a glass dome, glowing",
    37:  "a dramatic script page with theatrical stage directions",
    38:  "a leather-bound research notebook with handwritten notes",
    39:  "a glowing lightbulb held carefully in two hands",
    40:  "an ancient scroll with partially faded prophecy text",
    41:  "Lord Zedd's Z-shaped metal staff with lightning energy",
    42:  "a metallic bird-like robot beak, angular and sarcastic-looking",
    43:  "a red gumball machine head dome on a small robot body",
    44:  "a grease-stained spaceship maintenance manual with duct tape binding",
    45:  "an official warrant document with a blinking red tracking dot",
    46:  "a phone booth shaped keychain fob, retro red",
    47:  "a long receipt scroll with ice cream illustrations",
    48:  "a leather-bound logbook titled 'Excuses'",
    49:  "a large red danger button with flashing warning lights",
    50:  "an ornate magician's top hat with a skull motif",
    51:  "a dented tin can labeled 'Chunky Bits' with expired sticker",
    52:  "a disturbing piece of fan art on crumpled paper",
    53:  "a detached semi-truck trailer, futuristic military style",
    54:  "a damaged robot voice box speaker with exposed wires",
    55:  "a handheld laser device with a defocused lens",
    56:  "a circular Dial Home Device console with glowing glyphs",
    57:  "a military team shoulder patch with SG-1 designation",
    58:  "a sealed canopic jar with hieroglyphic markings, slightly ajar",
    60:  "a vintage computer terminal login screen showing 'ACCESS GRANTED'",
    61:  "a voided security badge with a red X stamped across it",
    62:  "a jagged dark ship hull fragment with organic veins, glowing faintly",
    63:  "a burlap sack of assorted mechanical spare parts and tools",
    64:  "a reel-to-reel tape labeled CLASSIFIED with smoke rising from it",
    65:  "a crumpled handwritten note with coffee stains",
    66:  "a magical parchment map showing glowing moving dots",
    67:  "an oversized overcoat pocket bulging with assorted objects",
    68:  "a perfectly proportioned black rectangular monolith",
    69:  "an empty glass rum flask with a cork",
    70:  "a brass compass with a spinning needle pointing sideways",
    71:  "a sonic screwdriver device, metallic with a glowing green tip",
    72:  "a stone angel statue with hands covering its face, weeping pose",
    73:  "a Y-shaped flux capacitor device with glowing energy channels",
    74:  "a guitar pick with lightning bolt design",
    # TODO: ID 75 is "Ticket to SheetmetalCon 26" (collectables.csv source
    # ID 206). The image at images/generated/75.png is still the old
    # DeLorean ornament placeholder; regenerate with this SheetmetalCon
    # prompt before the production print.
    75:  "a VIP credential ticket for SheetmetalCon 26, metallic blue and silver, embossed press brake icon, conference lanyard hole at top",
    76:  "a green warp pipe from Mario, arcade coin token nearby",
    77:  "a blue spiny Koopa shell with motion blur",
    78:  "a glowing circular identity disc from Tron, neon blue edge",
    79:  "a pair of stylish sunglasses with a chaos theory equation reflected",
    80:  "a police badge engraved with 'Sequel Police'",
    81:  "a signed autograph page with two signatures and a doodle",
    83:  "a red Mexican luchador wrestling mask",
    84:  "a printed email on paper with pixel art header",
    85:  "a dark menacing car CPU chip fragment with red circuitry",
    86:  "a dashboard-mounted turbo boost button, red with chrome ring",
    87:  "a weathered brown leather fedora hat",
    88:  "a glowing ancient manuscript with alien script",
    89:  "a crushed empty beer can with a faint golden glow",
    90:  "a glowing blue Nuka-Cola bottle with radioactive bubbles",
    92:  "a targeting monocle HUD device with percentage readouts",
    93:  "a large mottled reptilian egg in a rough nest",
    95:  "a convention lanyard badge with 'VETERAN' ribbon",
    96:  "a telephone handset with a confident smirk aura",
    97:  "a black staff t-shirt neatly folded with a radio on top",
    98:  "a sheep silhouette with a WiFi signal symbol above its head",
    99:  "a legendary hacker conference badge, circuit board with 'HELLO'",
    100: "a commemorative circuit board badge with '34' etched prominently",
}

# ─────────────────────── PROMPT TEMPLATES ────────────────────────────────────

# Flux.1 Dev: no negative prompt support, relies on descriptive positive prompt.
# CFG guidance is low (1.0-3.5), steps 20-30, 512x512.

FLUX_TEMPLATE = (
    "A single {subject}, extreme close-up macro shot, "
    "the object is bright white and fills 80 percent of the frame. "
    "Pure black background, pure black void behind the object. "
    "The object is rendered in bright white and light gray tones only, "
    "like a white plaster cast or white porcelain sculpture of the object. "
    "Strong rim lighting and bright studio key light from above. "
    "High contrast, sharp details, bold readable silhouette. "
    "Video game inventory icon, Pip-Boy icon style from Fallout. "
    "No text, no labels, no watermarks, no color, no extra objects, "
    "no ground, no floor, no surface. Isolated floating object."
)

# SDXL / Juggernaut: supports negative prompt.
SDXL_TEMPLATE = (
    "A single {subject}, centered on a pure black background. "
    "Monochrome grayscale rendering, bright white highlights against dark black void. "
    "Clean studio lighting from above-left, sharp fine details, bold readable silhouette. "
    "Video game inventory icon, highly detailed miniature object study. "
    "Professional product render, 8K detail, "
    "Pip-Boy inventory icon style from Fallout, isolated object floating in darkness."
)

SDXL_NEGATIVE = (
    "color, colorful, saturated, rainbow, red, blue, green, yellow, "
    "text, words, letters, labels, watermark, signature, logo, "
    "multiple objects, crowd, scene, landscape, background detail, "
    "ground plane, floor, table, surface, shadow on background, "
    "blurry, soft focus, low detail, low contrast, flat lighting, "
    "person, hands, fingers, face, human, anime, cartoon, "
    "border, frame, vignette, noise, grain, artifacts, "
    "photorealistic skin, photograph of a screen"
)

# ─────────────────────── SUBJECT EXTRACTION ──────────────────────────────────

def extract_subject(item_id, title, source, description):
    """Derive a visual subject from collectible metadata.

    Priority:
    1. Manual override (SUBJECT_OVERRIDES) — best quality
    2. Heuristic extraction from title + source

    The goal is a concise noun phrase describing ONE drawable object.
    """
    if item_id in SUBJECT_OVERRIDES:
        return SUBJECT_OVERRIDES[item_id]

    # Fallback: strip parenthetical qualifiers from title, use as subject
    # e.g. "Navi (Muted)" -> "Navi"
    clean = re.sub(r'\s*\(.*?\)\s*', ' ', title).strip()

    # Add source context if it helps
    subject = f"a {clean.lower()} from {source}"
    return subject


def make_slug(title):
    """Create a filesystem-safe slug from a title."""
    slug = re.sub(r'[^a-zA-Z0-9]+', '_', title).strip('_').lower()
    return slug[:60]  # truncate long names


def generate_prompt(subject, fmt="flux"):
    """Generate a complete prompt for the given subject and model format."""
    template = FLUX_TEMPLATE if fmt == "flux" else SDXL_TEMPLATE
    # Avoid "A single a ..." — strip leading article from subject
    clean = re.sub(r'^(a|an)\s+', '', subject, flags=re.IGNORECASE)
    return template.format(subject=clean)


# ─────────────────────── MAIN ────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Generate image prompts from collectibles CSV")
    parser.add_argument("--csv", default="data/collectibles.csv", help="Input CSV path")
    parser.add_argument("--out", default="prompts", help="Output directory for prompt files")
    parser.add_argument("--format", choices=["flux", "sdxl", "both"], default="both",
                        help="Model format (flux, sdxl, or both)")
    parser.add_argument("--manifest", default=None, help="Manifest CSV path (default: <out>/manifest.csv)")
    parser.add_argument("--dry-run", action="store_true", help="Print prompts to stdout, don't write files")
    args = parser.parse_args()

    # Read CSV
    csv_path = args.csv
    if not os.path.isfile(csv_path):
        print(f"ERROR: CSV not found: {csv_path}", file=sys.stderr)
        sys.exit(1)

    items = []
    with open(csv_path, "r", encoding="utf-8-sig") as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                item_id = int(row["ID"])
            except (ValueError, KeyError):
                continue
            items.append({
                "id": item_id,
                "title": row.get("Title", "").strip(),
                "source": row.get("Source", "").strip(),
                "tier": row.get("Tier", "").strip(),
                "description": row.get("Description", "").strip(),
            })

    if not items:
        print("ERROR: No items parsed from CSV", file=sys.stderr)
        sys.exit(1)

    print(f"Parsed {len(items)} collectibles from {csv_path}")

    # Create output directory
    if not args.dry_run:
        os.makedirs(args.out, exist_ok=True)

    manifest_rows = []
    formats = ["flux", "sdxl"] if args.format == "both" else [args.format]

    for item in items:
        subject = extract_subject(item["id"], item["title"], item["source"], item["description"])
        slug = make_slug(item["title"])

        for fmt in formats:
            prompt = generate_prompt(subject, fmt)
            filename = f"{item['id']:03d}_{slug}_{fmt}.txt"

            if args.dry_run:
                print(f"\n{'='*72}")
                print(f"ID={item['id']} | {item['title']} | {item['source']} | {fmt.upper()}")
                print(f"Subject: {subject}")
                print(f"Prompt: {prompt}")
                if fmt == "sdxl":
                    print(f"Negative: {SDXL_NEGATIVE}")
            else:
                filepath = os.path.join(args.out, filename)
                with open(filepath, "w", encoding="utf-8") as pf:
                    pf.write(prompt)
                    if fmt == "sdxl":
                        pf.write(f"\n\n---NEGATIVE---\n{SDXL_NEGATIVE}")

            manifest_rows.append({
                "id": item["id"],
                "title": item["title"],
                "source": item["source"],
                "tier": item["tier"],
                "slug": slug,
                "format": fmt,
                "filename": filename,
                "subject": subject,
                "has_override": "yes" if item["id"] in SUBJECT_OVERRIDES else "no",
            })

    # Write manifest
    if not args.dry_run:
        manifest_path = args.manifest or os.path.join(args.out, "manifest.csv")
        with open(manifest_path, "w", encoding="utf-8", newline="") as mf:
            writer = csv.DictWriter(mf, fieldnames=[
                "id", "title", "source", "tier", "slug", "format",
                "filename", "subject", "has_override"
            ])
            writer.writeheader()
            writer.writerows(manifest_rows)
        print(f"Wrote {len(manifest_rows)} prompt files to {args.out}/")
        print(f"Manifest: {manifest_path}")

    # Stats
    overridden = sum(1 for i in items if i["id"] in SUBJECT_OVERRIDES)
    print(f"\nSubject overrides: {overridden}/{len(items)} "
          f"({len(items)-overridden} using auto-extraction)")


if __name__ == "__main__":
    main()
