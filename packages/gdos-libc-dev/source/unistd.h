#ifndef unistd_h_INCLUDED
#define unistd_h_INCLUDED

#include <stddef.h>
#include <stdint.h>

typedef int64_t ssize_t;
typedef uint64_t pid_t;

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

pid_t getpid(void);
ssize_t write(int fd, const void *buf, size_t count);
[[noreturn]] void _exit(int status);

#endif // unistd_h_INCLUDED
