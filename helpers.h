#ifndef LIBSUCKTERM_HELPERS_H
#define LIBSUCKTERM_HELPERS_H
#include <string.h>
#include <errno.h>
#include <sys/types.h>

typedef unsigned char uchar;
typedef unsigned int uint;
typedef unsigned long ulong;
typedef unsigned short ushort;

#define SERRNO strerror(errno)
#define MIN(a, b)  ((a) < (b) ? (a) : (b))
#define MAX(a, b)  ((a) < (b) ? (b) : (a))
#define LEN(a)     (sizeof(a) / sizeof(a[0]))
#define DEFAULT(a, b)     (a) = (a) ? (a) : (b)
#define BETWEEN(x, a, b)  ((a) <= (x) && (x) <= (b))
#define LIMIT(x, a, b)    (x) = (x) < (a) ? (a) : (x) > (b) ? (b) : (x)
#define MODBIT(x, set, bit) ((set) ? ((x) |= (bit)) : ((x) &= ~(bit)))

ssize_t xwrite(int fd, const char* s, size_t len);

void* xmalloc(size_t len);
void* xrealloc(void* p, size_t len);

int utf8decode(char* s, long* u);
int utf8encode(long* u, char* s);
int isfullutf8(char* s, int b);
int utf8size(char* s);

void die(const char* errstr, ...);

#endif
