#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "mman.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if (pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int)(p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;

  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for (p = proc; p < &proc[NPROC]; p++) {
    initlock(&p->lock, "proc");
    p->state = UNUSED;
    p->kstack = KSTACK((int)(p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu *
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc *
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;

  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc *
allocproc(void)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;
  p->mlfq_level = 0;
  p->mlfq_ticks = 0;

  // Allocate a trapframe page.
  if ((p->trapframe = (struct trapframe *)kalloc()) == 0) {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if (p->pagetable == 0) {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if (p->trapframe)
    kfree((void *)p->trapframe);
  p->trapframe = 0;
  if (p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if (pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if (mappages(pagetable, TRAMPOLINE, PGSIZE, (uint64)trampoline,
               PTE_R | PTE_X) < 0) {
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if (mappages(pagetable, TRAPFRAME, PGSIZE, (uint64)(p->trapframe),
               PTE_R | PTE_W) < 0) {
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;

  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if (n > 0) {
    if (sz + n > TRAPFRAME) {
      return -1;
    }
    if ((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if (n < 0) {
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
kfork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if ((np = allocproc()) == 0) {
    return -1;
  }

  // Copy user memory from parent to child.
  if (uvmcopy(p->pagetable, np->pagetable, p->sz) < 0) {
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // Inherit mmap()'d regions. We don't eagerly copy their physical
  // pages here -- like the rest of a page-fault-driven design, the
  // child just gets the same VMA bookkeeping (so accesses in its
  // range still fault into mmapfault()) and its own reference to the
  // backing file, matching POSIX fork() semantics for mmap regions.
  for (i = 0; i < NVMA; i++) {
    if (p->vma[i].used) {
      np->vma[i] = p->vma[i];
      if (np->vma[i].file)
        filedup(np->vma[i].file);
    }
  }

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for (i = 0; i < NOFILE; i++)
    if (p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for (pp = proc; pp < &proc[NPROC]; pp++) {
    if (pp->parent == p) {
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Release all of a process's mmap()'d regions: free any physical
// pages currently mapped, write dirty MAP_SHARED pages back to their
// backing file, and drop the file references. Called from kexit() so
// mmap()'d memory doesn't leak and shared writes aren't lost.
static void
vmaclose(struct proc *p)
{
  struct vma *v;
  uint64 a;

  for (v = p->vma; v < &p->vma[NVMA]; v++) {
    if (!v->used)
      continue;
    for (a = v->addr; a < v->addr + v->length; a += PGSIZE) {
      pte_t *pte = walk(p->pagetable, a, 0);
      if (pte != 0 && (*pte & PTE_V)) {
        uint64 pa = PTE2PA(*pte);
        if (v->file && (v->flags & MAP_SHARED) && (v->prot & PROT_WRITE)) {
          uint64 foff = v->offset + (a - v->addr);
          begin_op();
          ilock(v->file->ip);
          writei(v->file->ip, 0, pa, foff, PGSIZE);
          iunlock(v->file->ip);
          end_op();
        }
        kfree((void *)pa);
        *pte = 0;
      }
    }
    if (v->file)
      fileclose(v->file);
    v->used = 0;
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
kexit(int status)
{
  struct proc *p = myproc();

  if (p == initproc)
    panic("init exiting");

  // Release any mmap()'d regions before the page table is torn down.
  vmaclose(p);

  // Close all open files.
  for (int fd = 0; fd < NOFILE; fd++) {
    if (p->ofile[fd]) {
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);

  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
kwait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for (;;) {
    // Scan through table looking for exited children.
    havekids = 0;
    for (pp = proc; pp < &proc[NPROC]; pp++) {
      if (pp->parent == p) {
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if (pp->state == ZOMBIE) {
          // Found one.
          pid = pp->pid;
          if (addr != 0 &&
              copyout(p->pagetable, p->sz, addr, (char *)&pp->xstate,
                      sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          pp->parent = 0;
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || killed(p)) {
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep_prepare(p); //DOC: wait-sleep
    release(&wait_lock);
    sleep();
    acquire(&wait_lock);
  }
}

// Cursor remembering where the last pick left off in proc[], so that
// re-scanning a priority level resumes round-robin instead of always
// restarting at index 0 (see the "Correctness note" below for why
// that distinction matters).
static int mlfq_next_idx = 0;

// Per-CPU process scheduler, implementing Multi-Level Feedback Queue
// (MLFQ) scheduling.
//
// Rationale: plain round-robin treats a short-lived, latency-sensitive
// process (e.g. one blocked on I/O most of the time) exactly like a
// long CPU-bound batch job, so interactive processes wait behind
// compute-heavy ones for their fair share of ticks. MLFQ instead:
//
//   - keeps MLFQNLEVELS queues, 0 (highest priority) .. NLEVELS-1
//     (lowest). Every new/runnable process starts at level 0.
//   - hands out longer time quanta at lower priority levels
//     (quantum(level) = 2^level clock ticks), so a process that keeps
//     using its *entire* quantum -- i.e. behaves like a CPU-bound job
//     -- gets demoted one level per exhausted quantum. A process that
//     blocks or yields before its quantum is up (typically because
//     it's I/O- or interaction-bound) keeps its current level, so
//     interactive workloads stay near the front of the queue.
//   - periodically (every MLFQAGETICKS ticks) boosts *every* process
//     back to level 0. Without this, a steady stream of short
//     interactive jobs could keep a CPU-bound job stuck at the bottom
//     queue forever; periodic aging bounds worst-case wait time.
//
// Correctness note (the scan order below is more subtle than it
// looks, because two different bugs live on either side of it):
//
//   - Picking one process and then restarting the *whole* scan from
//     level 0 / index 0 every time sounds harmless, but it lets
//     whichever RUNNABLE process happens to sit at the lowest index
//     win indefinitely, starving a higher-index process (e.g. a
//     long-lived daemon like init) whenever a lower-index process
//     keeps cycling back to RUNNABLE quickly.
//   - The opposite fix -- walking the *entire* proc[] table for level
//     0 before ever looking at level 1, and the entire table for
//     level 1 before level 2, etc., all within one scheduler() call --
//     avoids that starvation but reintroduces the very problem MLFQ
//     exists to solve: a handful of CPU-bound processes can cascade
//     down through every level (quantum 1, then 2, then 4, then 8
//     ticks each) before the scheduler ever comes back up to check
//     whether a *newly runnable* level-0 process -- like an
//     interactive job that just woke up -- is waiting. With several
//     hogs this adds up to tens of ticks of avoidable latency.
//
// The fix is to do both at once: pick exactly one process per
// scheduler() pass (so a fresh level-0 process is always reconsidered
// immediately next time round), but resume the index scan from where
// mlfq_next_idx left off rather than from 0 (so processes at the same
// level still get a fair round-robin turn instead of the lowest index
// always winning).
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for (;;) {
    // The most recent process to run may have had interrupts
    // turned off; enable them to avoid a deadlock if all
    // processes are waiting. Then turn them back off
    // to avoid a possible race between an interrupt
    // and wfi.
    intr_on();
    intr_off();

    mlfq_ageall();

    int found = 0;
    for (int level = 0; level < MLFQNLEVELS && !found; level++) {
      for (int k = 0; k < NPROC && !found; k++) {
        int idx = (mlfq_next_idx + k) % NPROC;
        p = &proc[idx];
        acquire(&p->lock);
        if (p->state == RUNNABLE && p->mlfq_level == level) {
          // Switch to chosen process.  It is the process's job
          // to release its lock and then reacquire it
          // before jumping back to us.
          p->state = RUNNING;
          c->proc = p;
          swtch(&c->context, &p->context);

          // Don't re-enable interrupts on release.
          mycpu()->intena = 0;

          // Process is done running for now.
          // It should have changed its p->state before coming back.
          c->proc = 0;
          found = 1;
          mlfq_next_idx = (idx + 1) % NPROC;
        }
        release(&p->lock);
      }
    }
    if (found == 0) {
      // nothing to run; stop running on this core until an interrupt.
      asm volatile("wfi");
    }
  }
}

// Boost every process back to the top MLFQ level (level 0) every
// MLFQAGETICKS ticks. Prevents starvation: a process demoted to a low
// level under heavy contention is guaranteed to be reconsidered at top
// priority at least that often, no matter how busy the system is.
void
mlfq_ageall(void)
{
  static uint64 last_boost = 0;
  struct proc *p;

  acquire(&tickslock);
  uint64 now = ticks;
  release(&tickslock);

  if (now - last_boost < MLFQAGETICKS)
    return;
  last_boost = now;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    p->mlfq_level = 0;
    p->mlfq_ticks = 0;
    release(&p->lock);
  }
}

// Called once per timer tick while a process is running (from
// usertrap()). Charges the tick to the process's current MLFQ time
// slice and, once that slice is used up, demotes the process one
// level (capping at the lowest level) and returns 1 so the caller
// knows to yield the CPU. Returns 0 if the process should keep running
// -- i.e. its quantum at the current level isn't exhausted yet.
int
mlfq_tick(struct proc *p)
{
  int quantum = 1 << p->mlfq_level; // longer slices at lower priority
  p->mlfq_ticks++;
  if (p->mlfq_ticks < quantum)
    return 0;
  p->mlfq_ticks = 0;
  if (p->mlfq_level < MLFQNLEVELS - 1)
    p->mlfq_level++;
  return 1;
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if (!holding(&p->lock))
    panic("sched p->lock");
  if (mycpu()->noff != 1)
    panic("sched locks");
  if (p->state == RUNNING)
    panic("sched RUNNING");
  if (intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  extern char userret[];
  static int first = 1;
  struct proc *p = myproc();

  // Still holding p->lock from scheduler.
  release(&p->lock);

  if (__atomic_load_n(&first, __ATOMIC_ACQUIRE)) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    // ensure other cores see first=0.
    __atomic_store_n(&first, 0, __ATOMIC_RELEASE);

    // We can invoke kexec() now that file system is initialized.
    // Put the return value (argc) of kexec into a0.
    p->trapframe->a0 = kexec("/init", (char *[]){"/init", 0});
    if (p->trapframe->a0 == -1) {
      panic("exec");
    }
  }

  // return to user space, mimicing usertrap()'s return.
  prepare_return();
  uint64 satp = MAKE_SATP(p->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// Register current process as waiting for wakeups on chan.
void
sleep_prepare(void *chan)
{
  struct proc *p = myproc();

  acquire(&p->lock);
  if (chan == 0)
    panic("sleep_prepare: zero chan");
  p->chan = chan;
  release(&p->lock);
}

// Put the thread to sleep.  Assumes sleep_prepare() was called before.
// If the channel registered by sleep_prepare() has been woken up in
// the meantime, do not go to sleep, and instead return immediately.
void
sleep(void)
{
  struct proc *p = myproc();

  acquire(&p->lock);
  if (p->chan != 0) {
    p->state = SLEEPING;
    // The process is voluntarily giving up the CPU before its MLFQ
    // time slice ran out -- exactly the behaviour MLFQ is meant to
    // reward. Reset its slice so it gets a full fresh quantum at its
    // current level next time it's scheduled, rather than carrying
    // over partial-tick credit from this run into the next one. Without
    // this, ticks accumulated across many short, frequently-blocking
    // runs (typical of an I/O- or sleep-heavy process, e.g. one that
    // repeatedly forks and wait()s) would eventually cross the quantum
    // threshold and demote it, even though it never used a single
    // continuous quantum -- exactly the failure mode that let
    // rapidly-forking children (which always start fresh at level 0)
    // periodically outrun a demoted reaper process like init.
    p->mlfq_ticks = 0;
    sched();
  }
  release(&p->lock);
}

// Wake up all processes sleeping on channel chan.
void
wakeup(void *chan)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->chan == chan) {
      // If the process is waiting for wakeups on this channel,
      // signal that the wakeup happened by clearing p->chan.
      p->chan = 0;

      // If this waiting process has gotten so far as to actually
      // go to sleep, also set it back to RUNNING.
      if (p->state == SLEEPING) {
        p->state = RUNNABLE;
      }
    }
    release(&p->lock);
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kkill(int pid)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->pid == pid) {
      p->killed = 1;
      if (p->state == SLEEPING) {
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;

  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if (user_dst) {
    return copyout(p->pagetable, p->sz, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if (user_src) {
    return copyin(p->pagetable, p->sz, dst, src, len);
  } else {
    memmove(dst, (char *)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
    // clang-format off
    [UNUSED]    = "unused",
    [USED]      = "used",
    [SLEEPING]  = "sleep ",
    [RUNNABLE]  = "runble",
    [RUNNING]   = "run   ",
    [ZOMBIE]    = "zombie"
    // clang-format on
  };
  struct proc *p;
  char *state;

  printk("\n");
  for (p = proc; p < &proc[NPROC]; p++) {
    if (p->state == UNUSED)
      continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printk("%d %s %s", p->pid, state, p->name);
    printk("\n");
  }
}
