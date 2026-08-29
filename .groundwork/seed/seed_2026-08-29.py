#!/usr/bin/env python
"""Seed the revival-fog conclusion ledger (2026-08-29), run by the coordinator's seeding fork.

Every record here is copied from a document in this tree (or from a rev-dispfix commit that
resolves in the shared object store); the verdict quotes are Gil-Ad's verbatim words as recorded
in docs/BULGE_CORPUS.md and the commit messages. Run with the groundwork venv python from anywhere:

    GROUNDWORK_WRITER=coordinator /Users/gil-ad/work/groundwork/.venv/bin/python .groundwork/seed/seed_2026-08-29.py

Re-running is idempotent: identical records get the same content-hash id and are not appended.
"""
from __future__ import annotations

import sys
from pathlib import Path

from groundwork.store import Ledger, LintError, Store, global_dir, open_ledger

ROOT = Path(__file__).resolve().parents[2]
L = open_ledger(ROOT)
G = Ledger(None, Store(global_dir(), "global"))
failed: list[tuple[str, list[str]]] = []
added: list[str] = []


def add(rec: dict, quote: str | None = None, quote_date: str | None = None, to_global: bool = False) -> str | None:
    ledger = G if to_global else L
    try:
        rid, warns = ledger.add(rec, writer="coordinator", quote=quote, quote_date=quote_date, to_global=to_global)
    except LintError as e:
        failed.append((rec.get("subject", "?"), e.errors))
        return None
    added.append(rid)
    return rid


def ev(t: str, ref: str, note: str | None = None) -> dict:
    d = {"type": t, "ref": ref}
    if note:
        d["note"] = note
    return d


SEED_NOTE = " [seeded 2026-08-29 by the coordinator from {src}]"

# =====================================================================================
# A. PERF_LAWS.md — nine laws
# =====================================================================================
LAWS = [
    ("L1_fanout_threshold", "A fan-out pays only above a work-per-dispatch threshold of ~tens of microseconds; under ~100 us the round trip is the cost (city glass -24 %, RTT cone pass +139 %, mirror-mask clear +33 %).", {},
     [ev("battery", "docs/PERF_LAWS.md#L1", "4 sightings"), ev("commit", "commit:9dcee857")],
     "a pass under ~100 us of work whose fan-out measures faster on an interleaved min-of-11 battery"),
    ("L2_scalar_ilp", "An 8-iteration independent scalar loop is not automatically improved by vectorising it: the scalar form already extracts ILP; price it in cycles, never in instructions (SSAO gather -9.8 % instr, +2.5 % cycles).", {},
     [ev("battery", "docs/PERF_LAWS.md#L2", "3 sightings"), ev("commit", "commit:aebab960")],
     "a short independent scalar loop whose vector form measures fewer CYCLES, not fewer instructions"),
    ("L3_register_pressure", "Register pressure beats op count in the cone and lighting kernels: C6 removed 10 vector-ALU ops and paid 29 spills, +5.42 % cycles; one __m256 is two of arm64's 32 v-registers.", {"granularity": "kernel_row"},
     [ev("battery", "docs/PERF_LAWS.md#L3"), ev("commit", "commit:ef8fa5d3")],
     "an op-count reduction in those kernels that does not raise ldr/str spill counts and measures faster"),
    ("L4_issue_bound", "Once a kernel is issue-bound instruction counts stop predicting time; the cone pass at ~81 % of NEON issue converts instructions to wall, lighting-w1 does not (-2.0 % instr -> +0.7..1.4 % cycles).", {},
     [ev("battery", "docs/PERF_LAWS.md#L4"), ev("commit", "commit:dac4bf9e")],
     "a row not at its issue ceiling whose instruction reduction reproduces as a cycle reduction across batteries"),
    ("L5_cores_triage", "cores = Gcyc / clock / wall finds both the wins and the refutations: a row far below the worker count is serial or barrier-bound, one at the worker count is compute-bound.", {},
     [ev("doc", "docs/PERF_LAWS.md#L5")],
     "a row whose implied core count mispredicts which lever (fan-out vs fewer ops) pays, on a measured battery"),
    ("L6_fewer_pairs", "The only lever with real headroom on a saturated per-pixel row is FEWER PAIRS: micro-optimising the body is exhausted (cone pass -52.6 % by range cull; greets lighting-w1 8.40 lights/px enter, 2.06-3.40 accumulate).", {},
     [ev("battery", "docs/PERF_LAWS.md#L6"), ev("commit", "commit:21624788")],
     "a body-only optimisation on the cone pass or greets lighting-w1 measuring above the placement floor"),
    ("L7_share_source", "Bit-exactness is won by SHARING SOURCE, not transcribing: the froxel reflection leg went 8-wide bit-exact first build by lifting the leg into one static inline helper both paths call.", {},
     [ev("battery", "docs/PERF_LAWS.md#L7", "28 warm frames across six arms"), ev("commit", "commit:dce8719f")],
     "a shared-source vectorisation that fails bit-exactness at the pins for a reason other than FMA contraction"),
    ("L8_per_tile_decidable", "Anything that runs per-pixel and is decidable per-tile or per-light is the first place to look, and the guard usually already exists - check before proposing it.", {},
     [ev("doc", "docs/PERF_LAWS.md#L8")],
     "a per-pixel row with a per-tile decidable predicate whose guard measures below the floor"),
    ("L9_runtime_predicate", "In a kernel at its register-allocation limit a new runtime predicate costs more than the work it removes: a never-taken skip inside the cube tap cost +12.4 %, a runtime bool hatch +4.3 % with the flag OFF; the cube tap only gets cheaper by being CALLED less.", {"scene": "greets", "row": "lighting-w1"},
     [ev("battery", "docs/PERF_LAWS.md#L9"), ev("doc", "docs/OPTIMIZATION_BACKLOG.md#L4783")],
     "a live bool added to the greets cube tap that measures flat or faster with the flag OFF"),
]
for key, claim, scope, evidence, fals in LAWS:
    add({"kind": "law", "subject": f"perf.law.{key}", "claim": claim + SEED_NOTE.format(src="docs/PERF_LAWS.md"),
         "value": "holds", "contract": "state", "scope": scope, "evidence": evidence, "falsifier": fals})

