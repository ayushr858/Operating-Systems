// mmap()/munmap() protection and flag bits.
// Kept intentionally close to the POSIX names so user code reads
// naturally, but this is xv6's own minimal subset -- only what the
// VMA implementation in vm.c/sysfile.c actually understands.

// prot
#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

// flags
#define MAP_SHARED    0x01 // writes are written back to the file
#define MAP_PRIVATE   0x02 // writes are copy-on-write, never written back
#define MAP_ANONYMOUS 0x04 // no backing file; pages are zero-filled
#define MAP_FIXED     0x08 // not supported: mmap() ignores/rejects this
