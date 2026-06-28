# LWO Editor — Authoring Custom Surface Markers (note for the editor work)

Context: the engine needs per-surface markers it can't infer (first one:
"single shadow id" — collapse a curved surface to one ShadowMatID so the PolyId
cube-shadow test doesn't paint self-shadow acne on every facet edge; see the
greets mummy fix, commit 84a41d8). Today that's a hardcoded C++ name allowlist
(`kSingleShadowIdMats = {"momy"}` in GREETS.CPP). We want it authored on the
surface instead. This note records the two viable routes + the format facts so
the editor can pick one.

## Format facts (verified in tools/lwsread)

- Objects are **LWOB** (classic pre-LW6 IFF). `tools/lwsread/LWOREAD.CPP`.
- **Surface name length:** names are null-terminated, even-byte-padded; the LWOB
  format imposes no hard cap, but our reader `ReadAsciiZ` uses a fixed
  `char Temp[256]` → **practical limit 255 chars + null**. ⚠️ The reader has NO
  bounds check (`Temp[a++]` in a loop) — a name ≥256 bytes overflows it. If the
  editor can produce long names, add a guard to `ReadAsciiZ` first. Downstream
  storage is heap `char*` (`FldMat::Name` → `Material::Name`), unbounded.
- **Surface FLAG field is 16-bit** (`fread(&Surf->Flags,1,2,LWO)`,
  `ReadSurfaceFlags`). Standard LWOB uses only bits **1–1024**:
  Luminous(1) Outline(2) Smoothing(4) ColorHighlights(8) ColorFilter(16)
  OpaqueEdge(32) TransparentEdge(64) SharpTerminator(128) DoubleSided(256)
  Additive(512) ShadowAlpha(1024). **Bits 0x0800, 0x1000, 0x2000, 0x4000,
  0x8000 are FREE.**
- The converter **already carries** `Surf->Flags → Material.TFlags` end-to-end
  (that's how the engine reads the smoothing flag today). So a custom FLAG bit
  needs **no converter change** — it arrives in `Material.TFlags` for free.

## Route A — custom FLAG bit (RECOMMENDED, "an actual flag")

Reserve a Revival custom-bit range in the surface FLAG word and assign:

| Bit | Name | Meaning |
|-----|------|---------|
| `0x4000` | `Rev_SingleShadowId` | Collapse this surface to one ShadowMatID (curved-surface self-shadow-acne fix) |
| `0x2000` | _reserved_ | (next marker — e.g. alcove handling) |
| `0x1000` | _reserved_ | |
| `0x8000` | _reserved_ | |
| `0x0800` | _reserved_ | |

Flow: **editor UI checkbox → sets bit in surface FLAG → LWO writer stores the
16-bit word → `lwsread` reads FLAG → `Material.TFlags` → engine checks
`TFlags & 0x4000`.** The engine swaps the C++ name allowlist for that mask test;
nothing else changes.

**Caveat (document it):** the custom bit only survives when **our editor is the
author of record**. If the LWO is re-saved by **stock LightWave**, it rewrites
the FLAG word with only the bits it knows → our bit is dropped. (A custom IFF
chunk has the same fate — LightWave drops unknown chunks on save.) Since
authoring goes through the editor, this is acceptable; just don't promise
stock-LightWave round-trip safety for these markers.

## Route B — surface-name token (round-trip-safe fallback)

If stock-LightWave round-trip safety is ever required: the editor checkbox
auto-manages a recognized token in the surface NAME (e.g. a trailing
` $sid` / `~1id`), and the engine matches the token instead of a flag. Names
survive any tool. Downsides: it's string-based, eats into the 255-char budget,
and the token is visible in the surface name. The editor should own the
token (add/remove on checkbox toggle, hide it from the displayed name) so the
artist never types it.

## Recommendation

Use **Route A** (`Rev_SingleShadowId = 0x4000`). It's a real flag (the preferred
shape), free in the format, already carried by the converter, and authored via
the editor. Reserve the 0x0800–0x8000 range for Revival markers and keep this
table as the registry. Only fall back to Route B if stock-LightWave re-save of
authored files becomes a real workflow.

When Route A lands, replace `kSingleShadowIdMats` (GREETS.CPP) with a
`Material::TFlags & Rev_SingleShadowId` test, and add `Rev_SingleShadowId` to
`FDS_DEFS.H` next to the `Mat_*`/`Surf_*` defines.
