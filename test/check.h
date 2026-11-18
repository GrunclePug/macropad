// Minimal test helpers: CHECK records failures and keeps going.
#pragma once

#include <stdio.h>

extern int failures;
extern int checks;

#define CHECK(cond)                                                \
  do {                                                             \
    checks++;                                                      \
    if (!(cond)) {                                                 \
      failures++;                                                  \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
    }                                                              \
  } while (0)

#define TEST(name) static void name()
#define RUN(name)          \
  do {                     \
    int before = failures; \
    name();                \
    printf("%s %s\n", failures == before ? "ok  " : "FAIL", #name); \
  } while (0)
