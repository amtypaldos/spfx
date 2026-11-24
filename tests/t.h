/* Tiny test harness */
#ifndef T_H
#define T_H
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int t_fail = 0, t_count = 0;
#define CHECK(cond) do { t_count++; if (!(cond)) { t_fail++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_MSG(cond, ...) do { t_count++; if (!(cond)) { t_fail++; printf("  FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define CHECK_NEAR(a, b, tol) do { double _a = (a), _b = (b); t_count++; if (!(fabs(_a - _b) <= (tol))) { t_fail++; printf("  FAIL %s:%d: %s=%g vs %s=%g (tol %g)\n", __FILE__, __LINE__, #a, _a, #b, _b, (double)(tol)); } } while (0)
#define RUN(fn) do { int _f = t_fail; fn(); printf("%s %s\n", t_fail == _f ? "ok  " : "FAIL", #fn); } while (0)
#define T_DONE() do { printf("%d checks, %d failed\n", t_count, t_fail); return t_fail ? 1 : 0; } while (0)
#endif
