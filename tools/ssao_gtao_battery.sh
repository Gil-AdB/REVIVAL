#!/bin/bash
# Full CPU-vs-GPU GTAO battery: for each pose, four renders (CPU/GPU x AO on/off)
# plus the two localisation arms (--ssao_ref with the CPU's own planes; and the
# same against the CPU's SCALAR reference path, FDS_SSAO_NOSIMD=1).
set -e
# Repo root: override with GTAO_ROOT= if the worktree moved.
ROOT=${GTAO_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
OUT=/tmp/gtao
cd $ROOT/Runtime
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
FOV=58.1092072
SS="--ssao --ssao-gtao"
EXTRA="$EXTRA_FLAGS"

run_pose() {
  local TAG=$1 T=$2 CAM=$3
  mkdir -p $OUT/$TAG
  local G="../build-gpu/GpuBench/GpuBench --fld=SCENES/GREETS.FLD --t=$T --cam=$CAM \
    --pass=deferred --xres=1920 --yres=1080 --iters=1 --warmup=0"
  local C="../build-gpu/DEMO/DEMO --snapshot=greets@t=$T --deferred --hdr --hdr-linear \
    --texture-filter=2 --profiler=0"

  $G --out=$OUT/$TAG/gpu_noao.ppm                              >$OUT/$TAG/gpu_noao.log 2>&1
  $G --out=$OUT/$TAG/gpu_ao.ppm $SS $EXTRA \
     --ssao_dump=$OUT/$TAG/gpu_ao.f32                          >$OUT/$TAG/gpu_ao.log 2>&1

  FDS_GREETS_CAM=$CAM FDS_GREETS_FOV=$FOV \
    $C --out=$OUT/$TAG/cpu_noao                                >$OUT/$TAG/cpu_noao.log 2>&1
  FDS_GREETS_CAM=$CAM FDS_GREETS_FOV=$FOV FDS_SSAO_DUMP_PATH=$OUT/$TAG/cpu_ao.f32 \
    $C --out=$OUT/$TAG/cpu_ao $SS --ssao_dump $EXTRA           >$OUT/$TAG/cpu_ao.log 2>&1
  # CPU scalar reference path (FDS_SSAO_NOSIMD=1): fast_rsqrt with its NR step,
  # instead of the shipped 8-wide path's bare _mm256_rsqrt_ps.
  FDS_SSAO_NOSIMD=1 FDS_GREETS_CAM=$CAM FDS_GREETS_FOV=$FOV \
    FDS_SSAO_DUMP_PATH=$OUT/$TAG/cpu_ao_scalar.f32 \
    $C --out=$OUT/$TAG/cpu_ao_scalar $SS --ssao_dump $EXTRA    >$OUT/$TAG/cpu_scalar.log 2>&1

  # LOCALISATION: GPU AO driven by the CPU's own depth + geometric normal.
  $G --out= $SS $EXTRA --ssao_ref=$OUT/$TAG/cpu_ao.f32 \
     --ssao_dump=$OUT/$TAG/gpu_ref.f32                         >$OUT/$TAG/gpu_ref.log 2>&1
  $G --out= $SS $EXTRA --ssao_ref=$OUT/$TAG/cpu_ao_scalar.f32 \
     --ssao_dump=$OUT/$TAG/gpu_ref_scalar.f32                  >$OUT/$TAG/gpu_ref_scalar.log 2>&1
  echo "done $TAG"
}

run_pose "${1}" "${2}" "${3}"
