#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

namespace wageco {
// 16 位对齐
const unsigned align_to = 16;
typedef char ALIGN[align_to];
// 标头
typedef union header {
  struct {
    size_t size;
    union header *pre;
    union header *next;
  } head;
  ALIGN align;
} header_t;

// 已分配的双向循环表， 已释放的双向循环表 --全局默认初始化
typedef struct {
  header_t *malloc_header;
  header_t *free_header;
  size_t malloc_size;
  size_t free_size;
} stack_memory_t;
static stack_memory_t stack_memory_list;
// 多线程防竞争
static pthread_mutex_t list_locker = PTHREAD_MUTEX_INITIALIZER;

inline void list_init(header_t *node, size_t size) {
  node->head.pre = node;
  node->head.next = node;
  node->head.size = size;
}

inline void list_insert(header_t *&header, header_t *node) {
  if (header) {
    header->head.pre->head.next = node;
    node->head.pre = header->head.pre;
    node->head.next = header;
    header->head.pre = node;
  }
  header = node;
}

inline header_t *list_find(header_t *header, size_t size) {
  if (header) {
    header_t *node = header;
    for (; node->head.size != size && node->head.next != header; node = node->head.next)
      ;
    if (node->head.size == size) {
      return node;
    }
  }
  return nullptr;
}

inline bool list_find(header_t *header, header_t *node) {
  if (header) {
    header_t *p = header;
    for (; p != node && p->head.next != header; p = p->head.next)
      ;
    if (p == node) {
      return true;
    }
  }
  return false;
}

inline void list_delete(header_t *&header, header_t *node) {
  if (header && header->head.next != header) {
    if (node == header) {
      header = header->head.next;
    }
    node->head.pre->head.next = node->head.next;
    node->head.next->head.pre = node->head.pre;
    list_init(node, node->head.size);
  } else {
    header = nullptr;
  }
}

// custom malloc - size must be greater than 0
void *malloc(size_t size) {
  if (!size) {
    return nullptr;
  }
  pthread_mutex_lock(&list_locker);
  size_t total_size = (sizeof(header_t) + size + align_to - 1) & ~(align_to - 1);
  // search list_free
  if (stack_memory_list.free_header) {
    header_t *node = list_find(stack_memory_list.free_header, total_size - sizeof(header_t));
    if (node) {
      list_delete(stack_memory_list.free_header, node);
      --stack_memory_list.free_size;
      list_insert(stack_memory_list.malloc_header, node);
      ++stack_memory_list.malloc_size;
      pthread_mutex_unlock(&list_locker);
      return (void *)(node + 1);
    }
  }
  // sbrk----- align
  void *now_addr = sbrk(total_size);
  // failed
  if (now_addr == (void *)-1) {
    printf("%s errno: %s\n", __func__, strerror(errno));
    pthread_mutex_unlock(&list_locker);
    return nullptr;
  }
  // success
  // node init
  header_t *node = (header_t *)now_addr;
  list_init(node, total_size - sizeof(header_t));
  // insert to list_malloc
  list_insert(stack_memory_list.malloc_header, node);
  ++stack_memory_list.malloc_size;
  pthread_mutex_unlock(&list_locker);
#ifdef DEBUG
  printf("Debug@%s alloc %ld, user %ld, start:%p\n", __func__, total_size, size, sbrk(0));
#endif
  // remove header
  return (void *)(node + 1);
}

// custom free
void free(void *addr) {
  if (!addr) {
    return;
  }
  header_t *node = (header_t *)addr - 1;
  pthread_mutex_lock(&list_locker);
  // double free || no malloc
  if (list_find(stack_memory_list.free_header, node) || !list_find(stack_memory_list.malloc_header, node)) {
    printf("%s errno double free or no malloc. start:%p\n", __func__, node);
    pthread_mutex_unlock(&list_locker);
    return;
  }
  void *program_break_now = sbrk(0);
  list_delete(stack_memory_list.malloc_header, node);
  --stack_memory_list.malloc_size;
  if ((char *)node + sizeof(header_t) + node->head.size == program_break_now) {
    // BUG: 存在内存泄漏
#ifdef DEBUG
    printf("Debug@%s %ld, start:%p\n", __func__, sizeof(header_t) + node->head.size, sbrk(0));
#endif
    // free memory space
    if (sbrk(0 - (sizeof(header_t) + node->head.size)) == (void *)-1) {
      printf("%s errno: %s\n", __func__, strerror(ENOMEM));
      pthread_mutex_unlock(&list_locker);
      return;
    }
  } else {
    // move to free list
    list_insert(stack_memory_list.free_header, node);
    ++stack_memory_list.free_size;
  }

  if (stack_memory_list.malloc_size == 0 && stack_memory_list.free_size > 0) {
    // TODO: 清空
  }
  pthread_mutex_unlock(&list_locker);
}

// custom calloc
void *calloc(size_t num, size_t size) {
  if (!num || !size) {
    return nullptr;
  }
  size_t total_size = num * size;
  void *addr = malloc(total_size);
  if (!addr) {
    return nullptr;
  }
  memset(addr, 0, total_size);
  return addr;
}

// custom realloc
void *realloc(void *addr, size_t size) {
  if (!size || !addr) {
    return malloc(size);
  }
  header_t *node = (header_t *)addr - 1;
  if (node->head.size >= size) {
    return addr;
  }
  // again malloc
  void *new_addr = malloc(size);
  if (new_addr) {
    memcpy(new_addr, addr, size);
    // free old
    free(addr);
  }
  return new_addr;
}
}  // namespace wageco