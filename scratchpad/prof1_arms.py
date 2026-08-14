#!/usr/bin/env python3
"""prof1_arms.py — emit the arm JSON files for the round-1 frame-time map.

Every arm is an explicit argv list; nothing is assembled from shell strings.
  python3 prof1_arms.py <outdir>
"""
import json, os, sys

OUT = sys.argv[1] if len(sys.argv) > 1 else "."

COMMON = ["--deferred_prof=1", "--hw_prof", "--strict_flags", "--init_timeline"]
DISP = ["--greets_displace"]
DISP_FULL = DISP + ["--greets_displace_free_edge",
                    "--greets_displace_border_mean=2",
                    "--greets_displace_seam_weld"]
HIS_CAM = "-8.6249094,2.72651696,-53.2339516,0.210607708,0.0055912463,-0.977554619"


def bench(scene, t, iters=20, xres=1920, yres=1080):
    return "--bench=scene@scene=%s,t=%d,iters=%d,xres=%d,yres=%d" % (scene, t, iters, xres, yres)


def arm(tag, args, env=None):
    return {"tag": tag, "args": args, "env": env or {}}


def write(name, arms):
    p = os.path.join(OUT, name)
    json.dump(arms, open(p, "w"), indent=1)
    print("wrote %s (%d arms)" % (p, len(arms)))


# ── 1. greets, the displacement/tessellation matrix, three poses ─────────────
for pose, (t, xr, yr, env) in {
        "t5743": (5743, 1920, 1080, {}),
        "his":   (3122, 1512, 848, {"FDS_GREETS_CAM": HIS_CAM}),
        "t6001": (6001, 1920, 1080, {}),
}.items():
    b = bench("greets", t, 20, xr, yr)
    write("arms_greets_%s.json" % pose, [
        arm("flat",         [b] + COMMON, env),
        arm("disp",         [b] + COMMON + DISP, env),
        arm("dispfull",     [b] + COMMON + DISP_FULL, env),
        arm("dispfull_mip", [b] + COMMON + DISP_FULL + ["--mip_aniso", "--texture_filter=1"], env),
    ])

# ── 2. city ─────────────────────────────────────────────────────────────────
# `--deferred` is REQUIRED here. greets forces the deferred path from inside the
# scene (greets_mirror -> Render(ForceDeferred)), city and fountain do not, so a
# bench without the flag silently profiles the FORWARD renderer — no cones, no
# DeferredLighting, no fastfog. Caught by the missing phases, 2026-08-14.
write("arms_city.json", [
    arm("t1961", [bench("city", 1961, 20), "--deferred"] + COMMON),
    arm("t2400", [bench("city", 2400, 20), "--deferred"] + COMMON),
    arm("t400",  [bench("city", 400, 20), "--deferred"] + COMMON),
])

# ── 3. chase + fountain ─────────────────────────────────────────────────────
# chase has no `--bench=scene` arm ("scene 'chase' not supported") and no
# `--repro` wiring, so it is profiled by asking the SNAPSHOT harness for the
# same timestamp ten times: the driver re-ticks and re-renders each one, and
# --deferred_prof's warmup exclusion drops the cold first frame exactly as it
# does under --bench.
def chasesnap(t, n=10):
    return ["--snapshot=chase@t=" + ",".join([str(t)] * n),
            "--out=/tmp/p1/chasesnap", "--deferred"]


write("arms_chase_fountain.json", [
    arm("chase_t800",     chasesnap(800) + COMMON),
    arm("chase_t1600",    chasesnap(1600) + COMMON),
    arm("fountain_t2500", [bench("fountain", 2500, 20), "--deferred"] + COMMON),
    arm("fountain_t1200", [bench("fountain", 1200, 20), "--deferred"] + COMMON),
])

# ── 4. the shatter frame ────────────────────────────────────────────────────
# The recorded 11.5 ms anchor is the SHARD BAKE WALL on the SECOND shatter frame
# (frame 1 is the cold bake) under exactly this bracket — SESSION_STATE
# 2026-08-12 "THE CAMPAIGN'S HEADLINE NUMBER, FINAL".
SHAT = {"FDS_GREETS_SHATTER": "1", "FDS_GREETS_CAM": "28.8,10.8,-62.85,1,0,0",
        "FDS_SHARD_REFL_PROF": "1"}
REPRO = ["--repro=greets@t=3122", "--repro_from=3112", "--repro_settle=0"]
write("arms_shatter.json", [
    arm("shatter_flat", REPRO + COMMON, SHAT),
    arm("shatter_disp", REPRO + COMMON + DISP_FULL, SHAT),
])

