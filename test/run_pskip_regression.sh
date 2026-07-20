#!/bin/sh
set -eu

cd "$(dirname "$0")"

make

# Remux a raw Annex-B .h264 elementary stream into an .mp4 so players get real
# PTS/framerate (bare .h264 has no container timestamps -> players warn and guess).
# Best-effort: never fail the correctness suite on a muxing/ffmpeg hiccup.
FPS="30000/1001"
remux() {
    for es in "$@"; do
        [ -f "$es" ] || continue
        ffmpeg -y -loglevel error -r "$FPS" -i "$es" -c:v copy "${es%.h264}.mp4" 2>/dev/null || true
    done
}

run_case() {
    name=$1
    shift
    echo "== pskip regression: $name =="
    ./test_pskip "$@" -o "reg_${name}"
    remux "reg_${name}_reference.h264" "reg_${name}_pskip.h264" "reg_${name}_baseline.h264"
}

# Correctness scenarios (textured content + fatal MV-invariant check).
# Resolutions span HD-ready (1280x720) up to ~2.5K (2560x1440); all dims %16.
run_case all_420_1280x720              -s all            -c 420 -w 1280 -H 720  -n 150
run_case edge_boxes_420_1280x720       -s edge-boxes     -c 420 -w 1280 -H 720  -n 150
run_case interior_boxes_420_1280x720   -s interior-boxes -c 420 -w 1280 -H 720  -n 150
run_case strips_420_1280x720           -s strips         -c 420 -w 1280 -H 720  -n 150
run_case chessboard_420_1280x720       -s chessboard     -c 420 -w 1280 -H 720  -n 150
run_case block3_420_1280x720           -s block3         -c 420 -w 1280 -H 720  -n 150
run_case block4_420_1280x720           -s block4         -c 420 -w 1280 -H 720  -n 150
run_case bands_420_1280x720            -s bands          -c 420 -w 1280 -H 720  -n 150
run_case cols_420_1280x720             -s cols           -c 420 -w 1280 -H 720  -n 150
run_case solid_interior_420_1280x720   -s solid-interior -c 420 -w 1280 -H 720  -n 150
run_case isolated_420_1280x720         -s isolated       -c 420 -w 1280 -H 720  -n 150
run_case lshape_420_1280x720           -s lshape         -c 420 -w 1280 -H 720  -n 150
run_case toprow_420_1280x720           -s toprow         -c 420 -w 1280 -H 720  -n 150
run_case leftcol_420_1280x720          -s leftcol        -c 420 -w 1280 -H 720  -n 150
run_case corner_420_1280x720           -s corner         -c 420 -w 1280 -H 720  -n 150
run_case combo_420_1280x720            -s combo          -c 420 -w 1280 -H 720  -n 150
# FHD (1920x1088) coverage
run_case combo_420_1920x1088           -s combo          -c 420 -w 1920 -H 1088 -n 150
run_case block4_420_1920x1088          -s block4         -c 420 -w 1920 -H 1088 -n 150
run_case edge_boxes_420_1920x1088      -s edge-boxes     -c 420 -w 1920 -H 1088 -n 150
# ~2.5K (2560x1440) coverage
run_case combo_420_2560x1440           -s combo          -c 420 -w 2560 -H 1440 -n 150
run_case solid_interior_420_2560x1440  -s solid-interior -c 420 -w 2560 -H 1440 -n 150
# 4:4:4 coverage across the range
run_case edge_boxes_444_1280x720       -s edge-boxes     -c 444 -w 1280 -H 720  -n 150
run_case block4_444_1920x1088          -s block4         -c 444 -w 1920 -H 1088 -n 150
run_case chessboard_444_2560x1440      -s chessboard     -c 444 -w 2560 -H 1440 -n 150

# Engagement proof: a deliberately wrong hint must visibly force a skip.
echo "== pskip engagement (negative test) =="
./test_pskip -N -s none -c 420 -w 1280 -H 720 -n 150 -o reg_negative

echo "pskip regression suite passed"
