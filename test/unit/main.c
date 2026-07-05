/* test/unit/main.c - lightweight unit test runner.
 *
 * Auto-registers whatever test_* translation units are linked in, so
 * the same main works whether the tree has only PR1's tests or all of
 * them. Each test module declares its tests via TEST_REGISTER() in its
 * own .c file; this file just iterates the registry.
 *
 * No external test framework - keeps the Mac CI dependency-free.
 */
#include <stdint.h>
#include <stdio.h>

#include "main.h"

/* Each test module provides an array of these, terminated by {NULL,NULL}
 * and named per the convention below. main() pulls them in via weak
 * references so the link succeeds even when a module is absent. */
extern const struct test_entry test_rpc_entries[];
extern const struct test_entry test_ioctl_entries[];
extern const struct test_entry test_policy_entries[];

/* Weak symbols: NULL when the module isn't linked (PR-scoped builds). */
__attribute__((weak)) const struct test_entry test_rpc_entries[]    = {{NULL,NULL}};
__attribute__((weak)) const struct test_entry test_ioctl_entries[]  = {{NULL,NULL}};
__attribute__((weak)) const struct test_entry test_policy_entries[] = {{NULL,NULL}};

static const struct test_entry *const k_registries[] = {
    test_rpc_entries,
    test_ioctl_entries,
    test_policy_entries,
};

int main(void) {
    int failed = 0, total = 0;
    for (size_t r = 0; r < sizeof(k_registries)/sizeof(k_registries[0]); r++) {
        for (const struct test_entry *e = k_registries[r]; e && e->name; e++) {
            int rc = e->fn();
            total++;
            if (rc == 0) {
                printf("[ ok ] %s\n", e->name);
            } else {
                printf("[fail] %s (rc=%d)\n", e->name, rc);
                failed++;
            }
        }
    }
    printf("\n%d tests, %d failed\n", total, failed);
    return failed ? 1 : 0;
}