# ── 4b. ablation INSIDE the greets lighting wave ────────────────────────────
# `lighting-w1` is one inlined monolith, so per-symbol attribution bottoms out
# there (docs/HW_PROFILING.md §9). Staged `--prof_*` gates are the instrument
# that goes deeper; this re-runs PERF_STATE §0's split on today's tree, where
# the shadow diet and the packed shadow planes have since landed.
for tag, t, xr, yr, env in (("t5743", 5743, 1920, 1080, {}),
                            ("his", 3122, 1512, 848, {"FDS_GREETS_CAM": HIS_CAM})):
    b = bench("greets", t, 20, xr, yr)
    write("arms_ablate_%s.json" % tag, [
        arm("base",            [b] + COMMON, env),
        arm("no_lights",       [b] + COMMON + ["--prof_no_lights"], env),
        arm("no_spec",         [b] + COMMON + ["--prof_no_spec"], env),
        arm("no_tex",          [b] + COMMON + ["--prof_no_tex"], env),
        arm("no_shadows",      [b] + COMMON + ["--no-shadows"], env),
        arm("no_cube_tap",     [b] + COMMON + ["--prof_no_cube_tap"], env),
        arm("polyid_no_pcf",   [b] + COMMON + ["--shadow_polyid_no_pcf"], env),
        arm("no_env_refl",     [b] + COMMON + ["--no-env_refl"], env),
    ])

# ── 5. per-flag deltas inside the displacement family (t=5743) ──────────────
b = bench("greets", 5743, 20)
write("arms_flagcost.json", [
    arm("disp",              [b] + COMMON + DISP),
    arm("disp_freeedge",     [b] + COMMON + DISP + ["--greets_displace_free_edge"]),
    arm("disp_bmean2",       [b] + COMMON + DISP + ["--greets_displace_border_mean=2"]),
    arm("disp_seamweld",     [b] + COMMON + DISP + ["--greets_displace_seam_weld"]),
    arm("disp_nogroove",     [b] + COMMON + DISP + ["--no-greets_displace_groove_shade"]),
    arm("disp_nomitre",      [b] + COMMON + DISP + ["--no-greets_displace_mitre"]),
    arm("dispfull",          [b] + COMMON + DISP_FULL),
    arm("dispfull_aniso",    [b] + COMMON + DISP_FULL + ["--mip_aniso"]),
    arm("dispfull_tf1",      [b] + COMMON + DISP_FULL + ["--texture_filter=1"]),
])

# ── 6. the COMMIT A/B — two binaries, one asset tree ────────────────────────
# A one-binary flag A/B prices the flag, not the commit (docs/HW_PROFILING.md
# §10). Build the parent in the same worktree, park both binaries OUTSIDE the
# repo, run them interleaved from this Runtime/:
#
#   cp build/DEMO/DEMO /tmp/bin_head
#   git checkout -q <parent> && cmake --build build && cp build/DEMO/DEMO /tmp/bin_parent
#   git checkout -q <head>   && cmake --build build
#   codesign -f -s - /tmp/bin_parent /tmp/bin_head
#
# Verify `git diff <parent> <head> -- Runtime/` is EMPTY first, or it is not a
# pure code A/B.
PARENT, HEAD = "/tmp/bin_parent", "/tmp/bin_head"
ab = []
for tag, args, env in (
        ("flat5743",     [bench("greets", 5743)] + COMMON, {}),
        ("disp5743",     [bench("greets", 5743)] + COMMON + DISP, {}),
        ("dispfull6001", [bench("greets", 6001)] + COMMON + DISP_FULL, {}),
        ("his_disp",     [bench("greets", 3122, 20, 1512, 848)] + COMMON + DISP,
                         {"FDS_GREETS_CAM": HIS_CAM}),
        ("city1961",     [bench("city", 1961), "--deferred"] + COMMON, {}),
):
    ab.append(dict(arm("P_" + tag, args, env), bin=PARENT))
    ab.append(dict(arm("H_" + tag, args, env), bin=HEAD))
write("arms_ab_commit.json", ab)

# The shatter bracket needs NINE rounds: at min-of-6 it reads 16.5 ms and at
# min-of-8 it reads 11.7 ms against an 11.5 ms anchor. Measured 2026-08-14.
write("arms_ab_shatter.json", [
    dict(arm("P_shatter", REPRO + COMMON, SHAT), bin=PARENT),
    dict(arm("H_shatter", REPRO + COMMON, SHAT), bin=HEAD),
])
