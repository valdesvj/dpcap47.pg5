#!/bin/sh
# shots.sh SIMDIR PYTHON - the PC simulator's screens -> SIMDIR/run/*.png (make shots). Look at every one.
S=$1; PY=${2:-python3}
rm -rf $S/run && mkdir -p $S/run/simdisk
cd $S/run || exit 1
# the plot (computed at the start), the table, LIVE (the limit found, a thruster off, a group lost), the map,
# the vessel, printing (IR, into print.pbm / print.txt), the menu and about
timeout 120 ../sim WAIT:300 SNAP:plot F1 SNAP:plot_full F1 F2 SNAP:table DOWN SNAP:table2 \
    F3 SNAP:live ENTER SNAP:live_limit 4 SNAP:live_off 0 DOT SNAP:live_fail 0 \
    F4 SNAP:map0 ENTER SNAP:map F5 SNAP:vessel F6 SNAP:printed \
    F1 DOT SNAP:abs_plot F2 SNAP:abs_table \
    EXIT SNAP:menu 5 SNAP:about EXIT EXIT SNAP:back || exit 1
test -f simdisk/DPCAP47/EXAMPLE.DPV || { echo "shots: the example vessel file was not written"; exit 1; }
test -s print.pbm || { echo "shots: nothing printed"; exit 1; }
grep -q "method=1" simdisk/DPCAP47/DPCAP47.CFG || { echo "shots: the method was not saved"; exit 1; }
for n in plot plot_full table table2 live live_limit live_off live_fail map0 map vessel printed menu about back \
         abs_plot abs_table; do
  $PY ../../../test/sim/render.py $n $n.png || exit 1
done
$PY - <<'PY'
from PIL import Image
lines = open("print.pbm").read().split("\n")
w, h = map(int, lines[1].split())
img = Image.new("L", (w, h), 255)
for y in range(h):
    for x in range(w):
        if lines[2 + y][x] == "1":
            img.putpixel((x, y), 0)
img.resize((w * 2, h * 2)).save("print.png")
PY
echo "shots: $(pwd)/*.png"
