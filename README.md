# Custom xv6 Kernel Extensions

A set of operating-systems extensions built on top of MIT's
[xv6-riscv](https://github.com/mit-pdos/xv6-riscv) teaching kernel:

- **Copy-on-write `fork()`** — per-page reference counting, eliminating
  eager memory duplication and cutting `fork()` overhead for large
  address spaces.
- **Lazy page allocation** — page-fault-driven allocation that defers
  memory commitment until first access.
- **`mmap()` / `munmap()`** — anonymous and file-backed mappings with
  shared/private semantics, built on the same on-demand-paging fault
  handler.
- **MLFQ scheduler** — a Multi-Level Feedback Queue replacing
  round-robin, with quantum-based priority demotion and periodic aging
  to eliminate starvation.

All four are implemented as real, working kernel code — not stubs —
and are verified against the full upstream `usertests` regression
suite (see [Testing](#testing) below).

## Why these four

Plain xv6 makes reasonable simplifying choices that a "real" kernel
doesn't: `fork()` copies every page up front, memory is allocated the
moment a process asks for it, there's no way to map a file or share
memory directly, and the scheduler treats a tight numerical loop
exactly the same as a shell waiting on your next keystroke. Each
extension here targets one of those simplifications directly, and
they build on each other in a specific way worth calling out:

**Lazy allocation and `mmap()` share a fault handler by design.**
`sbrk()`'s lazy path already asked "what if a valid address just isn't
backed by a physical page yet, and we fix that on first touch?" in the
base xv6-riscv tree this repo starts from. `mmap()` asks almost the
same question for a different reason (an anonymous or file-backed
region instead of the heap), so `mmapfault()` in `kernel/vm.c` is a
sibling to the existing `vmfault()`, not a rewrite of the memory
model. `usertrap()` tries copy-on-write first, then the lazy heap
fault, then the mmap fault — three fully independent mechanisms
that happen to all present as "page not there yet, or there but
read-only" store/load faults.

**Copy-on-write turns `fork()` isolation into a *promise*, not a
*copy*.** The physical memory is shared and marked read-only in both
processes the instant `fork()` returns; the first process to actually
write to a given page is the one that pays for a private copy of it
(`cowfault()`). Processes that `fork()` and then immediately `exec()`
— the overwhelmingly common case in a Unix-like shell — now do
effectively zero page copying for the address space they're about to
discard anyway.

**MLFQ is the one extension that isn't about memory at all** — it's
included because "cut `fork()` overhead" and "defer allocation" are
half the operating-systems story; the other half is *who gets the CPU
and when*, and a kernel that gets memory management right but still
makes an interactive shell wait behind a `while(1);` isn't telling the
whole story.

## Repository layout

This is a fork of xv6-riscv with the following files added or
substantially modified. Everything not listed here is unmodified
upstream xv6.

| File | What changed |
|---|---|
| `kernel/kalloc.c` | Per-physical-page reference counting (`kaddrefcount`, `kgetrefcount`); `kfree()` only returns a page to the free list once its count hits zero. |
| `kernel/riscv.h` | Added `PTE_COW`, a software PTE bit marking a copy-on-write page. |
| `kernel/vm.c` | `uvmcopy()` rewritten to share physical pages COW instead of copying them; added `cowfault()`; added `mmapfault()` for on-demand mmap paging; fixed `copyout()`'s read-only-page guard to resolve COW faults instead of rejecting them (see [Bugs found and fixed](#bugs-found-and-fixed-along-the-way)). |
| `kernel/proc.h` | Added `struct vma` and a per-process `vma[NVMA]` table; added MLFQ scheduling fields (`mlfq_level`, `mlfq_ticks`) to `struct proc`. |
| `kernel/proc.c` | `kfork()`/`kexit()` inherit/release VMAs; `scheduler()` rewritten for MLFQ; added `mlfq_ageall()`, `mlfq_tick()`; `sleep()` resets a process's time-slice counter (see below). |
| `kernel/trap.c` | `usertrap()`'s page-fault path now tries COW → lazy alloc → mmap fault in order; timer-tick handling drives MLFQ demotion instead of unconditional per-tick `yield()`. |
| `kernel/mman.h` | New: `PROT_*` / `MAP_*` constants shared by kernel and user code. |
| `kernel/sysfile.c` | New: `sys_mmap()`, `sys_munmap()`. |
| `kernel/param.h` | Added `NVMA`, `MLFQNLEVELS`, `MLFQAGETICKS`. |
| `kernel/memlayout.h` | Added `MMAPBASE`/`VMASIZE`, the fixed address-space slots `mmap()` hands out. |
| `kernel/syscall.{h,c}`, `user/usys.pl`, `user/user.h` | Registered `mmap`/`munmap` as real syscalls end-to-end. |
| `user/cowtest.c`, `user/mmaptest.c`, `user/mlfqtest.c` | New demo/regression programs for the three user-visible features (see [Testing](#testing)). |

Lazy `sbrk()` allocation (`vmfault()` in `kernel/vm.c`, `sys_sbrk()` in
`kernel/sysproc.c`) was already present in the upstream xv6-riscv
branch this repo is based on; it's documented here because `mmapfault()`
is deliberately written as its sibling, but it isn't new code from this
project.

## How each piece works

### Copy-on-write `fork()`

- Every physical page handed out by `kalloc()` gets a reference count,
  starting at 1.
- `uvmcopy()` (called by `fork()`) no longer allocates a new page and
  `memmove()`s the contents. Instead, for every writable page in the
  parent, it clears `PTE_W`, sets `PTE_COW`, maps the *same* physical
  page into the child's page table, and bumps the page's reference
  count. Read-only pages (text) are shared as-is with no COW bit
  needed, since they were never going to be written anyway.
- `cowfault()` handles the store page fault that happens the first
  time either process writes to a shared page: if the page's refcount
  is still 1 (the other side already dropped its reference, e.g. via
  `exit()`), it just flips `PTE_W` back on — no copy needed, since
  there's no one left to see the "wrong" data. Otherwise it allocates
  a fresh page, copies the contents, and remaps it privately.
- `kfree()` only actually frees a page once its reference count drops
  to zero, so a page shared N ways survives until all N owners have
  either written to it (triggering their own private copy) or exited.

### Lazy allocation

`sbrk()` can grow a process's declared size (`p->sz`) without mapping
any pages. The first access to newly-grown memory takes a page fault;
`vmfault()` (in `kernel/vm.c`) allocates and zero-fills a page and maps
it in, only at that point. A process that calls `sbrk()` for a large
buffer it barely touches pays for exactly the pages it uses.

### `mmap()` / `munmap()`

- Each process has a fixed table of `NVMA` (16) VMA ("virtual memory
  area") slots, each pre-assigned a disjoint 256MB region of address
  space just below the trapframe (`MMAPBASE`/`VMASIZE` in
  `kernel/memlayout.h`). `mmap()` just claims an unused slot — no
  address-space search needed, and no `MAP_FIXED` support.
- `mmap()` only records bookkeeping (address range, protection, flags,
  backing file + offset). No physical memory is touched and, for a
  file-backed mapping, no I/O happens yet.
- The first access to a page in a VMA's range faults into
  `mmapfault()`, which allocates a physical page, zero-fills it, maps
  it with the mapping's requested `PROT_*` permissions, and — for a
  file-backed mapping — reads the corresponding chunk of the file into
  it via `readi()`.
- `munmap()` (and `kexit()`'s cleanup) writes a `MAP_SHARED` +
  `PROT_WRITE` page back to its file with `writei()` before freeing it,
  so writes through a shared mapping are visible to anything else that
  reads the file afterwards. `MAP_PRIVATE` mappings never write back.
- `fork()` inherits the parent's VMA table (and takes its own reference
  to any backing file) so mmap'd regions remain valid in the child;
  `exit()` releases them. Only unmapping a prefix, a suffix, or an
  entire mapping is supported — punching a hole in the middle of one
  is not (this matches the scope of the classic teaching-OS mmap lab
  this is modeled after).

### MLFQ scheduler

- `MLFQNLEVELS` (4) priority queues, 0 (highest) to 3 (lowest). Every
  new process starts at level 0.
- Quantum at level `L` is `2^L` clock ticks. A process that uses its
  *entire* quantum gets demoted one level (capped at the lowest); a
  process that blocks or yields before its quantum is up keeps its
  current level.
- Every `MLFQAGETICKS` (100) ticks, every process is boosted back to
  level 0, bounding the worst-case wait time for a process stuck at
  the bottom under sustained contention.
- `sleep()` resets a process's accumulated tick count for its current
  quantum. Without this, a process that runs in many short bursts
  separated by blocking (exactly what a `fork()`/`wait()`-heavy or
  I/O-heavy process looks like) would slowly accumulate tick credit
  across unrelated scheduling episodes and eventually get wrongly
  demoted, even though it never used one continuous quantum. See
  [Bugs found and fixed](#bugs-found-and-fixed-along-the-way) for how
  this was actually found.
- The scan order in `scheduler()` picks **one** process per pass,
  starting from the highest non-empty priority level, and resumes the
  index scan from a rotating cursor rather than always restarting at
  index 0 — see the long comment above `scheduler()` in
  `kernel/proc.c` for why both halves of that sentence turned out to
  matter (get either one wrong and either a high-index process
  starves, or a handful of CPU-bound processes can cascade through
  every priority level before an interactive process is reconsidered).

## Bugs found and fixed along the way

This section exists because a project like this is only as credible
as its testing, and these three were real, reproducible regressions
caught by running the full `usertests` suite — not hypothetical edge
cases:

1. **`copyout()` rejected legitimate writes to COW pages.** xv6's
   `copyout()` has a guard against the kernel writing into a
   read-only user text page (`if ((*pte & PTE_W) == 0) return -1;`).
   Once COW pages also carry `PTE_W`-clear (using `PTE_COW` instead),
   this same guard started rejecting *any* kernel-side write into a
   COW page that hadn't been touched yet — for example `piperead()`
   copying pipe data into a freshly-forked process's COW'd stack.
   Symptom: `usertests`' `preempt` test failed a pipe `read()` with no
   panic and no obvious cause. Fixed by having `copyout()` call
   `cowfault()` to resolve the page before writing, only failing for
   *actually* read-only (non-COW) pages.
2. **Scheduler restarting the scan from index 0 starved high-index
   processes.** An early version of the MLFQ `scheduler()` picked one
   process, then restarted the entire scan from level 0 / proc-table
   index 0 on every reschedule. Under a bursty fork()/exit() workload,
   whichever runnable process sat at the lowest table index kept
   winning indefinitely, starving a higher-index process — observed as
   `usertests`' `reparent` test intermittently exhausting the process
   table (`fork()` failing) even though nothing was actually leaking.
3. **The opposite fix reintroduced multi-second scheduling latency.**
   Making the scanner walk the *entire* proc table for level 0 before
   ever checking level 1 (to fix bug #2) fixed the starvation, but let
   several CPU-bound processes cascade through all 4 priority levels —
   up to `(1+2+4+8)` ticks each — before the scheduler ever came back
   to check whether a newly-runnable, high-priority interactive process
   was waiting. With a handful of busy processes this added up to real,
   user-visible multi-second stalls (caught by `mlfqtest` timing out).
   The final fix picks exactly one process per scheduler pass *and*
   resumes the index scan from a rotating cursor, getting both
   starvation-freedom and responsiveness at once.

All three are described in more detail in the comments at their fix
sites (`kernel/vm.c`'s `copyout()`, `kernel/proc.c`'s `scheduler()`).

## Building and running

Requires a RISC-V64 cross-compiler and QEMU with `riscv64` system
emulation support. On Debian/Ubuntu:

```
sudo apt-get install gcc-riscv64-linux-gnu binutils-riscv64-linux-gnu qemu-system-misc
```

Then, from the repository root:

```
make TOOLPREFIX=riscv64-linux-gnu- qemu-nox
```

(Omit `TOOLPREFIX` if you have a `riscv64-unknown-elf-` toolchain
installed instead — the Makefile auto-detects several common prefixes.)

This builds the kernel and filesystem image and boots xv6 in QEMU with
the serial console attached to your terminal. You'll land at a `$`
shell prompt. `Ctrl-a x` exits QEMU.

## Testing

From the xv6 shell prompt, once booted:

```
$ cowtest      # copy-on-write fork(): forks, has children touch every
               # inherited page, verifies parent's copy is untouched,
               # and times the fork() itself
$ mmaptest     # anonymous mappings, MAP_SHARED write-back to a file,
               # and MAP_PRIVATE write isolation
$ mlfqtest     # starts 4 CPU-bound busy-loops, then measures how
               # quickly an interactive pause(1) loop still gets
               # scheduled around them
$ usertests    # the full upstream xv6 regression suite -- passes
               # cleanly against every change in this repo
```

`usertests` takes a few minutes and is the real confidence check: it
exercises fork/exec/pipes/the filesystem/permissions far beyond what
the three feature-specific demos above cover, and every change in this
repo has been run against it to green.

## Known limitations

- `mmap()` doesn't support `MAP_FIXED`, and can't punch a hole in the
  middle of an existing mapping (only unmapping a prefix, suffix, or
  the whole region is supported).
- An anonymous `MAP_SHARED` mapping is not actually shared across
  `fork()` — the child gets its own zero-filled copy on first fault,
  rather than the same physical pages as the parent. File-backed
  `MAP_SHARED` mappings work correctly across `fork()` because both
  sides independently read/write the same underlying file.
- `copyin()`/`copyout()` don't reach into not-yet-faulted `mmap()`
  pages the way `usertrap()`'s page-fault path does — a syscall that's
  handed a pointer into a fresh mmap'd region before it's ever been
  touched will fail rather than triggering `mmapfault()`. Touching the
  memory from user code first (as any real program naturally does
  before passing it to a syscall) avoids this.
- The MLFQ scheduler is a single global policy with fixed constants
  (`MLFQNLEVELS`, `MLFQAGETICKS` in `kernel/param.h`); there's no
  per-process priority hinting (e.g. a `nice()` syscall).

## Credits

Built on [xv6-riscv](https://github.com/mit-pdos/xv6-riscv) by the
MIT PDOS group, itself a re-implementation of Dennis Ritchie's and Ken
Thompson's Unix Version 6. See the original `README` for xv6's own
acknowledgments and license (MIT).
