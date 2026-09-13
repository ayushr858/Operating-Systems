// cowtest.c: exercises copy-on-write fork().
//
// Allocates a large chunk of memory, forks several children, has each
// child write to (touch) every page it inherited, then checks that
// the parent's original data is untouched. Under a naive fork() this
// is unremarkable; the interesting part is timing fork() itself: with
// COW, fork() on a large address space should be dramatically faster
// than an eager-copy fork(), because no data is duplicated until a
// write actually happens.

#include "kernel/types.h"
#include "kernel/riscv.h"
#include "kernel/vm.h"
#include "user/user.h"

#define NPAGES 256 // 1MB of heap

int
main(void)
{
  char *base = sbrk(NPAGES * PGSIZE);
  if (base == SBRK_ERROR) {
    printf("cowtest: sbrk failed\n");
    exit(1);
  }

  // Fill with a recognizable pattern before forking.
  for (int i = 0; i < NPAGES; i++)
    base[i * PGSIZE] = (char)(i & 0xff);

  int t0 = uptime();
  int pid = fork();
  int t1 = uptime();

  if (pid < 0) {
    printf("cowtest: fork failed\n");
    exit(1);
  }

  if (pid == 0) {
    // Child: touch (write to) every inherited page. Each write should
    // trigger cowfault() in the kernel exactly once per page.
    for (int i = 0; i < NPAGES; i++)
      base[i * PGSIZE] = (char)((i + 1) & 0xff);

    // Verify the writes stuck, from the child's point of view.
    for (int i = 0; i < NPAGES; i++) {
      if (base[i * PGSIZE] != (char)((i + 1) & 0xff)) {
        printf("cowtest: child: page %d corrupted after write\n", i);
        exit(1);
      }
    }
    printf("cowtest: child wrote and verified %d pages\n", NPAGES);
    exit(0);
  }

  wait(0);

  // Parent's copy must be unaffected by the child's writes: that's
  // the entire point of COW -- the pages diverged on write, they were
  // never shared storage.
  int bad = 0;
  for (int i = 0; i < NPAGES; i++) {
    if (base[i * PGSIZE] != (char)(i & 0xff)) {
      printf("cowtest: parent: page %d was clobbered by child!\n", i);
      bad = 1;
    }
  }

  printf("cowtest: fork() of a %dKB address space took %d ticks\n",
         NPAGES * PGSIZE / 1024, t1 - t0);

  if (bad) {
    printf("cowtest: FAILED (copy-on-write isolation broken)\n");
    exit(1);
  }
  printf("cowtest: PASSED\n");
  exit(0);
}