# =====================================================================================
# B. PERF_LAWS.md — fourteen measurement traps
# =====================================================================================
TRAPS = [
    ("T1_green_pins_not_exercised", "A green 13/13 does not mean a path was exercised: every pin is a one-tick --snapshot and default-ON paths that switch on from tick 2 execute zero times; a binary computing no city water fog passed 12/12 pins and failed 5/7 warm rows. Use tools/warm_gate.sh.", {"tool": "snapshot"}, "commit:939dba5f"),
    ("T1b_cold_census_misprices", "A cold one-tick census mis-prices the backlog: the froxel composite's scalar half was on file at 0.72 % of renderFrame (cold, 1512x848) and is 5.15 % warm at 1920x1080, 7x low.", {"tool": "census"}, "commit:dce8719f"),
    ("T2_stale_shares", "Stale shares get stale: re-take the measurement before sizing a plan off it (the 36.6 % cube-tap share stood for eleven rounds; all shadow work is 16-28 %).", {"scene": "greets"}, "commit:63f0b8f1"),
    ("T3_hwread_process_wide", "TailProf::hwRead() is proc_pid_rusage(getpid()) - PROCESS-WIDE; a nested scope inside a threaded pass measures every thread. Split threaded sub-scopes by ablation differencing, never by nesting a timer.", {"tool": "TailProf"}, "docs/PERF_STATE.md#L214"),
    ("T4_chdir_asset_root", "ChdirToAssetRoot makes a binary's FILE LOCATION select its assets: DEMO chdirs to its own directory, so a worktree binary always renders that worktree's assets. Three false alarms traced to this; gate with --no-chdir_assets.", {"tool": "DEMO"}, "commit:18a1f0b4"),
    ("T5_chase_pose_sequence", "chase pins are POSE-SEQUENCE dependent: t=800 alone differs from t=800 third of five by 434 591 px (20.96 %, max 5); only the first pose of a process is comparable across recipes.", {"scene": "chase", "tool": "snapshot"}, "commit:18a1f0b4"),
    ("T6_pgrep_self_match", "pgrep -f matches your own shell: two agents' box-quiet loops deadlocked on each other's pgrep command lines. Use ps -Ao comm= | grep -c '/DEMO$'.", {"tool": "pgrep"}, "commit:0a37816f"),
    ("T7_ssao_dump_inflates", "--ssao_dump forces the scalar apply loop and inflates the pass it measures ~3.5x (4.2 -> 13.7-16.0 ms); a correctness instrument, never a perf one.", {"flag": "ssao_dump"}, "docs/PERF_LAWS.md#T7"),
    ("T8_cmake_install_rpath", "cmake --install fails on a pre-existing rpath step (install_name_tool: no LC_RPATH) - that is NOT a failed build; the binary is placed by the POST_BUILD copy. Verify with --no-chdir_assets.", {"tool": "cmake"}, "docs/PERF_LAWS.md#T8"),
    ("T9_warm_gate_zero_frames", "A row that emitted no frames must not print as a pixel mismatch: warm_gate.sh printed an empty got under FAIL and sent a reader hunting a regression for an hour; it now prints ERROR <row> produced N/M frames.", {"tool": "warm_gate.sh"}, "commit:b0db7796"),
    ("T10_repetition_is_blind", "24 repetitions of a blind test is still a blind test: the bsWorld regression passed 13/13 pins and 24 one-tick runs; the defect began at tick 4.", {"tool": "snapshot"}, "commit:b227010a"),
    ("T11_half_gated_flag", "A flag that gates half a change is not a revert arm: round 5's glass change shipped a fan-out and a bsWorld cache under one flag that did not gate the cache. If --no-<flag> is not byte-identical to the parent, it is not a control.", {}, "commit:c15a9cb1"),
    ("T12_not_yours_to_rebaseline", "A warm-gate red from someone else's change is not yours to re-baseline: one pixel, max 1-2, deterministic 3/3 is exactly what a stale cached bounding sphere looks like. Report to the owner, leave the baseline.", {"tool": "warm_gate.sh"}, "commit:8841589d"),
    ("T13_lto_layout_floor", "Between-binary comparisons carry an LTO-layout floor of +-0.9 % on renderFrame and up to 6 % on a row; prefer same-binary flag flips, else diff per-function instruction counts to prove locality.", {"granularity": "binary"}, "commit:3cff1085"),
    ("T14_instruments_need_frames", "Instruments need the frames they were built for: both shadow censuses report every 8 main-view frames and the cone/fog censuses report the PREVIOUS frame; a one-pose snapshot prints nothing. Use --bench or a multi-pose snapshot and read the second report.", {"tool": "census"}, "docs/PERF_LAWS.md#T14"),
]
for key, claim, scope, ref in TRAPS:
    evidence = [ev("doc", f"docs/PERF_LAWS.md#{key.split('_')[0]}")]
    if ref.startswith("commit:"):
        evidence.append(ev("commit", ref))
    elif ref != evidence[0]["ref"]:
        evidence.append(ev("doc", ref))
    add({"kind": "trap", "subject": f"perf.trap.{key}", "claim": claim + SEED_NOTE.format(src="docs/PERF_LAWS.md"),
         "value": "trap", "contract": "state", "scope": scope, "evidence": evidence,
         "falsifier": "a documented case where following the trap's advice measured wrong, or the mechanism the trap names was shown absent"})

