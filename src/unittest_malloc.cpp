#include <gtest/gtest.h>
#include <stdlib.h>

#include "mmemory.h"

// 需要安装 gtest
// g++ unittest_malloc.cpp mmemory.cpp -lgtest -DDEBUG -o test&&./test

const size_t const_test = INT16_MAX * 100;
void test_malloc(size_t count) {
  size_t size = std::rand() % 1000;
  void* q = malloc(size);
  void* m = malloc(size);
  free(q);
  for (int i = 0; i < count; ++i) {
    void* p = malloc(size);
    if (p)
      free(p);
  }
  free(m);
}
void test_my_malloc(size_t count) {
  size_t size = std::rand() % 1000;
  void* q = wageco::malloc(size);
  void* m = wageco::malloc(size);
  wageco::free(q);
  for (int i = 0; i < count; ++i) {
    void* p = wageco::malloc(size);
    if (p)
      wageco::free(p);
  }
  wageco::free(m);
}

TEST(testMalloc, MallocTest) {
  test_malloc(const_test);
}
TEST(testMalloc, MyMallocTest) {
  test_my_malloc(const_test);
}