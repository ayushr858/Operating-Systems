// mmaptest.c: exercises mmap()/munmap().
//
// 1. Anonymous mapping: map zero-filled memory, write to it, read it
//    back, unmap it.
// 2. File-backed private mapping: map a file read-only, check its
//    contents match what open()+read() would give, and confirm a
//    write to a PRIVATE mapping (if writable) never reaches the file.
// 3. File-backed shared mapping: map a file MAP_SHARED|PROT_WRITE,
//    write through the mapping, munmap it, then re-open the file with
//    ordinary read() and check the write landed on disk.

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/riscv.h"
#include "user/user.h"

char buf[512];
char fillbuf[4096]; // must be global/static: xv6's user stack is only one page

void
anon_test(void)
{
  printf("mmaptest: anonymous mapping... ");
  char *p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                 -1, 0);
  if (p == MAP_FAILED) {
    printf("FAILED (mmap)\n");
    exit(1);
  }
  // Anonymous memory must start zero-filled.
  for (int i = 0; i < 4096; i++) {
    if (p[i] != 0) {
      printf("FAILED (not zero-filled at %d)\n", i);
      exit(1);
    }
  }
  for (int i = 0; i < 4096; i++)
    p[i] = (char)(i & 0x7f);
  for (int i = 0; i < 4096; i++) {
    if (p[i] != (char)(i & 0x7f)) {
      printf("FAILED (readback mismatch at %d)\n", i);
      exit(1);
    }
  }
  if (munmap(p, 4096) < 0) {
    printf("FAILED (munmap)\n");
    exit(1);
  }
  printf("OK\n");
}

void
shared_write_test(const char *path)
{
  printf("mmaptest: MAP_SHARED write-back... ");

  int fd = open(path, O_CREATE | O_RDWR);
  if (fd < 0) {
    printf("FAILED (open)\n");
    exit(1);
  }
  memset(fillbuf, 'a', sizeof(fillbuf));
  if (write(fd, fillbuf, sizeof(fillbuf)) != sizeof(fillbuf)) {
    printf("FAILED (write)\n");
    exit(1);
  }
  close(fd);

  fd = open(path, O_RDWR);
  char *p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) {
    printf("FAILED (mmap)\n");
    exit(1);
  }
  // Touch the mapping: this should fault the page in via mmapfault()
  // and read 'a's from the file.
  if (p[0] != 'a') {
    printf("FAILED (initial contents wrong)\n");
    exit(1);
  }
  // Overwrite through the mapping.
  memset(p, 'b', 4096);
  if (munmap(p, 4096) < 0) {
    printf("FAILED (munmap)\n");
    exit(1);
  }
  close(fd);

  // Re-read with plain read() and check the write landed on disk.
  fd = open(path, O_RDONLY);
  int n = read(fd, buf, sizeof(buf));
  close(fd);
  if (n != sizeof(buf)) {
    printf("FAILED (readback size)\n");
    exit(1);
  }
  for (int i = 0; i < n; i++) {
    if (buf[i] != 'b') {
      printf("FAILED (write-back mismatch at %d)\n", i);
      exit(1);
    }
  }
  printf("OK\n");
}

void
private_no_writeback_test(const char *path)
{
  printf("mmaptest: MAP_PRIVATE isolation... ");

  int fd = open(path, O_RDWR);
  char *p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  if (p == MAP_FAILED) {
    printf("FAILED (mmap)\n");
    exit(1);
  }
  memset(p, 'z', 4096); // private write: must never reach the file
  munmap(p, 4096);
  close(fd);

  fd = open(path, O_RDONLY);
  int n = read(fd, buf, sizeof(buf));
  close(fd);
  for (int i = 0; i < n; i++) {
    if (buf[i] == 'z') {
      printf("FAILED (private write leaked to file)\n");
      exit(1);
    }
  }
  printf("OK\n");
}

int
main(void)
{
  anon_test();
  shared_write_test("mmaptest.tmp");
  private_no_writeback_test("mmaptest.tmp");
  unlink("mmaptest.tmp");
  printf("mmaptest: PASSED\n");
  exit(0);
}
