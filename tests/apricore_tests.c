// Headless tests for what every module shares (include/apricore.h): the
// version.
//
// Written in C on purpose: apricore.h is a C front end, and this proves it
// compiles and links as one.

#include "apricore.h"
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
    CHECK(apri_get_version() == APRICOTFIELDS_VERSION);
    CHECK(apri_get_version() != 0);

    // CMake's project version is read from the header, so the two agree.
    char header[32];
    snprintf(header, sizeof(header), "%u.%u.%u", (unsigned)APRICOTFIELDS_VERSION_MAJOR,
             (unsigned)APRICOTFIELDS_VERSION_MINOR, (unsigned)APRICOTFIELDS_VERSION_PATCH);
    CHECK(strcmp(header, APRICORE_TEST_CMAKE_VERSION) == 0);

    CHECK(APRICOTFIELDS_VERSION_GET_MAJOR(APRICOTFIELDS_VERSION) == APRICOTFIELDS_VERSION_MAJOR);
    CHECK(APRICOTFIELDS_VERSION_GET_MINOR(APRICOTFIELDS_VERSION) == APRICOTFIELDS_VERSION_MINOR);
    CHECK(APRICOTFIELDS_VERSION_GET_PATCH(APRICOTFIELDS_VERSION) == APRICOTFIELDS_VERSION_PATCH);

    // Each part at its largest, and next to its neighbours.
    const uint32_t top = APRICOTFIELDS_MAKE_VERSION(1023, 1023, 4095);
    CHECK(top == 0xFFFFFFFFu);
    CHECK(APRICOTFIELDS_VERSION_GET_MAJOR(top) == 1023);
    CHECK(APRICOTFIELDS_VERSION_GET_MINOR(top) == 1023);
    CHECK(APRICOTFIELDS_VERSION_GET_PATCH(top) == 4095);
    CHECK(APRICOTFIELDS_VERSION_GET_MINOR(APRICOTFIELDS_MAKE_VERSION(1, 0, 4095)) == 0);
    CHECK(APRICOTFIELDS_VERSION_GET_PATCH(APRICOTFIELDS_MAKE_VERSION(0, 1, 0)) == 0);

    // Packed versions compare as versions.
    CHECK(APRICOTFIELDS_MAKE_VERSION(0, 9, 4095) < APRICOTFIELDS_MAKE_VERSION(0, 10, 0));
    CHECK(APRICOTFIELDS_MAKE_VERSION(0, 1023, 4095) < APRICOTFIELDS_MAKE_VERSION(1, 0, 0));
}

int main(void)
{
    test_version();
    if (failures)
    {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("apricore_tests: all passed\n");
    return 0;
}
