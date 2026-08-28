#!/bin/zsh
# tear_bisect.sh <outroot> <poses-regex>  — renders the flag-bisection arms
# (every default-ON greets displacement sub-flag and every branch fix toggled
# OFF one at a time, plus the shading-side controls) at the given poses.
set -u
OUT=$1; POSES=$2
HIS="--deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao"
D="--greets-displace"
typeset -A ARMS
ARMS=(
  no_edge            "$HIS $D --no-greets_displace_edge"
  no_seam_union      "$HIS $D --no-greets_displace_seam_union"
  no_fold_relax      "$HIS $D --no-greets_displace_fold_relax"
  no_shadow_planes   "$HIS $D --no-greets_displace_shadow_planes"
  no_offscreen_skip  "$HIS $D --no-greets_displace_offscreen_skip"
  no_neighbor_pin    "$HIS $D --no-greets_displace_neighbor_pin"
  no_line_height     "$HIS $D --no-greets_displace_line_height"
  no_border_pin      "$HIS $D --no-greets_displace_border_pin"
  no_front_orient    "$HIS $D --no-greets_displace_front_orient"
  no_groove_front    "$HIS $D --no-greets_displace_groove_front_majority"
  no_corner_ride     "$HIS $D --no-greets_displace_corner_front_ride"
  no_plane_front     "$HIS $D --no-greets_displace_plane_front_majority"
  no_groove_shade_pl "$HIS $D --no-greets_displace_groove_shade_plane"
  smooth0            "$HIS $D --greets_displace_smooth=0"
  adapt0             "$HIS $D --greets_displace_adapt=0"
  no_parallax        "$HIS $D --no-parallax"
  no_ssao            "--deferred --hdr --hdr-linear --texture-filter=2 $D"
  no_hdr             "--deferred --texture-filter=2 --ssao --ssao-gtao $D"
  seam_weld          "$HIS $D --greets_displace_seam_weld"
  free_edge          "$HIS $D --greets_displace_free_edge"
)
for name in ${(k)ARMS}; do
  POSES=$POSES PAR=${PAR:-3} /Users/gil-ad/work/rev-dispfix/tools/tear_battery.sh $OUT $name ${=ARMS[$name]}
done
echo "bisect complete $(date +%H:%M:%S)"
