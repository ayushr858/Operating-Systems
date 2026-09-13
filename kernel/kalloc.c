// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.
//
// Extended with per-physical-page reference counting to support
// copy-on-write fork(). Every physical page that can be handed out by
// kalloc() has an associated reference count. kfree() only actually
// returns the page to the free list once its count drops to zero.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

// Reference counts for every physical page in [0, PHYSTOP), indexed by
// page number. Protected by refcnt_lock so increments/decrements racing
// with kalloc/kfree are safe.
struct spinlock refcnt_lock;
uint8 refcount[PHYSTOP / PGSIZE];

#define PA2IDX(pa) (((uint64)(pa)) / PGSIZE)

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&refcnt_lock, "refcnt");
  freerange(end, (void *)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char *)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE) {
    // freerange pages start with a refcount of 1 so the first kfree()
    // call below actually frees them onto the free list.
    refcount[PA2IDX(p)] = 1;
    kfree(p);
  }
}

// Increment the reference count of the physical page containing pa.
// Called whenever a new PTE is made to point at an existing physical
// page (e.g. COW fork()).
void
kaddrefcount(void *pa)
{
  acquire(&refcnt_lock);
  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("kaddrefcount");
  refcount[PA2IDX(pa)]++;
  release(&refcnt_lock);
}

// Return the current reference count of a physical page.
int
kgetrefcount(void *pa)
{
  int c;
  acquire(&refcnt_lock);
  c = refcount[PA2IDX(pa)];
  release(&refcnt_lock);
  return c;
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
//
// With reference counting, kfree() decrements the page's refcount and
// only places it back on the free list once the count reaches zero.
void
kfree(void *pa)
{
  struct run *r;
  int should_free;

  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  acquire(&refcnt_lock);
  if (refcount[PA2IDX(pa)] < 1)
    panic("kfree: refcount underflow");
  refcount[PA2IDX(pa)]--;
  should_free = (refcount[PA2IDX(pa)] == 0);
  release(&refcnt_lock);

  if (!should_free)
    return;

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run *)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
// A freshly allocated page always starts with a reference count of 1.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if (r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if (r) {
    memset((char *)r, 5, PGSIZE); // fill with junk
    acquire(&refcnt_lock);
    refcount[PA2IDX(r)] = 1;
    release(&refcnt_lock);
  }
  return (void *)r;
}
