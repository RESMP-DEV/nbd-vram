/* test/unit/main.h - shared registry type for the unit test runner. */
#ifndef TEST_UNIT_MAIN_H
#define TEST_UNIT_MAIN_H

typedef int (*test_fn)(void);
struct test_entry { const char *name; test_fn fn; };

#endif
