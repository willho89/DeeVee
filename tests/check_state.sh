#!/bin/sh
# DeeVee make check: save state / resume point and Restart (D-031).
# Save at frame 400 of a fixture, resume in a fresh process, and restart.
#
#   check_state.sh "<run harness>" <core> <fixture.iso> <work dir>
set -u
run=$1 core=$2 iso=$3 work=$4
mkdir -p "$work"
state=$work/check.state

HARNESS_STATE=$state HARNESS_SAVE=400 $run "$core" "$iso" 401 "$work" >"$work/save.log" 2>/dev/null
if ! grep -q "serialize at frame 400: ok" "$work/save.log"; then
  echo "   save/load: FAILED (serialize)"; exit 1
fi
HARNESS_DEBUG=1 HARNESS_STATE=$state HARNESS_LOAD=0 $run "$core" "$iso" 200 "$work" >"$work/load.log" 2>&1
if grep -q "unserialize at frame 0: ok" "$work/load.log" && grep -q "Resumed from save state" "$work/load.log"; then
  echo "   save/load (ok)"
else
  echo "   save/load: FAILED (unserialize)"; exit 1
fi
HARNESS_DEBUG=1 HARNESS_RESET=300 $run "$core" "$iso" 330 "$work" >"$work/reset.log" 2>&1
if grep -q "OSD: Restarting disc" "$work/reset.log"; then
  echo "   reset (ok)"
else
  echo "   reset: FAILED"; exit 1
fi
