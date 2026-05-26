#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define ANON_BYTES (64 * 1024)

int main(void) {
  void *anon, *heap;
  int fd;
  void *fmap;
  pid_t pid = getpid();
  char *p;

  heap = malloc(ANON_BYTES);
  if (heap)
    memset(heap, 0xaa, ANON_BYTES);

  anon = mmap(NULL, ANON_BYTES, PROT_READ | PROT_WRITE,
              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (anon == MAP_FAILED) {
    perror("mmap anon");
    return 1;
  }
  memset(anon, 0x55, ANON_BYTES);

  fd = open("/proc/self/exe", O_RDONLY);
  if (fd >= 0) {
    fmap = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    if (fmap != MAP_FAILED) {
      p = fmap;
      (void)p[0];
    }
  }

  printf("test_pagetree pid=%d\n", pid);
  printf("  heap=%p anon=%p (size=%d)\n", heap, anon, ANON_BYTES);
  printf("Now run (in another shell or after backgrounding):\n");
  printf("  echo %d > /proc/pagetree && cat /proc/pagetree\n", pid);
  printf("Stop this process with: kill %d\n", pid);
  fflush(stdout);

  while (1)
    pause();
  return 0;
}
