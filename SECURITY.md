# Clip-Boy Security & Release Integrity

## Reporting

Found a vulnerability in the firmware or the release pipeline? Open a GitHub
issue for non-sensitive reports, or email the maintainer for anything that
should stay private until fixed. Please don't file exploit details publicly
before we've had a chance to respond.

---

## Release integrity (how to trust a firmware download)

Clip-Boy uses **transparency-style attestation, not device-enforced Secure
Boot.** We deliberately do **not** burn the ESP32-S3 Secure-Boot eFuses: that
is an irreversible, one-way change that would stop you from flashing your own
or modified builds, which is contrary to the badge's open / GPLv3 /
user-flashable / collectible-moddable design. Instead, every official release
is signed so you can verify the bytes are ours.

The integrity manifest is **committed to this repo** (not attached to a
GitHub release), so the flasher can fetch it cross-origin from a different
trust domain than the host serving the bins:

| File (in `release/`) | What it is |
|---|---|
| `flash-spec.json` | flash recipe: per-part offsets **and** SHA-256 |
| `SHA256SUMS` | SHA-256 of every bin **and** of `flash-spec.json` |
| `SHA256SUMS.minisig` | the Ed25519 (legacy/non-prehashed) signature over `SHA256SUMS` |

The **firmware bins themselves are distributed by the web flasher**
([flash.brycebadges.com](https://flash.brycebadges.com)), not published here.
The `-rift` (founding-backer) bins are **gated** behind backer auth — they are
not secret (their hashes are in the signed `SHA256SUMS`, and a backer's gated
download still verifies against it), but they are not publicly downloadable.

### Verify

1. Install [minisign](https://jedisct1.github.io/minisign/) (tiny, Ed25519).
2. Check the signature against our public key, then the file hashes:

```sh
minisign -Vm SHA256SUMS -P RWS3nF/cRYZf3xN3Z9KSkNbKdGcOzcAIzm5+9bkUpmrO361rt8lkhlOa
sha256sum -c SHA256SUMS        # or: shasum -a 256 -c SHA256SUMS  (macOS)
```

If both pass, the bins your flasher (or you, via esptool) write are exactly
what we signed. The web flasher does the equivalent **in-browser**: it pins
this public key in its code, fetches `SHA256SUMS` + `.minisig` cross-origin
from this repo, verifies the Ed25519 signature with WebCrypto, then checks each
served bin against the signature-anchored hash before writing it. (A page that
fully controls the served JS could still strip the check — that floor is
irreducible from inside the browser; the out-of-band verify above and the
reproducible builds below are the real anchors.)

### Trust anchor — the release public key

```
RWS3nF/cRYZf3xN3Z9KSkNbKdGcOzcAIzm5+9bkUpmrO361rt8lkhlOa
```

Committed at [`keys/clipboy-release.pub`](keys/clipboy-release.pub). For
minisign this short string **is** the key fingerprint. The private key lives
only in a passphrase-protected password-manager item on the signing machine —
never in this repo, never shared with any web/CF/Stripe credential. A host or
CDN compromise therefore can't re-sign: an attacker would also need this
offline key.

---

## Reproducible builds (the keyless backstop)

The signature proves *we* built the bytes; reproducible builds let a skeptic
confirm the bytes match the **public source** without trusting us or our build
machine. This applies to the public variants **`sn34k`** and **`res34rch`**.

Toolchain (pinned — must match for byte-identical output):

- `arduino-cli compile` with ESP32 Arduino core **`esp32:esp32@2.0.10`**
  (toolchain `xtensa-esp32s3-elf-gcc 8.4.0`, esp-2021r2-patch5).
- All other libraries are **vendored** in [`libs/`](libs/) — a fresh clone
  builds with no external fetches (`build.sh` passes `--libraries libs`).
  The vendored set includes pre-compiled `.a` archives (lz4, audio-tools)
  whose internal path literals are frozen at vendoring time, so they're
  constant across clones.
- Exact FQBN + build properties are in [`scripts/build.sh`](scripts/build.sh)
  (`CDCOnBoot=cdc, FlashSize=16M, PSRAM=opi, PartitionScheme=default`, custom
  `build.partitions=partitions`, `upload.maximum_size=8388608`).

Two things make the output **independent of where you build it**:

- **Deterministic stamp** — `build.sh` writes `build_stamp.h` with the commit
  `date+sha` (instead of wall-clock `__DATE__`/`__TIME__`), so the About
  screen and the binary don't change with build time.
- **Path normalization** — `build.sh` passes
  `-ffile-prefix-map=<project-dir>=.`, so the absolute build path the SDK
  otherwise bakes into the image's `app_elf_sha256` is stripped. Verified: a
  built ELF contains zero occurrences of the project's absolute path.

Rebuild and compare (use the same fixed build path the release uses, so the
embedded `./build/<variant>` matches):

```sh
bash scripts/build.sh            --build-path build/sn34k      # sn34k
bash scripts/build.sh --res34rch --build-path build/res34rch  # res34rch
sha256sum build/<variant>/ui_test.ino.bin   # == the released clipboy-<variant>-app.bin
```

A clean-tree build of a given commit is byte-identical to ours (confirmed by
double-build `cmp`). If your tree is dirty the stamp gains a `-dirty` suffix and
the bytes won't match — that's intended.

### ⚠ Not reproducible: rift variants

`--rift` (`clipboy-*-rift-*.bin`) bakes in **local, uncommitted** Quantum-Rift
boot art (DC34-103), so it can't be reproduced from the public repo. Rift bins
get the **signed-hash** treatment only; there is no keyless backstop for them.

---

## Release procedure (maintainer)

```sh
bash scripts/make_release_bins.sh   # builds all variants; emits per-part
                                    # sha256 into flash-spec.json + SHA256SUMS
scripts/sign_release.sh             # -> SHA256SUMS.minisig (legacy Ed25519; needs the key)
git add release/flash-spec.json release/SHA256SUMS release/SHA256SUMS.minisig
git commit && git push              # the manifest+signature go on the repo;
                                    # the flasher fetches them cross-origin
# Hand the built bins to web_flash's sync (it serves them; rift behind backer auth).
```

`make_release_bins.sh` is secret-free and deterministic given the bins;
`sign_release.sh` is the only step that touches the private key (kept separate
so routine bin generation never needs it). The `.bin` files are **not**
committed and **not** published as a release here — web_flash distributes them
(rift gated). Only the committed manifest + signature make the bins verifiable
wherever they're served.
