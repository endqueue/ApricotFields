// Headless tests for rbtrend_get_optional_gpu_features (rbtrendcontext.h).
//
// Written in C, to prove the header compiles and links as C. What the
// feature checks refuse needs a device, and isn't tested here.

#include "render/rbtrendcontext.h"
#include <stdio.h>

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

// The features only Rend's own code uses, and none that one of its calls
// needs: those the caller asks for itself.
static void test_optional_features(void)
{
    current_test = "optional_features";
    const CRGPU_FEATURES features = rbtrend_get_optional_gpu_features();
    CHECK(features == (CRGPU_FEATURE_MULTI_DRAW_INDIRECT | CRGPU_FEATURE_DEVICE_MAPPABLE_MEMORY));
    CHECK(!(features & CRGPU_FEATURE_PRESENT));
    CHECK(!(features & CRGPU_FEATURE_PASS_TIMESTAMPS));
    CHECK(!(features & CRGPU_FEATURE_MESH_SHADING));
    CHECK(!(features & CRGPU_FEATURE_LAYERED_RENDERING));
    CHECK(!(features & CRGPU_FEATURE_CUBE_ARRAY_VIEWS));
    CHECK(!(features & CRGPU_FEATURE_DEPTH_BIAS_CLAMP));
    CHECK(!(features & CRGPU_FEATURE_INDEPENDENT_BLEND));
}

int main(void)
{
    test_optional_features();

    if (failures)
    {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all Rend feature tests passed\n");
    return 0;
}
