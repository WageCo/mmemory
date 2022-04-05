#ifndef __WAGECO_MMEMORY_H_
#define __WAGECO_MMEMORY_H_
#include <unistd.h>
namespace wageco {
void *malloc(size_t size);
void free(void *addr);
void *calloc(size_t num, size_t nsize);
void *realloc(void *addr, size_t size);
}  // namespace wageco

#endif