# the four traps from the displacement round (rev-dispfix, 2026-08-28/29) and the zsh one
NEW_TRAPS = [
    ("tear_battery_absolute_outroot", "tools/tear_battery.sh needs an ABSOLUTE outroot, or every pose silently renders nothing while printing done.", {"tool": "tear_battery.sh"}, "commit:fa606dd4"),
    ("bake_face_array_is_whole_trimesh", "DisplaceStoneSubdiv's faces/fIdx hold the WHOLE TriMesh (foreign-material faces appended verbatim), so any pass looping 0..nF0 must test isTargetNew - the inherited tsplit split 530 floor faces from the rooms bake.", {"tool": "DisplaceStoneSubdiv", "scene": "greets"}, "commit:135e642a"),
    ("refdiff_raw_is_tracked", "docs/img/refdiff/raw holds 120 TRACKED verdict-corpus dumps; do not rm -rf it as scratch (a fork deleted and restored them from HEAD).", {"tool": "git", "scene": "greets"}, "commit:fa606dd4"),
    ("zsh_flag_string_splitting", "zsh does not word-split a variable holding a flag string: use ${=VAR} to split it, and never echo a line of ===.", {"tool": "zsh"}, "commit:dcdb3843"),
]
for key, claim, scope, ref in NEW_TRAPS:
    add({"kind": "trap", "subject": f"perf.trap.{key}", "claim": claim + SEED_NOTE.format(src="rev-dispfix handoff / SESSION_STATE"),
         "value": "trap", "contract": "state", "scope": scope, "evidence": [ev("commit", ref)],
         "falsifier": "a run that reproduces the failure with the advice followed, or the code path changed so the mechanism is gone"})

# =====================================================================================
# C. BULGE_CORPUS.md — his labeled states, verbatim (the coordinator copies the quotes)
# =====================================================================================
CORPUS = [
    ("1", "bare", "ok_geometry", "the bare wall is flat", "2026-08-28", "the undisplaced wall's SHAPE is flat; NOT a blanket shading-OK (planes 45/41 carry pre-existing pollution)"),
    ("2", "prefo", "broken", "still bulges, still not even close to be ok", "2026-08-28", "the month-old bulge era"),
    ("3", "fixcam", "broken", "i took a look on the pics you've watched - they have the same issue as always - a bulge", "2026-08-28", "front_orient era"),
    ("4", "ship", "broken_improved", "it still sucks (although a bit bit better)", "2026-08-28", "groove_shade_plane era"),
    ("5", "r3", "broken", "you are showing me obviously wrong results, and you are unable to detect that the results are bad", "2026-08-28", "current defaults of 2026-08-28 (all four fixes ON) - the state every prior metric passed"),
    ("6", "v3", "broken", "wow, this is worse than useless", "2026-08-28", "the clean-room v3 bake (rev-dispv3)"),
    ("7", "reflex_weld", "broken_tears", "the last change added tears. I don't care about those tears, it's a bug written when trying to fix a real bug", "2026-08-28", "reflex weld build, superseded; weld now default OFF"),
    ("8", "pom_shell_v1", "broken", "the first version of the displacment map already has an issue where parts of the wall jumped outside (it didn't even use tessalation)", "2026-08-28", "historical: the first displacement-map version"),
    ("9", "r3_marked", "broken_localized", "marked the pic", "2026-08-28", "his in-place marks on the r3 render: rect (1080,315)-(1235,415) and ellipse c(1160,650) r(130,115) at cam A"),
]
corpus_ids = {}
for st, arm, label, quote, date, note in CORPUS:
    corpus_ids[st] = add({"kind": "verdict", "subject": "greets.wall.look",
                          "claim": f"corpus state {st} ({arm}): {note}. Quote copied VERBATIM from docs/BULGE_CORPUS.md by the coordinator during the 2026-08-29 seeding; the coordinator did not witness this line, the file did.",
                          "value": label, "contract": "look", "scope": {"scene": "greets", "defect": "wall", "state": st, "arm": arm},
                          "evidence": [ev("transcript", "docs/BULGE_CORPUS.md", f"row {st} of the corpus table")]},
                         quote=quote, quote_date=date)
# 2026-08-28 (after the ride-sign / sibling_abut rounds) — quoted in commit bc79e39d's message
corpus_ids["10"] = add({"kind": "verdict", "subject": "greets.wall.look",
                        "claim": "state 10 (ride-sign + sibling_abut defaults, rev-dispfix d3f7a1ef): the wall reads fairly correct for the first time; tears remain near it. Quote copied verbatim from the coordinator's transcript as recorded in commit bc79e39d, seeded 2026-08-29.",
                        "value": "fairly_correct_with_tears", "contract": "look",
                        "scope": {"scene": "greets", "defect": "wall", "state": "10", "arm": "ride_sign_fix"},
                        "evidence": [ev("transcript", "commit:bc79e39d", "quoted in the commit message"), ev("doc", "docs/BULGE_CORPUS.md")]},
                       quote="i'm not sure the --greets_displace_shoulder_plateau makes it better or not. but for the first time, we can see wall in a fairly correct way. I still see tears near the wall - not sure anymore if any other default flag causes it or what",
                       quote_date="2026-08-28")
v_unsure = add({"kind": "verdict", "subject": "greets.wall.look.shoulder_plateau",
                "claim": "undecided on --greets_displace_shoulder_plateau after the first pair (2026-08-28). Verbatim, seeded by the coordinator 2026-08-29.",
                "value": "unsure", "contract": "look", "scope": {"scene": "greets", "flag": "greets_displace_shoulder_plateau"},
                "evidence": [ev("transcript", "commit:bc79e39d")]},
               quote="i'm not sure the --greets_displace_shoulder_plateau makes it better or not", quote_date="2026-08-28")
corpus_ids["11"] = add({"kind": "verdict", "subject": "greets.wall.look",
                        "claim": "state 11 (the shoulder_plateau pair of 2026-08-29: C = plateau arm, B = default with holes, A = pre-round): C looks good, B has holes, A okay. Verbatim from the coordinator's transcript, seeded 2026-08-29.",
                        "value": "ok", "contract": "look",
                        "scope": {"scene": "greets", "defect": "wall", "state": "11", "arm": "shoulder_plateau_pair"},
                        "evidence": [ev("transcript", "doc:coordinator transcript 2026-08-29"), ev("image_read", "docs/img/kb/AC_camA.png")]},
                       quote="C looks good. B has holes (makes sense). A also okay", quote_date="2026-08-29")

