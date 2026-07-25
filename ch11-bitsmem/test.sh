#!/bin/bash
# test.sh - drives the required bitsmem demonstration and saves evidence.
set -e

EVID_DIR="../evidence/bitsmem"
mkdir -p "$EVID_DIR"

echo "== building =="
make clean && make

echo "== inserting module =="
sudo dmesg -C
sudo insmod ./bitsmem.ko

echo "== cache visible in /proc/slabinfo =="
grep bits_rec /proc/slabinfo | tee "$EVID_DIR/01_slabinfo.txt"

echo "== alloc 1000 =="
echo "alloc 1000" | sudo tee /dev/bitsmem
sudo cat /dev/bitsmem | tee "$EVID_DIR/02_after_alloc.txt"

echo "== free 400 =="
echo "free 400" | sudo tee /dev/bitsmem
sudo cat /dev/bitsmem | tee "$EVID_DIR/03_after_free.txt"

echo "== bench =="
echo "bench" | sudo tee /dev/bitsmem
sudo cat /dev/bitsmem | tee "$EVID_DIR/04_bench.txt"

echo "== malformed command -> expect EINVAL =="
echo "alloc oops" | sudo tee /dev/bitsmem; echo "(tee exit code above should be non-zero)" | tee "$EVID_DIR/05_einval.txt"

echo "== removing module (frees remaining 600) =="
sudo rmmod bitsmem
dmesg | tail -n 5 | tee "$EVID_DIR/06_dmesg_unload.txt"

echo "== kmemleak scan =="
if [ -e /sys/kernel/debug/kmemleak ]; then
	echo scan | sudo tee /sys/kernel/debug/kmemleak
	sleep 2
	sudo cat /sys/kernel/debug/kmemleak | tee "$EVID_DIR/07_kmemleak.txt"
else
	echo "kmemleak not available on this kernel (see README)" | tee "$EVID_DIR/07_kmemleak.txt"
fi

echo "== done, evidence saved under $EVID_DIR =="
