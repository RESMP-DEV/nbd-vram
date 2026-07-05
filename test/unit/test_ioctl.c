/* test/unit/test_ioctl.c - ioctl struct layout tests (PR1 foundation). */
#include <string.h>

#include "p100vram_ioctl.h"
#include "main.h"

static int test_create_disk_layout(void) {
    if (sizeof(struct p100vram_create_disk) != P100VRAM_CREATE_DISK_EXPECTED_SIZE)
        return 1;
    /* name[] should occupy the final 32 bytes (offset 24..55) */
    struct p100vram_create_disk d;
    memset(&d, 0, sizeof(d));
    strncpy(d.name, "shelf0", sizeof(d.name) - 1);
    if (d.name[0] != 's') return 2;
    if (d.name[sizeof(d.name) - 1] != '\0') return 3;
    return 0;
}

const struct test_entry test_ioctl_entries[] = {
    { "create_disk_layout", test_create_disk_layout },
    { NULL, NULL },
};