# the decision's cost and gain claims
cost_share = add({"kind": "measurement", "subject": "greets.shoulder_plateau.px_changed_share",
                  "claim": "with --greets_displace_shoulder_plateau 73-93 % of wall pixels differ from the default arm at cam A / cam B / t=6194, mean |delta| 6/255 (the coordinator's own snapshot diff, 2026-08-29).",
                  "value": [73, 93], "contract": "share_pct",
                  "scope": {"scene": "greets", "flag": "greets_displace_shoulder_plateau", "arm": "shoulder_plateau", "metric": "px_changed_share"},
                  "evidence": [ev("snapshot_diff", "docs/img/kb/AC_camA.png"), ev("snapshot_diff", "docs/img/kb/AC_camB.png"), ev("snapshot_diff", "docs/img/kb/AC_p6194.png")],
                  "falsifier": "re-diff default vs plateau at the three poses on the tip binary"})
cost_max = add({"kind": "measurement", "subject": "greets.shoulder_plateau.max_abs_delta",
                "claim": "max |delta| 110-130 / 255 (p99 31-37) between the default and shoulder_plateau arms at cam A / cam B / t=6194 (coordinator's snapshot diff, 2026-08-29).",
                "value": 130, "eps": 10, "contract": "pixel_delta",
                "scope": {"scene": "greets", "flag": "greets_displace_shoulder_plateau", "arm": "shoulder_plateau", "metric": "max_abs_delta"},
                "evidence": [ev("snapshot_diff", "docs/img/kb/AC_camA.png"), ev("snapshot_diff", "docs/img/kb/AC_camB.png"), ev("snapshot_diff", "docs/img/kb/AC_p6194.png")],
                "falsifier": "re-diff at the three poses on the tip binary"})
bias_def = add({"kind": "measurement", "subject": "greets.wall.plateau_bias",
                "claim": "normal-space plateau bias of the default arm: plateaus sit -0.036 u behind the reference height field on every wall plane (43/45/48/49), grooves within +0.01 (tools/nspace_relief.py, rev-dispfix).",
                "value": -0.036, "contract": "world_units", "scope": {"scene": "greets", "arm": "default", "metric": "plateau_bias"},
                "evidence": [ev("battery", "commit:7c2ca8d7", "SESSION_STATE 2026-08-29a on rev-dispfix"), ev("commit", "commit:d3f7a1ef")],
                "falsifier": "nspace_relief on the default arm at cam A reading a plateau bias outside -0.036 +- 0.005",
                "provenance": {"head": "acd51087"}})
bias_shp = add({"kind": "measurement", "subject": "greets.wall.plateau_bias",
                "claim": "normal-space plateau bias with --greets_displace_shoulder_plateau: -0.018 u on every wall plane, bevel to zero (rev-dispfix SESSION_STATE 2026-08-29a).",
                "value": -0.018, "contract": "world_units", "scope": {"scene": "greets", "arm": "shoulder_plateau", "metric": "plateau_bias"},
                "evidence": [ev("battery", "commit:7c2ca8d7"), ev("commit", "commit:07cbbf91")],
                "falsifier": "nspace_relief on the plateau arm at cam A reading outside -0.018 +- 0.005",
                "provenance": {"head": "acd51087"}})
marks_def = add({"kind": "measurement", "subject": "greets.wall.marks_p90_dz",
                 "claim": "p90 |dz| inside his two marked regions (rect / ellipse, cam A) on the default arm: 0.393 / 0.503 u past the outer-mitre envelope (refdiff).",
                 "value": [0.393, 0.503], "contract": "world_units_range", "scope": {"scene": "greets", "arm": "default", "metric": "marks_p90_dz", "pose": "camA"},
                 "evidence": [ev("battery", "commit:7c2ca8d7")], "falsifier": "refdiff_detect at cam A on the tip binary",
                 "provenance": {"head": "acd51087"}})
marks_shp = add({"kind": "measurement", "subject": "greets.wall.marks_p90_dz",
                 "claim": "p90 |dz| inside his two marked regions with --greets_displace_shoulder_plateau: 0.279 / 0.358 u (protrusion fraction rises 13.4/6.7 -> 18.8/8.5 % against the mip-2 reference at grazing).",
                 "value": [0.279, 0.358], "contract": "world_units_range", "scope": {"scene": "greets", "arm": "shoulder_plateau", "metric": "marks_p90_dz", "pose": "camA"},
                 "evidence": [ev("battery", "commit:7c2ca8d7")], "falsifier": "refdiff_detect at cam A on the plateau arm",
                 "provenance": {"head": "acd51087"}})
add({"kind": "decision", "subject": "greets.wall.look.shoulder_plateau",
     "claim": "shoulder_plateau goes ON: he judged the pair (state 11) and said go. Trade recorded: cost = 73-93 % of wall pixels move at mean 6/255, max 110-130; gain = plateau bias -0.036 -> -0.018, marks p90 0.393/0.503 -> 0.279/0.358. Verbatim, seeded by the coordinator 2026-08-29.",
     "value": "on", "contract": "look", "scope": {"scene": "greets", "flag": "greets_displace_shoulder_plateau"},
     "depends_on": [x for x in (cost_share, cost_max, bias_def, bias_shp, marks_def, marks_shp) if x], "supersedes": [v_unsure] if v_unsure else [],
     "evidence": [ev("transcript", "doc:coordinator transcript 2026-08-29"), ev("doc", "docs/SHADING_CONTRACT.md")]},
    quote="go for shoulder_plateau", quote_date="2026-08-29")

# =====================================================================================
# D. detectors — refdiff is the validated gate; five earlier detectors passed a BROKEN state
# =====================================================================================
add({"kind": "law", "subject": "detector.valid",
     "claim": "refdiff (offline plane+heightfield reference, --refplane_dump + tools/refdiff_detect.py) is the validated wall-defect detector: exact 0.00 on the bare walls, fires on every era he labeled broken, orders them as he ordered them.",
     "value": "valid", "contract": "state", "scope": {"detector": "refdiff", "scene": "greets", "defect": "wall"},
     "evidence": [ev("battery", "docs/BULGE_CORPUS.md", "validation matrix"), ev("commit", "commit:6b2b0a58")],
     "falsifier": "a corpus state it mislabels (fires on an OK state or is quiet on a BROKEN one)"})
