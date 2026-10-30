// Headless tests for Aprend's SPIR-V reflection (src/render/aprendreflect.cpp)
// and for writing a block member where the reflection lays it out.
//
// A shader needs a device; its reflection doesn't. This test reflects SPIR-V
// compiled ahead of time (tests/aprendreflect_tests_spirv.h, with the GLSL
// beside each) through the internal header, as aprendranges_tests does with
// the pools' range list. What the pipelines and layouts do with the
// reflection needs a device and isn't tested here.

#include "aprendreflect_internal.hpp"
#include "aprendreflect_tests_spirv.h"
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

// ---- Helpers ------------------------------------------------------------------

template <size_t N>
static bool reflect(const uint32_t (&words)[N], SPUDGPU_SHADER_STAGE stage, aprend_reflection_store *out, std::string *error)
{
    return aprend_reflect_spirv(words, N, stage, out, error);
}

static const APREND_SHADER_RESOURCE *find_resource(const aprend_reflection_store &store, const char *name)
{
    for (const APREND_SHADER_RESOURCE &resource : store.resources)
        if (strcmp(resource.name, name) == 0)
            return &resource;
    return nullptr;
}

static const APREND_SHADER_MEMBER *find_member(const aprend_reflection_store &store, const APREND_SHADER_RESOURCE *resource,
                                               const char *name)
{
    if (!resource)
        return nullptr;
    for (uint32_t i = 0; i < resource->member_count; ++i)
        if (strcmp(store.members[resource->first_member + i].name, name) == 0)
            return &store.members[resource->first_member + i];
    return nullptr;
}

static const APREND_SHADER_INPUT *find_input(const aprend_reflection_store &store, const char *name)
{
    for (const APREND_SHADER_INPUT &input : store.inputs)
        if (strcmp(input.name, name) == 0)
            return &input;
    return nullptr;
}

// ---- Tests --------------------------------------------------------------------

