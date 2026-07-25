# bitsmon — kthread sampler with deferred workqueue statistics

Purva Bhagwagar, 2025CA01061

## Design

- `bitsmon-worker` (`kthread_run`) appends a `{seq, jiffies, ktime_ns}`
  record to `sample_list` every `interval_ms` (default 1000, sysfs-tunable
  100–5000) and sleeps via `wait_event_interruptible_timeout()` so
  `kthread_stop()` wakes it immediately instead of waiting out the sleep.
- `sample_list` is protected by `sample_lock` (a spinlock, IRQ-safe variant).
- A delayed work item on a private ordered workqueue fires every 5 s,
  drains the list, computes min/max/avg inter-sample gap, updates
  `stats` under `stats_lock` (a mutex), logs one `pr_info`, and
  re-queues itself.
- `/dev/bitsmon` (`read`) returns the latest snapshot under `stats_lock`.
- `/sys/class/bitsmon/bitsmon/interval_ms` (rw) reconfigures the sampling
  period live; the kthread picks it up on its next wait.

## Why the spinlock only ever protects the list (spinlock vs mutex)

`sample_lock` guards *only* the act of linking/unlinking `sample_record`
nodes on `sample_list` — nothing else. The work function's job (draining,
computing statistics, printing a summary) never happens while that
spinlock is held. Two reasons:

1. **A spinlock disables preemption (and, via the `irqsave` variant, IRQs)
   for its entire critical section.** Anything held under it must be O(1)
   and non-blocking. Computing min/max/avg over N samples and calling
   `pr_info()` are both unbounded/blocking-adjacent operations (the
   console/printk path can take locks and, depending on config, sleep
   or take a while under contention). Holding a spinlock across that
   would inflate scheduling latency for every other CPU/context waiting
   on the same lock, and on a `_irqsave` spinlock it would also delay
   interrupt handling on that CPU.

2. **The stats structure needs a lock you're allowed to sleep under.**
   `stats_lock` is a mutex precisely because updating `stats` happens in
   process/workqueue context where nothing is time-critical, and a mutex
   lets the kernel put a contending thread to sleep instead of busy-waiting,
   which is the right trade-off for a lock that might be held slightly
   longer (here, across the `pr_info` call).

The fix implemented here is the standard kernel pattern: under
`sample_lock`, do a single `list_splice_init()` to move every pending
record onto a **local, unshared list** — an O(1) pointer-swap operation —
and release the lock immediately. All the actual work (iterating,
computing gaps, freeing records) then runs on that private list with no
lock held at all, and only the final numeric result is published under
`stats_lock`. This keeps the spinlock's critical section constant-time
regardless of how many samples piled up, and keeps any operation that
could plausibly block or take a while off the spinlock entirely.

## kmemleak note

If `/sys/kernel/debug/kmemleak` is not present, the running kernel was
not built with `CONFIG_DEBUG_KMEMLEAK` (common on Ubuntu's stock
`generic` kernel — see top-level notes). `test.sh` detects this and
records it rather than failing; the unload path still frees every
outstanding `sample_record` unconditionally (see `bitsmon_exit`), which
is what a clean kmemleak scan would confirm.

## Files

- `bitsmon.c` — driver source
- `Makefile` — out-of-tree kbuild module makefile
- `test.sh` — runs the full required demonstration, saves output under
  `../evidence/bitsmon/`