for det, what in [("gbi", "GBI normal-field bulge index (tools/bulge_detect.py)"), ("sweep", "SWEEP shading sweep metric"),
                  ("sgm", "SGM shading-vs-depth-geometry metric"), ("blackpx", "black-pixel / hole counts"),
                  ("geomxsec", "geometry cross-sections of the displaced mesh")]:
    add({"kind": "refutation", "subject": f"detector.{det}",
         "claim": f"{what} is NOT a valid wall-defect gate: it passed corpus state 5 (r3), which he labeled BROKEN; the seam-column juts it missed protrude 28.7 % / 14.9 % inside his marks under refdiff.",
         "value": "invalid_as_wall_defect_gate", "contract": "state", "scope": {"detector": det, "scene": "greets", "defect": "wall"},
         "evidence": [ev("battery", "docs/BULGE_CORPUS.md"), ev("commit", "commit:6b2b0a58")],
         "falsifier": "the detector separating every labeled corpus state on re-run"})

# =====================================================================================
# E. refutations — FeatureFlags.def flag texts + the refuted rounds of the perf window
# =====================================================================================
REFUTED = [
    ("lever.cpb", {"scene": "greets", "defect": "wall"}, "cpb does not move wall pixels (wall regions bit-identical under toggle at cams A/B); floor-only at current defaults - refuted AS A WALL LEVER.", "commit:c2d0f625", "c2d0f625",
     "a wall pixel moving under a cpb toggle at cams A/B"),
    ("flag.mirror_mask_pool_clear", {"scene": "greets", "row": "mirror-mask-clear"}, "Clearing StampMirrorMasks' 8-10 MB of planes with parallel_memset is +33 % SLOWER (serial 0.478-0.481 ms, pooled 0.617-0.639 ms, four interleaved rounds): DRAM-bandwidth-bound, extra workers add synchronisation not bandwidth. Kept default OFF as its own proof.", "commit:9dcee857", "9dcee857",
     "an interleaved battery where the pooled clear measures faster than serial"),
    ("flag.refl_skip_post", {"scene": "chase", "flag": "refl_skip_post"}, "REFUTED AS A PERF ITEM: the reflection pass's whole post chain is 1.27 ms, the flag recovers 0.66-0.94 ms of tick and costs 88.4 % of the frame at max 181/255 - worst saving-to-damage ratio on the ladder by two orders of magnitude.", "commit:1770bf51", "1770bf51",
     "a measurement showing more than ~1 ms recovered or a frame change below the visibility floor"),
    ("flag.refl_skip_rain", {"scene": "chase", "flag": "refl_skip_rain"}, "REFUTED AS A PERF ITEM: rain@refl and rain@main are 0.0000 ms at chase t=800 and t=1105 and the flag is byte-identical to base - chase has no rain armed; its apparent -0.10/-0.17 ms is the between-arm floor.", "commit:1770bf51", "1770bf51",
     "chase arming rain, after which the row is non-zero"),
    ("flag.greets_displace_free_edge.vertex_coincidence", {"scene": "greets", "flag": "greets_displace_free_edge", "defect": "wall"}, "Testing 'nothing on the far side' by VERTEX COINCIDENCE only is refuted by the t=1088 wall corner: two walls meeting geometrically without shared vertex positions both classified OPEN, both freed, both recessed - a full-height slit; the veto now probes a face soup within 0.05 u.", "commit:2b61b85f", "2b61b85f",
     "a corner where geometric abuttal and vertex coincidence agree and the slit still opens"),
    ("flag.greets_displace_crease_normals", {"scene": "greets", "flag": "greets_displace_crease_normals", "defect": "wall"}, "Crease-gated post-bake shading normals are REFUTED AS A DEFAULT: crease30 is worse than the 80-deg weld on every region (pier front GBI 3.50 vs 2.82, curved 13.74 vs 8.56, seam 84.3 vs 80.7); the panel-scale bulge was the groove-shade TARGET, not the smoothing angle.", "commit:16aac2ee", "16aac2ee",
     "a region where crease30 measures better than the 80-deg weld under refdiff"),
    ("flag.greets_displace_joint_snap", {"scene": "greets", "flag": "greets_displace_joint_snap", "defect": "wall"}, "Bed-joint snap of border verts is REFUTED AS A FIX for his marked juts by the refdiff gate, three widening variants: 23 verts -> unmoved 28.7/14.9 %, +290 cores -> unmoved, 4451 verts -> WORSE 30.0/15.8 % and the recession deepened; the protrusion is not vert levels.", "commit:929702cb", "929702cb",
     "a snap variant that moves the marks' protrusion fraction down under refdiff"),
    ("flag.greets_displace_joint_split", {"scene": "greets", "flag": "greets_displace_joint_split", "defect": "wall"}, "The joint-band edge split is REFUTED BY THE GATE in three iterations (corner exemption / full traversal / single border + snap): marks 27-29 / 15-16 % unmoved, the third worsens the ellipse; the marked faces are same-plane with corners ON the joint rows.", "commit:fcfc939c", "fcfc939c",
     "a split variant that lowers the marks' protrusion under refdiff"),
    ("flag.greets_displace_env_clamp", {"scene": "greets", "flag": "greets_displace_env_clamp", "defect": "wall"}, "The envelope vert clamp is REFUTED AS A FIX for his marked juts: it pulls 576 real off-envelope verts and changes ~78k px, but the marks are unmoved (28.7 -> 27.1 % / 14.9 -> 14.8 %); the juts are not vert positions off the envelope.", "commit:fab5b1c0", "fab5b1c0",
     "the clamp moving the marks' protrusion fraction"),
    ("flag.greets_displace_edge_vert_merge", {"scene": "greets", "flag": "greets_displace_edge_vert_merge", "defect": "tears"}, "The tolerant shared-edge vertex merge is REFUTED as a tear lever: 264 merges fire and S6120 + cam A stay byte-identical, holes unchanged 1101 / 116 - the hairline twins are minted by different creation passes.", "commit:bd754627", "bd754627",
     "a hole count moving under the merge at S6120 or cam A"),
    ("flag.greets_displace_tsplit_shared", {"scene": "greets", "flag": "greets_displace_tsplit_shared", "defect": "tears", "pose": "H6194"}, "The level-boundary T-junction stitch is REFUTED AS THE H6194 FIX: holes 5072 -> 5049 (-0.5 %) for +739 faces; the two triangles nearest 99 % of the hole pixels TOUCH (tri-tri distance 0) - not a T-junction.", "commit:c3c1ec43", "c3c1ec43",
     "H6194 holes dropping by more than a few percent under the stitch"),
    ("flag.greets_displace_tsplit_all_faces", {"scene": "greets", "flag": "greets_displace_tsplit_all_faces"}, "Applying tsplit to foreign-material faces is refuted on every axis: it closes nothing (5352 vs 5337 holes), makes 7 poses worse, +81 % faces, +140 ms bake - it was retessellating the floor from the rooms bake.", "commit:135e642a", "135e642a",
     "the unscoped variant closing holes the scoped one does not"),
    ("lever.ssao_gather_vectorise", {"scene": "chase", "row": "ssao"}, "Vectorising the GTAO march's depth gather is bit-exact and removes 9.8 % of the row's instructions - and costs 2.5 % MORE cycles: the scalar loop's eight independent iterations were already extracting ILP the vector form serialises.", "commit:aebab960", "aebab960",
     "a vector gather measuring fewer cycles than the scalar loop on an interleaved battery"),
    ("lever.cone_c6_midpoint", {"row": "cones-call"}, "C6, the midpoint closed form for the cone kernel, removes 10 vector-ALU ops and pays 29 spills: +1.41 % instructions, +5.42 % cycles, +4.97 % wall.", "commit:ef8fa5d3", "ef8fa5d3",
     "a C6 variant that does not raise spills and measures faster"),
    ("lever.gtao_acos_sqrt", {"row": "ssao"}, "The gtaoAcos sqrt substitution is a NET LOSS on every column; it came OFF Gil-Ad's decision stack rather than onto it.", "commit:cb023cab", "cb023cab",
     "the substitution measuring faster on any column"),
    ("lever.stamp_mirror_masks_parallel", {"scene": "greets", "row": "mirror-mask-clear"}, "Parallelising StampMirrorMasks' plane clears is +33 % slower (same finding as flag.mirror_mask_pool_clear, from the round that measured it).", "commit:9dcee857", "9dcee857",
     "see flag.mirror_mask_pool_clear"),
]
for subj, scope, claim, commit, head, fals in REFUTED:
    add({"kind": "refutation", "subject": subj, "claim": claim + SEED_NOTE.format(src="FeatureFlags.def / commit message"),
         "value": "refuted", "contract": "state", "scope": scope,
         "evidence": [ev("battery", commit), ev("doc", "FDS/Base/FeatureFlags.def")], "falsifier": fals,
         "provenance": {"head": head}})

