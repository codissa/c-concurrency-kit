#ifndef CK_TEST_H
#define CK_TEST_H

/* Minimal dependency-free test harness. */

#include <stdio.h>
#include <stdlib.h>

static int ck_test_failures;

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ck_test_failures++;                                                 \
        }                                                                       \
    } while (0)

#define CHECK_EQ(a, b)                                                          \
    do {                                                                        \
        long long _a = (long long)(a), _b = (long long)(b);                     \
        if (_a != _b) {                                                         \
            fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld != %lld)\n",          \
                    __FILE__, __LINE__, #a, #b, _a, _b);                        \
            ck_test_failures++;                                                 \
        }                                                                       \
    } while (0)

#define RUN(test)                                                               \
    do {                                                                        \
        int _before = ck_test_failures;                                         \
        test();                                                                 \
        printf("%-4s %s\n", ck_test_failures == _before ? "ok" : "FAIL", #test); \
    } while (0)

#define TEST_EXIT() return ck_test_failures ? EXIT_FAILURE : EXIT_SUCCESS

#endif /* CK_TEST_H */
