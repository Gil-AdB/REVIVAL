#!/bin/zsh
# Cushion-fix constraint battery: each pose in ON (default umbrella) and OFF
# (--no-block_level --no-geom_bisector == parent, proved byte-exact) arms.
cd "$(dirname "$0")/../Runtime" || exit 1
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
OUT=/tmp/xsec/battery
mkdir -p "$OUT"
while IFS='|' read -r t cam name; do
  for arm in on off; do
    d="$OUT/${name}_$arm"
    mkdir -p "$d"
    extra=""
    [ "$arm" = "off" ] && extra="--no-greets_displace_block_level --no-greets_displace_geom_bisector"
    FDS_GREETS_CAM="$cam" ./DEMO --snapshot=greets@t=$t --out=$d \
      --deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao \
      --greets-displace --profiler=0 ${=extra} > "$d/log.txt" 2>&1
    echo "$name $arm rc=$?"
  done
done <<'EOF'
6001|20.4873486,3.21136093,-59.2149506,-0.934701741,-0.0668740645,0.349085331|t6001
6039|22.6031151,3.21344829,-59.4865112,-0.965923965,-0.0604484417,0.251668096|t6039
5975|19.1424026,3.21037412,-58.9481239,-0.903572917,-0.0723865554,0.422275066|t5975
1088|-7.06931162,4.96647167,-29.4504185,-0.580759645,0.16690442,-0.796781898|t1088
5799|10.9715099,3.19996166,-55.1480103,-0.418409944,-0.119019777,0.900426149|t5799
5869|14.0997791,3.20612359,-57.1331596,-0.67585206,-0.0996474698,0.730270028|t5869
5929|16.989603,3.20967865,-58.3003693,-0.82907784,-0.0831432045,0.552916884|t5929
5967|18.752037,3.21019745,-58.8513527,-0.892443955,-0.0741753578,0.445018977|t5967
5987|19.7497902,3.21076035,-59.0800819,-0.918940723,-0.0697668344,0.388175935|t5987
EOF
echo BATTERY-DONE