# =====================================================================================
# F. PERF_STATE §00v — the window's end-to-end baselines (AFTER binary 78c0a752 + the §00v.4 fix;
#    verified on fog-wt 9b885002 per PERF_LAWS Part 3)
# =====================================================================================
BASE = [  # scene, pose, metric, value, evidence type
    ("greets", "5743", "renderFrame", 46.925, "min_of_8"), ("greets", "5965", "renderFrame", 41.516, "min_of_8"),
    ("city", "1961", "renderFrame", 47.952, "min_of_8"), ("chase", "800", "renderFrame", 36.167, "min_of_5"),
    ("chase", "1105", "renderFrame", 39.179, "min_of_5"), ("chase", "1600", "renderFrame", 22.841, "min_of_5"),
    ("greets", "5743", "tick", 55.594, "min_of_5"), ("greets", "5965", "tick", 47.242, "min_of_5"),
    ("city", "1961", "tick", 59.552, "min_of_5"),
]
for scene, pose, metric, val, et in BASE:
    add({"kind": "measurement", "subject": f"{scene}.{metric}.ms",
         "claim": f"{scene} t={pose} {metric} = {val} ms on the window's AFTER binary (default-vs-default, interleaved, dummy drivers, --profiler=0, 1920x1080); PERF_STATE 00v.1.",
         "value": val, "contract": "frame_ms", "scope": {"scene": scene, "pose": pose, "metric": metric, "arm": "default"},
         "evidence": [ev(et, "docs/PERF_STATE.md#00v.1")], "falsifier": f"an interleaved min-of-N of {scene} t={pose} on the tip outside +-1 % of this",
         "provenance": {"head": "9b885002"}})
# the +1.8 % greets row delta that the floor voids (F7)
add({"kind": "measurement", "subject": "greets.lighting_w1.tick_delta",
     "claim": "greets t=5743 lighting-w1 +1.8 % vs the window's parent (dup-arm drift +-0.1 %) - later shown to be code placement (00w), not a code change.",
     "value": 1.8, "contract": "pct_delta", "scope": {"scene": "greets", "pose": "5743", "row": "lighting-w1", "granularity": "kernel_row", "metric": "delta_pct"},
     "evidence": [ev("min_of_5", "docs/PERF_STATE.md#00v.2"), ev("commit", "commit:3cff1085")], "falsifier": "an arm with a mnemonic-identical kernel that does NOT carry the delta",
     "provenance": {"head": "9b885002"}})
add({"kind": "law", "subject": "perf.floor",
     "claim": "a single kernel row's between-binary delta carries a +-1.5-2 % code-placement floor: 24 bytes of inert padding moved greets lighting-w1 by 1.21 %, and an arm with the parent's exact 5359-instruction kernel still carried +1.08 % (00w).",
     "value": 2.0, "contract": "pct_delta", "scope": {"granularity": "kernel_row", "metric": "delta_pct"},
     "evidence": [ev("min_of_11_interleaved", "docs/PERF_STATE.md#00w"), ev("commit", "commit:3cff1085")],
     "falsifier": "inert padding or a mnemonic-identical arm measuring a kernel row within +-0.5 % across three batteries",
     "provenance": {"head": "3cff1085"}})

