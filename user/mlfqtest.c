// mlfqtest.c: demonstrates the MLFQ scheduler favoring an interactive
// (frequently-sleeping) process over CPU-bound children.
//
// Forks several "CPU hog" children that spin in a tight busy loop
// (never blocking, so they use their entire time quantum every turn
// and get demoted level by level), while the parent acts as an
// "interactive" process that calls pause(1) in a loop -- i.e. it
// voluntarily gives up the CPU well before any quantum would expire,
// so under MLFQ it should stay at (or quickly return to) the highest
// priority level.
//
// What to look for: the parent's measured pause() latency should stay
// close to 1 tick even while the hogs are running flat out. Under a
// plain round-robin scheduler with N runnable processes, a call
// intended to wait 1 tick can instead take on the order of N ticks to
// return, because the interactive process has to wait its turn behind
// every hog in the same queue.

#include "kernel/types.h"
#include "user/user.h"

#define NHOGS 4
#define NPAUSES 30

int
main(void)
{
  int hogpid[NHOGS];

  printf("mlfqtest: starting %d CPU-bound hogs...\n", NHOGS);

  for (int i = 0; i < NHOGS; i++) {
    int pid = fork();
    if (pid < 0) {
      printf("mlfqtest: fork failed\n");
      exit(1);
    }
    if (pid == 0) {
      // Busy-spin forever (until killed by the parent) -- never
      // sleeps, never yields voluntarily, so it should get demoted to
      // low-priority queues and stay there.
      volatile long x = 0;
      for (;;)
        x++;
    }
    hogpid[i] = pid;
  }

  int total = 0;
  int worst = 0;
  for (int i = 0; i < NPAUSES; i++) {
    int t0 = uptime();
    pause(1);
    int dt = uptime() - t0;
    total += dt;
    if (dt > worst)
      worst = dt;
  }

  printf("mlfqtest: parent's pause(1) averaged %d/%d ticks over %d calls, "
         "worst case %d ticks, while %d hogs ran flat out\n",
         total, NPAUSES, NPAUSES, worst, NHOGS);
  printf("mlfqtest: (values close to 1 tick show interactive processes\n");
  printf("mlfqtest:  aren't starved behind CPU-bound processes under MLFQ)\n");

  for (int i = 0; i < NHOGS; i++)
    kill(hogpid[i]);

  for (int i = 0; i < NHOGS; i++)
    wait(0);

  exit(0);
}
