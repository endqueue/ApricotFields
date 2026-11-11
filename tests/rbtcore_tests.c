// Headless tests for what every module shares (include/rbtcore.h): the
// version.
//
// Written in C on purpose: rbtcore.h is a C front end, and this proves it
// compiles and links as one.

#include "rbtcore.h"
#include "archive/rbtarchive.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
static const char *current_test = "";

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            printf("FAIL %s (%s:%d): %s\n", current_test, __FILE__, __LINE__, #condition);                           \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void test_version(void)
{
    current_test = "test_version";

    // Built together, the library and its caller have one version.
    CHECK(rbt_get_version() == RABBITTUS_VERSION);
    CHECK(rbt_get_version() != 0);

    // CMake's project version is read from the header, so the two agree.
    char header[32];
    snprintf(header, sizeof(header), "%u.%u.%u", (unsigned)RABBITTUS_VERSION_MAJOR,
             (unsigned)RABBITTUS_VERSION_MINOR, (unsigned)RABBITTUS_VERSION_PATCH);
    CHECK(strcmp(header, RBTCORE_TEST_CMAKE_VERSION) == 0);

    CHECK(RABBITTUS_VERSION_GET_MAJOR(RABBITTUS_VERSION) == RABBITTUS_VERSION_MAJOR);
    CHECK(RABBITTUS_VERSION_GET_MINOR(RABBITTUS_VERSION) == RABBITTUS_VERSION_MINOR);
    CHECK(RABBITTUS_VERSION_GET_PATCH(RABBITTUS_VERSION) == RABBITTUS_VERSION_PATCH);

    // Each part at its largest, and next to its neighbours.
    const uint32_t top = RABBITTUS_MAKE_VERSION(1023, 1023, 4095);
    CHECK(top == 0xFFFFFFFFu);
    CHECK(RABBITTUS_VERSION_GET_MAJOR(top) == 1023);
    CHECK(RABBITTUS_VERSION_GET_MINOR(top) == 1023);
    CHECK(RABBITTUS_VERSION_GET_PATCH(top) == 4095);
    CHECK(RABBITTUS_VERSION_GET_MINOR(RABBITTUS_MAKE_VERSION(1, 0, 4095)) == 0);
    CHECK(RABBITTUS_VERSION_GET_PATCH(RABBITTUS_MAKE_VERSION(0, 1, 0)) == 0);

    // Packed versions compare as versions.
    CHECK(RABBITTUS_MAKE_VERSION(0, 9, 4095) < RABBITTUS_MAKE_VERSION(0, 10, 0));
    CHECK(RABBITTUS_MAKE_VERSION(0, 1023, 4095) < RABBITTUS_MAKE_VERSION(1, 0, 0));
}

// Debug names on a handle (any module's: an Rabbittus Archive codec table here):
// stored, read back, replaced, removed; NULL objects accepted.
static void test_debug_names(void)
{
    current_test = "test_debug_names";
    rbtarchive_codecs codecs = NULL;
    CHECK(rbtarchive_codecs_create(&codecs) == RBT_RESULT_OK);
    CHECK(rbt_debug_name_get(codecs) == NULL);
    static const char first[] = "codec table";
    rbt_debug_name_set(codecs, first);
    CHECK(rbt_debug_name_get(codecs) == first); // the pointer itself, not a copy
    RBT_SET_DEBUG_NAME(codecs, "another");
    CHECK(strcmp(RBT_GET_DEBUG_NAME(codecs), "another") == 0);
    rbt_debug_name_set(codecs, NULL);
    CHECK(rbt_debug_name_get(codecs) == NULL);
    rbt_debug_name_set(NULL, first);
    CHECK(rbt_debug_name_get(NULL) == NULL);
    rbtarchive_codecs_destroy(codecs);
}

int main(void)
{
    test_version();
    test_debug_names();
    if (failures)
    {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("rbtcore_tests: all passed\n");
    return 0;
}