static void test_vertex_shader()
{
    current_test = "test_vertex_shader";
    aprend_reflection_store store;
    std::string error;
    CHECK(reflect(spirv_test_vert, SPUDGPU_SHADER_STAGE_VERTEX, &store, &error));
    CHECK(store.view.stage == SPUDGPU_SHADER_STAGE_VERTEX);
    CHECK(store.view.entry_point && strcmp(store.view.entry_point, "main") == 0);

    // A uniform block, with std140's offsets: a mat3 takes 48 bytes, a vec3
    // and a float share 16.
    const APREND_SHADER_RESOURCE *camera = find_resource(store, "u_Camera");
    CHECK(camera);
    if (camera)
    {
        CHECK(strcmp(camera->block_name, "Camera") == 0);
        CHECK(camera->set == 0 && camera->binding == 0);
        CHECK(camera->type == SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        CHECK(camera->count == 1);
        CHECK(camera->block_size == 128);
        CHECK(camera->used);
        CHECK(camera->member_count == 4);
    }
    const APREND_SHADER_MEMBER *normal_matrix = find_member(store, camera, "normal_matrix");
    CHECK(normal_matrix && normal_matrix->type == APREND_UNIFORM_TYPE_MAT3);
    CHECK(normal_matrix && normal_matrix->offset == 64 && normal_matrix->size == 48 && normal_matrix->matrix_stride == 16);
    const APREND_SHADER_MEMBER *eye = find_member(store, camera, "eye");
    CHECK(eye && eye->type == APREND_UNIFORM_TYPE_VEC3 && eye->offset == 112);
    const APREND_SHADER_MEMBER *exposure = find_member(store, camera, "exposure");
    CHECK(exposure && exposure->type == APREND_UNIFORM_TYPE_FLOAT && exposure->offset == 124);

    // Declared and not used: listed, and marked so.
    const APREND_SHADER_RESOURCE *unused = find_resource(store, "u_Unused");
    CHECK(unused && !unused->used && unused->set == 1);

    // The push constant block starts at its first member, not at 0.
    CHECK(store.view.has_push_constants);
    CHECK(store.view.push_constant_name && strcmp(store.view.push_constant_name, "draw") == 0);
    CHECK(store.view.push_constant_begin == 16);
    CHECK(store.view.push_constant_end == 32);
    CHECK(store.view.push_constant_member_count == 1);

    // Inputs in location order; a mat4 takes four.
    CHECK(store.view.input_count == 5);
    const APREND_SHADER_INPUT *model = find_input(store, "in_model");
    CHECK(model && model->location == 2 && model->location_count == 4 && model->components == 4);
    CHECK(model && model->scalar_type == APREND_SHADER_SCALAR_TYPE_FLOAT);
    const APREND_SHADER_INPUT *ids = find_input(store, "in_ids");
    CHECK(ids && ids->location == 6 && ids->scalar_type == APREND_SHADER_SCALAR_TYPE_INT && ids->components == 2);
    const APREND_SHADER_INPUT *unused_input = find_input(store, "in_unused");
    CHECK(unused_input && !unused_input->used);
    for (uint32_t i = 1; i < store.view.input_count; ++i)
        CHECK(store.view.inputs[i - 1].location < store.view.inputs[i].location);
}

static void test_fragment_shader()
{
    current_test = "test_fragment_shader";
    aprend_reflection_store store;
    std::string error;
    CHECK(reflect(spirv_test_frag, SPUDGPU_SHADER_STAGE_FRAGMENT, &store, &error));
    CHECK(store.view.color_output_count == 2);
    CHECK(!store.view.has_push_constants);
    CHECK(store.view.input_count == 0);

    // An array of structs is its elements' members.
    const APREND_SHADER_RESOURCE *material = find_resource(store, "u_Material");
    CHECK(material && material->set == 1 && material->binding == 0 && material->block_size == 224);
    const APREND_SHADER_MEMBER *light_color = find_member(store, material, "lights[1].color");
    CHECK(light_color && light_color->type == APREND_UNIFORM_TYPE_VEC4 && light_color->offset == 80);
    const APREND_SHADER_MEMBER *radius = find_member(store, material, "lights[0].radius");
    CHECK(radius && radius->offset == 44);
    // A mat2's columns are 16 apart in std140; an array of floats' elements too.
    const APREND_SHADER_MEMBER *uv = find_member(store, material, "uv_transform");
    CHECK(uv && uv->type == APREND_UNIFORM_TYPE_MAT2 && uv->offset == 96 && uv->size == 32 && uv->matrix_stride == 16);
    const APREND_SHADER_MEMBER *weights = find_member(store, material, "weights");
    CHECK(weights && weights->array_count == 3 && weights->array_stride == 16 && weights->offset == 128);
    const APREND_SHADER_MEMBER *rm = find_member(store, material, "rm");
    CHECK(rm && rm->row_major && rm->type == APREND_UNIFORM_TYPE_MAT3 && rm->offset == 176);

    // Images: their type, shape and what they read as.
    const APREND_SHADER_RESOURCE *albedo = find_resource(store, "u_Albedo");
    CHECK(albedo && albedo->type == SPUDGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && albedo->image_shape == APREND_TEXTURE_VIEW_SHAPE_2D);
    CHECK(albedo && albedo->image_scalar_type == APREND_SHADER_SCALAR_TYPE_FLOAT && !albedo->image_depth_compare);
    const APREND_SHADER_RESOURCE *env = find_resource(store, "u_Env");
    CHECK(env && env->image_shape == APREND_TEXTURE_VIEW_SHAPE_CUBE);
    const APREND_SHADER_RESOURCE *layers = find_resource(store, "u_Layers");
    CHECK(layers && layers->type == SPUDGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE && layers->image_shape == APREND_TEXTURE_VIEW_SHAPE_2D_ARRAY);
    const APREND_SHADER_RESOURCE *sampler = find_resource(store, "u_Sampler");
    CHECK(sampler && sampler->type == SPUDGPU_DESCRIPTOR_TYPE_SAMPLER && sampler->image_shape == APREND_TEXTURE_VIEW_SHAPE_NONE);
    const APREND_SHADER_RESOURCE *stencil = find_resource(store, "u_Stencil");
    CHECK(stencil && stencil->image_scalar_type == APREND_SHADER_SCALAR_TYPE_UINT);
    const APREND_SHADER_RESOURCE *shadow = find_resource(store, "u_Shadow");
    CHECK(shadow && shadow->image_depth_compare);
    const APREND_SHADER_RESOURCE *textures = find_resource(store, "u_Textures");
    CHECK(textures && textures->count == 4 && textures->binding == 7);

    // In set and binding order.
    for (uint32_t i = 1; i < store.view.resource_count; ++i)
    {
        const APREND_SHADER_RESOURCE &a = store.view.resources[i - 1];
        const APREND_SHADER_RESOURCE &b = store.view.resources[i];
        CHECK(a.set < b.set || (a.set == b.set && a.binding < b.binding));
    }
}

static void test_compute_shader()
{
    current_test = "test_compute_shader";
    aprend_reflection_store store;
    std::string error;
    CHECK(reflect(spirv_test_comp, SPUDGPU_SHADER_STAGE_COMPUTE, &store, &error));
    CHECK(store.view.workgroup_size[0] == 8 && store.view.workgroup_size[1] == 4 && store.view.workgroup_size[2] == 1);

    // A storage block that ends in an array without a size.
    const APREND_SHADER_RESOURCE *items = find_resource(store, "items");
    CHECK(items && items->type == SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER && items->read_only);
    CHECK(items && items->block_size == 16 && items->runtime_array_stride == 16);
    const APREND_SHADER_MEMBER *list = find_member(store, items, "items");
    CHECK(list && list->array_count == 0 && list->array_stride == 16 && list->offset == 16);

    const APREND_SHADER_RESOURCE *out = find_resource(store, "u_Out");
    CHECK(out && out->type == SPUDGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE && out->image_shape == APREND_TEXTURE_VIEW_SHAPE_2D);
}

static void test_nested_arrays()
{
    current_test = "test_nested_arrays";
    aprend_reflection_store store;
    std::string error;
    CHECK(reflect(spirv_nested_frag, SPUDGPU_SHADER_STAGE_FRAGMENT, &store, &error));
    const APREND_SHADER_RESOURCE *nested = find_resource(store, "u_Nested");
    CHECK(nested && nested->block_size == 192);
    // Structs in arrays in structs in arrays, each element listed.
    const APREND_SHADER_MEMBER *b = find_member(store, nested, "outers[1].inner[1].b");
    CHECK(b && b->type == APREND_UNIFORM_TYPE_VEC2 && b->offset == 72);
    const APREND_SHADER_MEMBER *c = find_member(store, nested, "outers[1].c");
    CHECK(c && c->offset == 80);
    // An array of arrays of a vector: each outer element is an array.
    const APREND_SHADER_MEMBER *grid1 = find_member(store, nested, "grid[1]");
    CHECK(grid1 && grid1->offset == 144 && grid1->array_count == 3 && grid1->array_stride == 16);
    CHECK(nested && nested->member_count == 12);
}

static void test_shared_push_constants()
{
    current_test = "test_shared_push_constants";
    // One block in two stages, each reading part of it: each stage's span
    // is only what it reads.
    aprend_reflection_store vertex;
    aprend_reflection_store fragment;
    std::string error;
    CHECK(reflect(spirv_push_vert, SPUDGPU_SHADER_STAGE_VERTEX, &vertex, &error));
    CHECK(reflect(spirv_push_frag, SPUDGPU_SHADER_STAGE_FRAGMENT, &fragment, &error));
    CHECK(vertex.view.push_constant_begin == 0 && vertex.view.push_constant_end == 64);
    CHECK(fragment.view.push_constant_begin == 64 && fragment.view.push_constant_end == 80);
    CHECK(fragment.view.push_constant_member_count == 2);
    CHECK(!fragment.members[fragment.view.push_constant_first_member].used);
    CHECK(fragment.members[fragment.view.push_constant_first_member + 1].used);
}

static void test_refused()
{
    current_test = "test_refused";
    aprend_reflection_store used;
    std::string error;
    // A texel buffer the entry point reads has no binding in Aprend.
    CHECK(!reflect(spirv_bad_frag, SPUDGPU_SHADER_STAGE_FRAGMENT, &used, &error));
    CHECK(error.find("texel buffer") != std::string::npos);

    // One it only declares is left out.
    aprend_reflection_store unused;
    error.clear();
    CHECK(reflect(spirv_unused_bad_frag, SPUDGPU_SHADER_STAGE_FRAGMENT, &unused, &error));
    CHECK(unused.view.resource_count == 0);

    // No entry point for the stage asked for.
    aprend_reflection_store wrong_stage;
    error.clear();
    CHECK(!reflect(spirv_test_vert, SPUDGPU_SHADER_STAGE_FRAGMENT, &wrong_stage, &error));
    CHECK(error.find("entry point") != std::string::npos);

    // Not SPIR-V.
    static const uint32_t garbage[] = {1, 2, 3, 4, 5, 6};
    aprend_reflection_store not_spirv;
    error.clear();
    CHECK(!reflect(garbage, SPUDGPU_SHADER_STAGE_VERTEX, &not_spirv, &error));
    CHECK(!error.empty());
}

static void test_write_member()
{
    current_test = "test_write_member";
    aprend_reflection_store store;
    std::string error;
    CHECK(reflect(spirv_test_frag, SPUDGPU_SHADER_STAGE_FRAGMENT, &store, &error));
    const APREND_SHADER_RESOURCE *material = find_resource(store, "u_Material");
    if (!material)
        return;
    uint8_t block[224];

    // An array of floats, tightly packed by the caller, 16 apart in the block.
    memset(block, 0xAB, sizeof(block));
    const float weights[3] = {1.0f, 2.0f, 3.0f};
    const APREND_SHADER_MEMBER *w = find_member(store, material, "weights");
    aprend_write_block_member(*w, block, weights);
    for (int i = 0; i < 3; ++i)
    {
        float read;
        memcpy(&read, block + 128 + i * 16, 4);
        CHECK(read == weights[i]);
    }
    CHECK(block[128 + 4] == 0xAB); // the padding after an element is left alone

    // A column-major mat2: two columns of 8, each 16 apart.
    const float m2[4] = {1, 2, 3, 4};
    const APREND_SHADER_MEMBER *uv = find_member(store, material, "uv_transform");
    aprend_write_block_member(*uv, block, m2);
    float column1[2];
    memcpy(column1, block + 96 + 16, 8);
    CHECK(column1[0] == 3 && column1[1] == 4);

    // A row-major mat3 given column-major: transposed into rows 16 apart.
    // Column c, row r of the caller's is 10 * c + r.
    float m3[9];
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            m3[c * 3 + r] = (float)(10 * c + r);
    const APREND_SHADER_MEMBER *rm = find_member(store, material, "rm");
    aprend_write_block_member(*rm, block, m3);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
        {
            float read;
            memcpy(&read, block + 176 + r * 16 + c * 4, 4);
            CHECK(read == (float)(10 * c + r));
        }
}

static void test_type_sizes()
{
    current_test = "test_type_sizes";
    CHECK(aprend_uniform_type_get_size(APREND_UNIFORM_TYPE_MAT2) == 16);
    CHECK(aprend_uniform_type_get_size(APREND_UNIFORM_TYPE_MAT3) == 36);
    CHECK(aprend_uniform_type_get_size(APREND_UNIFORM_TYPE_UNKNOWN) == 0);
    CHECK(aprend_uniform_type_columns(APREND_UNIFORM_TYPE_MAT3) == 3);
    CHECK(aprend_uniform_type_column_size(APREND_UNIFORM_TYPE_MAT3) == 12);
    CHECK(aprend_uniform_type_column_size(APREND_UNIFORM_TYPE_VEC3) == 12);
    CHECK(aprend_uniform_type_columns(APREND_UNIFORM_TYPE_VEC4) == 1);
}

int main()
{
    test_vertex_shader();
    test_fragment_shader();
    test_compute_shader();
    test_nested_arrays();
    test_shared_push_constants();
    test_refused();
    test_write_member();
    test_type_sizes();
    if (failures)
    {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("aprendreflect_tests: all passed\n");
    return 0;
}
