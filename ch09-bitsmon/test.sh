#!/bin/bash
# test.sh - drives the required bitsmon demonstration and saves evidence.
set -e

EVID_DIR="../evidence/bitsmon"
mkdir -p "$EVID_DIR"

echo "== building =="
make clean && make

echo "== inserting module =="
sudo dmesg -C
sudo insmod ./bitsmon.ko

echo "== kthread visible =="
ps -eLo comm | grep bitsmon | tee "$EVID_DIR/01_ps_kthread.txt"

echo "== waiting for two stat summaries (~10s) =="
sleep 11
dmesg | tee "$EVID_DIR/02_dmesg_summaries.txt"

echo "== reading /dev/bitsmon =="
cat /dev/bitsmon | tee "$EVID_DIR/03_snapshot_default.txt"

echo "== reconfiguring interval_ms to 250 =="
echo 250 | sudo tee /sys/class/bitsmon/bitsmon/interval_ms
sleep 6
cat /dev/bitsmon | tee "$EVID_DIR/04_snapshot_250ms.txt"

echo "== removing module =="
sudo rmmod bitsmon
dmesg | tail -n 5 | tee "$EVID_DIR/05_dmesg_unload.txt"

echo "== kmemleak scan =="
if [ -e /sys/kernel/debug/kmemleak ]; then
	echo scan | sudo tee /sys/kernel/debug/kmemleak
	sleep 2
	sudo cat /sys/kernel/debug/kmemleak | tee "$EVID_DIR/06_kmemleak.txt"
else
	echo "kmemleak not available on this kernel (see README)" | tee "$EVID_DIR/06_kmemleak.txt"
fi

echo "== done, evidence saved under $EVID_DIR =="