# =====================================================================================
# G. the pins (14 poses across 7 recipes) + render_gate's 4 rows, byte-null at tip 3cff1085 / 2d4b33df
# =====================================================================================
PINS = [
    ("city_deferred", "city", "1961", "bd4ffbf87d1492175a9b6c1111fb3f5f"),
    ("greets_t1588", "greets", "1588", "570a7b443f768393dc6647044a9e67b3"),
    ("greets_acceptance", "greets", "5743", "440aa6bbb350ae95fbacf339dd2ad957"),
    ("greets_acceptance", "greets", "2845", "00d17bc5610624bd1fea698c4b096979"),
    ("greets_acceptance", "greets", "6097", "29c1e7fbd30e9ef811588a63d0778b7b"),
    ("greets_acceptance", "greets", "6133", "bc1b0a8a703d6d6f6b3953eafc864d48"),
    ("fountain", "fountain", "2500", "8db68ccb59416e9a44037e9f387b7bd9"),
    ("chase_default", "chase", "100", "b67b47f0de8b41365f96fff68e50d367"),
    ("chase_default", "chase", "400", "5bc199d4949a6212b4b7cb1004ab0e3a"),
    ("chase_default", "chase", "800", "d1284b5a727bb6c5924b6ba3012f89ae"),
    ("chase_default", "chase", "1200", "9c0f7c2fac7b8a1408f62110bb70d12f"),
    ("chase_default", "chase", "1600", "9cdf5603f231392e64000ed2b850877a"),
    ("chase_cinematic", "chase", "800", "d50a32d33f23a6de505257b663dbdc62"),
    ("chase_cinematic", "chase", "1600", "92ffa25d675a716c6809a7db133c3961"),
]
for recipe, scene, pose, md5 in PINS:
    add({"kind": "measurement", "subject": f"pin.{recipe}",
         "claim": f"snapshot pin {recipe} t={pose}: recipe in the SESSION_STATE gates table; reproduced at its recorded value on both binaries at tip 3cff1085 (WCELL round) and byte-null through 2d4b33df.",
         "value": md5, "contract": "snapshot_md5", "scope": {"scene": scene, "pose": pose, "recipe": recipe},
         "evidence": [ev("md5_pin", "docs/PERF_STATE.md#gates-3cff1085"), ev("md5_pin", "docs/SESSION_STATE.md#gates")],
         "falsifier": "the recipe rendered verbatim (pose sequence, env, no --profiler on chase) on the tip giving another md5, 3/3",
         "provenance": {"head": "2d4b33df"}})
for row, md5 in [("mirrortest", "4ac809e5f5323076de1a6d5ef2fb9e92"), ("rttslot", "826c09e63217e778cfcef70fe0167279"),
                 ("conetest", "b41894f969d1f89dd2d7d794f160e286"), ("halotest", "166fa25a846668cc9b2d4dae2d800a7b")]:
    add({"kind": "measurement", "subject": "pin.render_gate",
         "claim": f"tools/render_gate.sh row {row} baseline (resolution-specific: stock rev.cfg 1920x1080; the script cannot pass in a tree whose rev.cfg is at his window size).",
         "value": md5, "contract": "snapshot_md5", "scope": {"recipe": "render_gate", "pose": row},
         "evidence": [ev("md5_pin", "tools/render_gate.sh")], "falsifier": "render_gate.sh at stock rev.cfg on the tip reporting another hash",
         "provenance": {"head": "2d4b33df"}})

# =====================================================================================
# H. the two known corrections, as supersession pairs with the plans that depended on them
# =====================================================================================
m366 = add({"kind": "measurement", "subject": "greets.lighting_w1.shadow_share",
            "claim": "the cube tap is 36.6 % of greets lighting-w1 (PERF_STATE §2, carried for eleven rounds).",
            "value": 36.6, "contract": "share_pct", "scope": {"scene": "greets", "row": "lighting-w1", "pose": "5780"},
            "evidence": [ev("single_run", "docs/PERF_STATE.md#L2241")], "falsifier": "re-take the share with the profiler at a judging pose",
            "provenance": {"head": "unknown-pre-2026-08-29"}})
add({"kind": "proposal", "subject": "plan.greets.cube_tap_cull",
     "claim": "per-(8x8 block x light) cube-tap cull, sized at 2.3-5.3 ms off the shadow share; needs a hi-Z pyramid over ZPage16 and a block-major restructure (OPTIMIZATION_BACKLOG 2026-08-29d).",
     "scope": {"scene": "greets", "row": "lighting-w1"}, "depends_on": [m366] if m366 else [],
     "evidence": [ev("doc", "docs/OPTIMIZATION_BACKLOG.md#L233")]})
add({"kind": "proposal", "subject": "plan.greets.shadow_tiling",
     "claim": "shadow-map memory layout / tiling plan, parked for a shadow-bottleneck campaign, sized on the shadow share (docs/SHADOWMAP_TILING_PLAN.md).",
     "scope": {"scene": "greets", "row": "lighting-w1"}, "depends_on": [m366] if m366 else [],
     "evidence": [ev("doc", "docs/SHADOWMAP_TILING_PLAN.md")]})
add({"kind": "measurement", "subject": "greets.lighting_w1.shadow_share",
     "claim": "ALL shadow work is 16-28 % of greets lighting-w1, pose-dependent; the 36.6 % was one pose on an older binary and every plan sized off it aimed at twice the prize (round 12 census, no code landed).",
     "value": [16, 28], "contract": "share_pct", "scope": {"scene": "greets", "row": "lighting-w1", "pose": "5743"},
     "supersedes": [m366] if m366 else [], "evidence": [ev("battery", "commit:63f0b8f1"), ev("doc", "docs/OPTIMIZATION_BACKLOG.md#L233")],
     "falsifier": "a profiler census at t=5743/5965 on the tip outside 16-28 %", "provenance": {"head": "63f0b8f1"}})
m2ms = add({"kind": "measurement", "subject": "city.tick_refl_xfrm.refl_correct_cost",
            "claim": "the --refl_correct commission costs ~2 ms/frame (2026-08-17 round, §00j, quoted at one pose against a larger between-binary noise).",
            "value": 2.0, "eps": 0.3, "contract": "frame_ms", "scope": {"scene": "city", "row": "Tick-ReflXfrm", "flag": "refl_correct"},
            "evidence": [ev("single_run", "docs/PERF_STATE.md#00j")], "falsifier": "a same-binary flag flip of --refl_correct on the row",
            "provenance": {"head": "unknown-2026-08-17"}})
