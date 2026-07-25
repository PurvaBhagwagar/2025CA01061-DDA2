# bitsmem — slab cache playground & allocator benchmark

Purva Bhagwagar, 2025CA01061

## Design

- A private `kmem_cache` (`"bits_rec"`, 64-byte objects) is created at
  load and destroyed at unload; it shows up in `/proc/slabinfo`.
- `/dev/bitsmem` (misc device) accepts text commands via `write()`:
  - `alloc N` — allocate N records from the cache, add to a tracked list.
  - `free N` — release N tracked records back to the cache.
  - `bench` — run the kmalloc-vs-vmalloc timing comparison.
  - Anything else, or a non-numeric argument, returns `-EINVAL`.
    A failed `alloc` frees only the partial batch it just allocated and
    returns `-ENOMEM`, leaving previously-allocated records untouched.
- `read()` reports outstanding count, high-water mark, cache object
  size, and the most recent benchmark numbers.
- Benchmark: for 64 KB and 1 MB, `kmalloc(GFP_KERNEL)` vs `vmalloc()`
  alloc+free cost, averaged over 100 iterations via `ktime_get()`.
- Unload frees every outstanding record before `kmem_cache_destroy()`.

## Analysis (assessed)

**Why `kmalloc` memory is physically contiguous and `vmalloc` memory is not.**
`kmalloc()` allocates from the slab/SLUB allocator, which itself carves
memory out of the buddy allocator's *physically contiguous* pages — for
anything under roughly one page it hands out a chunk of an existing
contiguous page, and for larger power-of-two sizes it directly requests
a matching contiguous block of physical pages. `vmalloc()`, by contrast,
grabs individual (possibly scattered) physical pages wherever the buddy
allocator can find them and stitches them into one *virtually* contiguous
range by installing page-table entries. The addresses look contiguous to
kernel code dereferencing them, but the underlying physical frames are
not adjacent.

**What that implies for DMA.** Most DMA-capable devices only understand
a single physical base address plus a length (or, without an IOMMU, a
scatter-gather list built from physically contiguous segments) — they
have no concept of the CPU's page tables and therefore no notion of
"virtually contiguous". Buffers handed to a DMA engine must be
`kmalloc`-family (or `dma_alloc_coherent`) allocations that are actually
physically contiguous; a `vmalloc()` buffer would need to be split into
its individual physical pages (or run through an IOMMU that can remap
scattered pages into one DMA-visible range) before a device could safely
walk it as one block.

**When `kvmalloc` is the right call.** `kvmalloc()` tries `kmalloc()`
first and transparently falls back to `vmalloc()` if the allocation is
too large to satisfy from the slab/buddy allocator without reclaim
pressure or fragmentation risk. It's the right choice when: the size is
variable and can occasionally be large (so a hard `kmalloc` failure
under fragmentation shouldn't be fatal), the buffer is for ordinary
kernel-internal use (not DMA — the caller doesn't need physical
contiguity), and the code doesn't want to hand-write "try kmalloc, catch
failure, fall back to vmalloc" logic itself.

**A scenario in this driver where `GFP_ATOMIC` would be required instead
of `GFP_KERNEL`.** Every allocation in `bitsmem` currently happens from
process context inside a `write()` syscall, where sleeping is fine, so
`GFP_KERNEL` is correct throughout. `GFP_ATOMIC` would be required if
record allocation instead happened from a context that cannot sleep —
for example, if records were allocated inside a spinlock-held critical
section, or from a hardware interrupt handler / softirq (e.g. if
`bitsmem` were extended to capture a record on every interrupt from some
device rather than only in response to a userspace `write()`). In that
case `kmem_cache_alloc(bits_cache, GFP_ATOMIC)` would be needed so the
allocator never tries to sleep waiting for reclaim.

## kmemleak note

If `/sys/kernel/debug/kmemleak` is not present, the running kernel
lacks `CONFIG_DEBUG_KMEMLEAK` (common on Ubuntu's stock `generic`
kernel — see top-level notes). `test.sh` detects and records this
rather than failing; `bitsmem_exit()` still walks `allocated_list` and
frees every outstanding record before `kmem_cache_destroy()` regardless.

## Files

- `bitsmem.c` — driver source
- `Makefile` — out-of-tree kbuild module makefile
- `test.sh` — runs the full required demonstration, saves output under
  `../evidence/bitsmem/`
