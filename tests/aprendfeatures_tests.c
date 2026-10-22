// Headless tests for aprend_get_optional_gpu_features (aprendcontext.h).
//
// Written in C, to prove the header compiles and links as C. What the
// feature checks refuse needs a device, and isn't tested here.

#include "render/aprendcontext.h"
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

// The features only Aprend's own code uses, and none that one of its calls
// needs: those the caller asks for itself.
static void test_optional_features(void)
{
    current_test = "optional_features";
    const SPUDGPU_FEATURES features = aprend_get_optional_gpu_features();
    CHECK(features == (SPUDGPU_FEATURE_MULTI_DRAW_INDIRECT | SPUDGPU_FEATURE_DEVICE_MAPPABLE_MEMORY));
    CHECK(!(features & SPUDGPU_FEATURE_PRESENT));
    CHECK(!(features & SPUDGPU_FEATURE_PASS_TIMESTAMPS));
    CHECK(!(features & SPUDGPU_FEATURE_MESH_SHADING));
    CHECK(!(features & SPUDGPU_FEATURE_LAYERED_RENDERING));
    CHECK(!(features & SPUDGPU_FEATURE_CUBE_ARRAY_VIEWS));
    CHECK(!(features & SPUDGPU_FEATURE_DEPTH_BIAS_CLAMP));
    CHECK(!(features & SPUDGPU_FEATURE_INDEPENDENT_BLEND));
}

int main(void)
{
    test_optional_features();

    if (failures)
    {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all Aprend feature tests passed\n");
    return 0;
}
