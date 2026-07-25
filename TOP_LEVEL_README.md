# 2025CA01061 — Assignment 2: Kernel Threads, Deferred Work & Memory Management
Purva Bhagwagar

Build both modules on your Ubuntu VM (see the chat walkthrough for exact
commands). Each subdirectory is self-contained per the course's
bits-ddrv/ layout:

- ch09-bitsmon/  -> Question 1 (kthread + workqueue)
- ch11-bitsmem/  -> Question 2 (slab cache + kmalloc/vmalloc benchmark)
- evidence/      -> dmesg/terminal output populated by each test.sh

Before zipping for submission, run `scripts/checkpatch.pl --file --no-tree`
on both .c files and fix all ERRORs.