add({"kind": "proposal", "subject": "plan.city.refl_correct_cost_cut",
     "claim": "cut the --refl_correct commission's per-frame cost (carried since 2026-08-17 as the row's owner).",
     "scope": {"scene": "city", "row": "Tick-ReflXfrm"}, "depends_on": [m2ms] if m2ms else [],
     "evidence": [ev("doc", "docs/OPTIMIZATION_BACKLOG.md#L1044")]})
add({"kind": "measurement", "subject": "city.tick_refl_xfrm.refl_correct_cost",
     "claim": "same-binary flag flip: Tick-ReflXfrm 1.952 ms with --refl_correct, 1.888 without - the commission costs 0.064 ms, 3.3 % of the row, ~30x less than on record.",
     "value": 0.064, "contract": "frame_ms", "scope": {"scene": "city", "row": "Tick-ReflXfrm", "flag": "refl_correct"},
     "supersedes": [m2ms] if m2ms else [], "evidence": [ev("battery", "commit:dcaddffa"), ev("doc", "docs/PERF_STATE.md#00q")],
     "falsifier": "the same flag flip on the tip measuring more than 0.2 ms", "provenance": {"head": "dcaddffa"}})

# =====================================================================================
# I. open items: reported bugs, the default-OFF menu awaiting his eye, the wall remainder
# =====================================================================================
OPENS = [
    ("bug.lightSphereScreenRect.drops_light", {"scene": "city"}, "lightSphereScreenRect's small-angle form is not conservative: 6 of 39 swept poses change, always brighter, max 5/255 in tile-boundary slivers - real, below the visibility floor, reported not fixed (--light_rect_exact OFF prices it).", "commit:0653da84"),
    ("bug.chase.zenc_sky_classifier", {"scene": "chase"}, "chase's sky is painted by the reflection pass through a single-class zEnc==0 classifier (00m).", "docs/PERF_STATE.md#00m"),
    ("greets.wall.remainder", {"scene": "greets", "defect": "tears"}, "5337 holes remain after the tears round, one world feature (x 17.85-17.91, z -62.95..-63.40): 54 % a missing 0.44 u wedge from two vertex families at one authored (x,y); 46 % raster/cull loss with winding exonerated. Next: find the pass minting the second family; a per-face binned census in the tiled rasteriser.", "commit:e60240b3"),
    ("menu.cone_half_y_wide", {"scene": "city", "flag": "cone_half_y_wide"}, "half-Y stepping for wide-cone tiles: -6.5 ms (46.7 % of the cone pass) for 0.37 % of pixels at max 5/255 - default OFF, his eye decides.", "commit:005a0823"),
    ("menu.water_glints_batch", {"scene": "chase", "flag": "water_glints_batch"}, "batched water glints: up to -11 % of a chase tick, pixels move by 1 LSB - default OFF, his eye decides.", "commit:62aec5b2"),
    ("menu.refl_skip_cones_or_ssao", {"scene": "chase", "flag": "refl_skip_cones"}, "skipping cones (-6.41 ms, max 6/255) or SSAO (-3.67 ms, max 119/255) inside chase's reflection pass: the ms column and the eye rank them in opposite orders - default OFF, his eye decides.", "commit:60f87341"),
    ("menu.light_rect_exact", {"flag": "light_rect_exact"}, "exact light screen rect (fixes bug.lightSphereScreenRect.drops_light at a cost) - default OFF, priced, awaiting a call.", "commit:0653da84"),
]
for subj, scope, claim, ref in OPENS:
    add({"kind": "open", "subject": subj, "claim": claim + SEED_NOTE.format(src="commit messages / PERF_STATE"), "scope": scope,
         "evidence": [ev("commit" if ref.startswith("commit:") else "doc", ref)]})

# =====================================================================================
# J. the GLOBAL store — traps that transfer between projects (default generic contracts of ~/.groundwork)
# =====================================================================================
GLOBAL_TRAPS = [
    ("tool.pgrep.self_match", {"tool": "pgrep"}, "pgrep -f matches the shell that runs it: box-quiet wait loops that grep their own command line deadlock on each other. Match executables (ps -Ao comm=), not argv.", "doc:revival-fog PERF_LAWS T6"),
    ("shell.zsh.flag_string_splitting", {"shell": "zsh"}, "zsh does not word-split an unquoted variable: a flag string in $VAR is one argument; use ${=VAR}. Also never echo a bare line of === in a script that feeds a parser.", "doc:revival-fog DISPFIX_HANDOFF pitfalls"),
    ("measure.min_of_n_interleaved_with_duplicate_arm", {}, "Perf A/B: interleave the arms, rotate order, take the min of N (11), and run a DUPLICATE of one arm as the contamination detector - a between-arm delta smaller than the duplicate's drift is not a finding.", "doc:revival-fog PERF_STATE 00v/00w method"),
    ("measure.printf_hides_races", {}, "printf/stderr in a hot threaded path serialises the workers and HIDES the race you are hunting (heisenbug); record lock-free per-worker and dump at the end, or build-bisect.", "doc:revival-fog measurement-tool-traps (1e91306)"),
    ("measure.two_run_md5_proves_nothing", {}, "A 2-run byte-identical gate cannot clear a threading race; determinism needs 24+ identical runs (md5 | sort | uniq -c) and a binomial before believing a clean batch (P(24 clean at 1-in-12) ~ 0.12).", "doc:revival-fog measurement-tool-traps (5f325d4)"),
]
for subj, scope, claim, ref in GLOBAL_TRAPS:
    add({"kind": "trap", "subject": subj, "claim": claim + SEED_NOTE.format(src="revival-fog"), "value": "trap", "contract": "exact",
         "scope": scope, "evidence": [ev("reference", ref)],
         "falsifier": "a documented case where following the advice measured wrong"}, to_global=True)

# =====================================================================================
print(f"added {len(added)} records; {len(failed)} could not be written")
for subj, errs in failed:
    print(f"  FAILED {subj}:")
    for e in errs:
        print(f"      - {e}")
sys.exit(1 if failed else 0)
