# MMEMORY

A simple stack memory allocator.

底层用了2个双链表，分别表示分配以及释放的空间。

实属逆向优化 :D

```cpp
[==========] Running 2 tests from 1 test suite.
[----------] Global test environment set-up.
[----------] 2 tests from testMalloc
[ RUN      ] testMalloc.MallocTest
[       OK ] testMalloc.MallocTest (53 ms)
[ RUN      ] testMalloc.MyMallocTest
Debug@malloc alloc 912, user 886, start:0x5635ed857390
Debug@malloc alloc 912, user 886, start:0x5635ed857720
Debug@free 912, start:0x5635ed857720
[       OK ] testMalloc.MyMallocTest (187 ms)
[----------] 2 tests from testMalloc (240 ms total)

[----------] Global test environment tear-down
[==========] 2 tests from 1 test suite ran. (241 ms total)
[  PASSED  ] 2 tests.
```
