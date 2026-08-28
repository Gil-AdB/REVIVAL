#!/bin/zsh
# tear_battery.sh <outroot> <armname> <armflags...>   — renders every pose of
# docs/tears_poses.txt under the given arm with the reference dumps enabled.
# POSES=<regex> filters pose ids; PAR=<n> parallel slots (default 3).
set -u
OUT=$1; ARM=$2; shift 2; FLAGS="$*"
WT=/Users/gil-ad/work/rev-dispfix
PAR=${PAR:-3}; POSES=${POSES:-.*}
run_one() {
  local id=$1 t=$2 cam=$3
  local d=$OUT/${ARM}_$id
  if [[ -n "$(ls $d/ 2>/dev/null | grep refplane)" ]]; then return 0; fi
  mkdir -p $d
  cd $WT/Runtime
  if [[ $cam == "-" ]]; then
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy FDS_SNAPSHOT_ZDUMP=1 FDS_SNAPSHOT_GBUFDUMP=1 \
      ./DEMO --snapshot=greets@t=$t --out=$d --bulge_dump --refplane_dump ${=FLAGS} >$d/stderr.log 2>&1
  else
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy FDS_SNAPSHOT_ZDUMP=1 FDS_SNAPSHOT_GBUFDUMP=1 FDS_GREETS_CAM="$cam" \
      ./DEMO --snapshot=greets@t=$t --out=$d --bulge_dump --refplane_dump ${=FLAGS} >$d/stderr.log 2>&1
  fi
  echo "done $ARM $id $(date +%H:%M:%S)"
}
n=0
grep -v '^#' $WT/docs/tears_poses.txt | grep -E "^($POSES) " | while read id t cam; do
  run_one $id $t $cam &
  n=$((n+1)); if (( n % PAR == 0 )); then wait; fi
done
wait
echo "battery $ARM complete $(date +%H:%M:%S)"
