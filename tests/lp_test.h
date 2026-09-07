#ifndef LP_TEST_H
#define LP_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int lp_test_failures = 0;

#define LP_CHECK(cond)                                                                             \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                        \
            ++lp_test_failures;                                                                    \
        }                                                                                          \
    } while (0)

#define LP_CHECK_STR_EQ(a, b)                                                                      \
    do {                                                                                           \
        const char *lp_a = (a);                                                                    \
        const char *lp_b = (b);                                                                    \
        if (strcmp(lp_a, lp_b) != 0) {                                                             \
            fprintf(stderr, "FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, lp_a, lp_b);     \
            ++lp_test_failures;                                                                    \
        }                                                                                          \
    } while (0)

#define LP_TEST_RETURN() return (lp_test_failures == 0) ? EXIT_SUCCESS : EXIT_FAILURE

#endif /* LP_TEST_H */
