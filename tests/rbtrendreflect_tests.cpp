// Headless tests for Rend's reflection and for writing a block member
// where the reflection lays it out.
//
// A shader needs a device; its reflection doesn't. The reflection is made at
// build time (tools/rbtrend_reflect, card X18) and read by the library from
// the package's table (src/render/rbtrendreflect_table.cpp). This test builds
// the tool's reflection and table writer in, reflects SPIR-V compiled ahead
// of time (tests/rbtrendreflect_tests_spirv.h, with the GLSL beside each),
// writes each table and reads it back through the library's reader, and
// checks the view read back is the one reflected, field for field, before
// every case checks it. It also checks the tables the build wrote for
// Rend's own shaders are the writer's, byte for byte, and every refusal
// of the reader, the table cut at every length. What the pipelines and
// layouts do with the reflection needs a device and isn't tested here.

#include "rbtrend_reflect_tool.hpp"
#include "rbtrendreflect_tests_spirv.h"
// Rend's own shaders as its build compiles them with Slang
// (cmake/rbtrend_slang.cmake), for what reflection reads differently there.
#include "rbtrend_exposure_adapt_histogram_frag.spv.inc"
#include "rbtrend_exposure_histogram_comp.spv.inc"
#include "rbtrend_lit_cull_comp.spv.inc"
#include "rbtrend_lit_frag.spv.inc"
#include "rbtrend_lit_shadow_vert.spv.inc"
#include "rbtrend_lit_vert.spv.inc"
// The tables the build wrote for them (cmake/rbtrend_slang.cmake).
#include "rbtrend_exposure_adapt_histogram_frag.reflection.inc"
#include "rbtrend_exposure_histogram_comp.reflection.inc"
#include "rbtrend_lit_cull_comp.reflection.inc"
#include "rbtrend_lit_frag.reflection.inc"
#include "rbtrend_lit_shadow_vert.reflection.inc"
#include "rbtrend_lit_vert.reflection.inc"
// Slang's reflection JSON of one, whose SPIR-V drops inputs it declares.
#include "rbtrend_lit_shadow_vert.reflection.json.inc"
#include <stddef.h>
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

static bool same_string(const char *a, const char *b)
{
    return (!a && !b) || (a && b && strcmp(a, b) == 0);
}

// [read], read back from its table, is [reflected], field for field.
static void check_same(const rbtrend_reflection_store &reflected, const rbtrend_reflection_store &read)
{
    const RBTREND_SHADER_REFLECTION &a = reflected.view;
    const RBTREND_SHADER_REFLECTION &b = read.view;
    CHECK(a.stage == b.stage);
    CHECK(same_string(a.entry_point, b.entry_point));
    CHECK(a.has_push_constants == b.has_push_constants);
    CHECK(same_string(a.push_constant_name, b.push_constant_name));
    CHECK(a.push_constant_begin == b.push_constant_begin && a.push_constant_end == b.push_constant_end);
    CHECK(a.push_constant_first_member == b.push_constant_first_member && a.push_constant_member_count == b.push_constant_member_count);
    CHECK(a.color_output_count == b.color_output_count);
    CHECK(memcmp(a.workgroup_size, b.workgroup_size, sizeof(a.workgroup_size)) == 0);
    CHECK(a.resource_count == b.resource_count && a.member_count == b.member_count && a.input_count == b.input_count);
    for (uint32_t i = 0; i < a.resource_count && i < b.resource_count; ++i)
    {
        const RBTREND_SHADER_RESOURCE &x = a.resources[i];
        const RBTREND_SHADER_RESOURCE &y = b.resources[i];
        CHECK(same_string(x.name, y.name) && same_string(x.block_name, y.block_name));
        CHECK(x.set == y.set && x.binding == y.binding && x.type == y.type && x.count == y.count);
        CHECK(x.block_size == y.block_size && x.runtime_array_stride == y.runtime_array_stride);
        CHECK(x.first_member == y.first_member && x.member_count == y.member_count);
        CHECK(x.image_shape == y.image_shape && x.image_scalar_type == y.image_scalar_type);
        CHECK(x.image_depth_compare == y.image_depth_compare && x.read_only == y.read_only && x.used == y.used);
    }
    for (uint32_t i = 0; i < a.member_count && i < b.member_count; ++i)
    {
        const RBTREND_SHADER_MEMBER &x = a.members[i];
        const RBTREND_SHADER_MEMBER &y = b.members[i];
        CHECK(same_string(x.name, y.name) && x.type == y.type && x.offset == y.offset && x.size == y.size);
        CHECK(x.array_count == y.array_count && x.array_stride == y.array_stride && x.matrix_stride == y.matrix_stride);
        CHECK(x.row_major == y.row_major && x.used == y.used);
    }
    for (uint32_t i = 0; i < a.input_count && i < b.input_count; ++i)
    {
        const RBTREND_SHADER_INPUT &x = a.inputs[i];
        const RBTREND_SHADER_INPUT &y = b.inputs[i];
        CHECK(same_string(x.name, y.name) && x.location == y.location && x.location_count == y.location_count);
        CHECK(x.scalar_type == y.scalar_type && x.components == y.components && x.used == y.used);
    }
    CHECK(reflected.storage_formats.size() == read.storage_formats.size());
    for (size_t i = 0; i < reflected.storage_formats.size() && i < read.storage_formats.size(); ++i)
        CHECK(reflected.storage_formats[i].set == read.storage_formats[i].set &&
              reflected.storage_formats[i].binding == read.storage_formats[i].binding &&
              reflected.storage_formats[i].format == read.storage_formats[i].format);
    CHECK(reflected.output_types == read.output_types);
}

// [words] reflected as the tool does, written as its table and read back
// into [out] as rbtrend_shader_create() does, the two views compared field
// for field. False, with [error], where the tool would refuse the SPIR-V.
template <size_t N>
static bool reflect(const uint32_t (&words)[N], CRGPU_SHADER_STAGE stage, rbtrend_reflection_store *out, std::string *error)
{
    rbtrend_reflection_store reflected;
    if (!rbtrend_reflect_spirv(words, N, stage, &reflected, error))
        return false;
    std::vector<uint8_t> table;
    CHECK(rbtrend_reflect_table_write(reflected, &table, error));
    const char *entry_point = reflected.view.entry_point ? reflected.view.entry_point : "";
    const RBT_RESULT result = rbtrend_reflect_table_read(table.data(), table.size(), stage, entry_point, out, error);
    CHECK(result == RBT_RESULT_OK);
    if (result != RBT_RESULT_OK)
        return false;
    check_same(reflected, *out);
    return true;
}

static const RBTREND_SHADER_RESOURCE *find_resource(const rbtrend_reflection_store &store, const char *name)
{
    for (const RBTREND_SHADER_RESOURCE &resource : store.resources)
        if (strcmp(resource.name, name) == 0)
            return &resource;
    return nullptr;
}

static const RBTREND_SHADER_MEMBER *find_member(const rbtrend_reflection_store &store, const RBTREND_SHADER_RESOURCE *resource,
                                               const char *name)
{
    if (!resource)
        return nullptr;
    for (uint32_t i = 0; i < resource->member_count; ++i)
        if (strcmp(store.members[resource->first_member + i].name, name) == 0)
            return &store.members[resource->first_member + i];
    return nullptr;
}

static const RBTREND_SHADER_INPUT *find_input(const rbtrend_reflection_store &store, const char *name)
{
    for (const RBTREND_SHADER_INPUT &input : store.inputs)
        if (strcmp(input.name, name) == 0)
            return &input;
    return nullptr;
}

// ---- Tests --------------------------------------------------------------------

static void test_vertex_shader()
{
    current_test = "test_vertex_shader";
    rbtrend_reflection_store store;
    std::string error;
    CHECK(reflect(spirv_test_vert, CRGPU_SHADER_STAGE_VERTEX, &store, &error));
    CHECK(store.view.stage == CRGPU_SHADER_STAGE_VERTEX);
    CHECK(store.view.entry_point && strcmp(store.view.entry_point, "main") == 0);

    // A uniform block, with std140's offsets: a mat3 takes 48 bytes, a vec3
    // and a float share 16.
    const RBTREND_SHADER_RESOURCE *camera = find_resource(store, "u_Camera");
    CHECK(camera);
    if (camera)
    {
        CHECK(strcmp(camera->block_name, "Camera") == 0);
        CHECK(camera->set == 0 && camera->binding == 0);
        CHECK(camera->type == CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        CHECK(camera->count == 1);
        CHECK(camera->block_size == 128);
        CHECK(camera->used);
        CHECK(camera->member_count == 4);
    }
    const RBTREND_SHADER_MEMBER *normal_matrix = find_member(store, camera, "normal_matrix");
    CHECK(normal_matrix && normal_matrix->type == RBTREND_UNIFORM_TYPE_MAT3);
    CHECK(normal_matrix && normal_matrix->offset == 64 && normal_matrix->size == 48 && normal_matrix->matrix_stride == 16);
    const RBTREND_SHADER_MEMBER *eye = find_member(store, camera, "eye");
    CHECK(eye && eye->type == RBTREND_UNIFORM_TYPE_VEC3 && eye->offset == 112);
    const RBTREND_SHADER_MEMBER *exposure = find_member(store, camera, "exposure");
    CHECK(exposure && exposure->type == RBTREND_UNIFORM_TYPE_FLOAT && exposure->offset == 124);

    // Declared and not used: listed, and marked so.
    const RBTREND_SHADER_RESOURCE *unused = find_resource(store, "u_Unused");
    CHECK(unused && !unused->used && unused->set == 1);

    // The push constant block starts at its first member, not at 0.
    CHECK(store.view.has_push_constants);
    CHECK(store.view.push_constant_name && strcmp(store.view.push_constant_name, "draw") == 0);
    CHECK(store.view.push_constant_begin == 16);
    CHECK(store.view.push_constant_end == 32);
    CHECK(store.view.push_constant_member_count == 1);

    // Inputs in location order; a mat4 takes four.
    CHECK(store.view.input_count == 5);
    const RBTREND_SHADER_INPUT *model = find_input(store, "in_model");
    CHECK(model && model->location == 2 && model->location_count == 4 && model->components == 4);
    CHECK(model && model->scalar_type == RBTREND_SHADER_SCALAR_TYPE_FLOAT);
    const RBTREND_SHADER_INPUT *ids = find_input(store, "in_ids");
    CHECK(ids && ids->location == 6 && ids->scalar_type == RBTREND_SHADER_SCALAR_TYPE_INT && ids->components == 2);
    const RBTREND_SHADER_INPUT *unused_input = find_input(store, "in_unused");
    CHECK(unused_input && !unused_input->used);
    for (uint32_t i = 1; i < store.view.input_count; ++i)
        CHECK(store.view.inputs[i - 1].location < store.view.inputs[i].location);
}

static void test_fragment_shader()
{
    current_test = "test_fragment_shader";
    rbtrend_reflection_store store;
    std::string error;
    CHECK(reflect(spirv_test_frag, CRGPU_SHADER_STAGE_FRAGMENT, &store, &error));
    CHECK(store.view.color_output_count == 2);
    CHECK(!store.view.has_push_constants);
    CHECK(store.view.input_count == 0);

    // An array of structs is its elements' members.
    const RBTREND_SHADER_RESOURCE *material = find_resource(store, "u_Material");
    CHECK(material && material->set == 1 && material->binding == 0 && material->block_size == 224);
    const RBTREND_SHADER_MEMBER *light_color = find_member(store, material, "lights[1].color");
    CHECK(light_color && light_color->type == RBTREND_UNIFORM_TYPE_VEC4 && light_color->offset == 80);
    const RBTREND_SHADER_MEMBER *radius = find_member(store, material, "lights[0].radius");
    CHECK(radius && radius->offset == 44);
    // A mat2's columns are 16 apart in std140; an array of floats' elements too.
    const RBTREND_SHADER_MEMBER *uv = find_member(store, material, "uv_transform");
    CHECK(uv && uv->type == RBTREND_UNIFORM_TYPE_MAT2 && uv->offset == 96 && uv->size == 32 && uv->matrix_stride == 16);
    const RBTREND_SHADER_MEMBER *weights = find_member(store, material, "weights");
    CHECK(weights && weights->array_count == 3 && weights->array_stride == 16 && weights->offset == 128);
    const RBTREND_SHADER_MEMBER *rm = find_member(store, material, "rm");
    CHECK(rm && rm->row_major && rm->type == RBTREND_UNIFORM_TYPE_MAT3 && rm->offset == 176);

    // Images: their type, shape and what they read as.
    const RBTREND_SHADER_RESOURCE *albedo = find_resource(store, "u_Albedo");
    CHECK(albedo && albedo->type == CRGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && albedo->image_shape == RBTREND_TEXTURE_VIEW_SHAPE_2D);
    CHECK(albedo && albedo->image_scalar_type == RBTREND_SHADER_SCALAR_TYPE_FLOAT && !albedo->image_depth_compare);
    const RBTREND_SHADER_RESOURCE *env = find_resource(store, "u_Env");
    CHECK(env && env->image_shape == RBTREND_TEXTURE_VIEW_SHAPE_CUBE);
    const RBTREND_SHADER_RESOURCE *layers = find_resource(store, "u_Layers");
    CHECK(layers && layers->type == CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE && layers->image_shape == RBTREND_TEXTURE_VIEW_SHAPE_2D_ARRAY);
    const RBTREND_SHADER_RESOURCE *sampler = find_resource(store, "u_Sampler");
    CHECK(sampler && sampler->type == CRGPU_DESCRIPTOR_TYPE_SAMPLER && sampler->image_shape == RBTREND_TEXTURE_VIEW_SHAPE_NONE);
    const RBTREND_SHADER_RESOURCE *stencil = find_resource(store, "u_Stencil");
    CHECK(stencil && stencil->image_scalar_type == RBTREND_SHADER_SCALAR_TYPE_UINT);
    const RBTREND_SHADER_RESOURCE *shadow = find_resource(store, "u_Shadow");
    CHECK(shadow && shadow->image_depth_compare);
    const RBTREND_SHADER_RESOURCE *textures = find_resource(store, "u_Textures");
    CHECK(textures && textures->count == 4 && textures->binding == 7);

    // In set and binding order.
    for (uint32_t i = 1; i < store.view.resource_count; ++i)
    {
        const RBTREND_SHADER_RESOURCE &a = store.view.resources[i - 1];
        const RBTREND_SHADER_RESOURCE &b = store.view.resources[i];
        CHECK(a.set < b.set || (a.set == b.set && a.binding < b.binding));
    }
}

static void test_compute_shader()
{
    current_test = "test_compute_shader";
    rbtrend_reflection_store store;
    std::string error;
    CHECK(reflect(spirv_test_comp, CRGPU_SHADER_STAGE_COMPUTE, &store, &error));
    CHECK(store.view.workgroup_size[0] == 8 && store.view.workgroup_size[1] == 4 && store.view.workgroup_size[2] == 1);

    // A storage block that ends in an array without a size.
    const RBTREND_SHADER_RESOURCE *items = find_resource(store, "items");
    CHECK(items && items->type == CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER && items->read_only);
    CHECK(items && items->block_size == 16 && items->runtime_array_stride == 16);
    const RBTREND_SHADER_MEMBER *list = find_member(store, items, "items");
    CHECK(list && list->array_count == 0 && list->array_stride == 16 && list->offset == 16);

    const RBTREND_SHADER_RESOURCE *out = find_resource(store, "u_Out");
    CHECK(out && out->type == CRGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE && out->image_shape == RBTREND_TEXTURE_VIEW_SHAPE_2D);
}

static void test_nested_arrays()
{
    current_test = "test_nested_arrays";
    rbtrend_reflection_store store;
    std::string error;
    CHECK(reflect(spirv_nested_frag, CRGPU_SHADER_STAGE_FRAGMENT, &store, &error));
    const RBTREND_SHADER_RESOURCE *nested = find_resource(store, "u_Nested");
    CHECK(nested && nested->block_size == 192);
    // Structs in arrays in structs in arrays, each element listed.
    const RBTREND_SHADER_MEMBER *b = find_member(store, nested, "outers[1].inner[1].b");
    CHECK(b && b->type == RBTREND_UNIFORM_TYPE_VEC2 && b->offset == 72);
    const RBTREND_SHADER_MEMBER *c = find_member(store, nested, "outers[1].c");
    CHECK(c && c->offset == 80);
    // An array of arrays of a vector: each outer element is an array.
    const RBTREND_SHADER_MEMBER *grid1 = find_member(store, nested, "grid[1]");
    CHECK(grid1 && grid1->offset == 144 && grid1->array_count == 3 && grid1->array_stride == 16);
    CHECK(nested && nested->member_count == 12);
}

static void test_shared_push_constants()
{
    current_test = "test_shared_push_constants";
    // One block in two stages, each reading part of it: each stage's span
    // is only what it reads.
    rbtrend_reflection_store vertex;
    rbtrend_reflection_store fragment;
    std::string error;
    CHECK(reflect(spirv_push_vert, CRGPU_SHADER_STAGE_VERTEX, &vertex, &error));
    CHECK(reflect(spirv_push_frag, CRGPU_SHADER_STAGE_FRAGMENT, &fragment, &error));
    CHECK(vertex.view.push_constant_begin == 0 && vertex.view.push_constant_end == 64);
    CHECK(fragment.view.push_constant_begin == 64 && fragment.view.push_constant_end == 80);
    CHECK(fragment.view.push_constant_member_count == 2);
    CHECK(!fragment.members[fragment.view.push_constant_first_member].used);
    CHECK(fragment.members[fragment.view.push_constant_first_member + 1].used);
}

static void test_refused()
{
    current_test = "test_refused";
    rbtrend_reflection_store used;
    std::string error;
    // A texel buffer the entry point reads has no binding in Rend.
    CHECK(!reflect(spirv_bad_frag, CRGPU_SHADER_STAGE_FRAGMENT, &used, &error));
    CHECK(error.find("texel buffer") != std::string::npos);

    // One it only declares is left out.
    rbtrend_reflection_store unused;
    error.clear();
    CHECK(reflect(spirv_unused_bad_frag, CRGPU_SHADER_STAGE_FRAGMENT, &unused, &error));
    CHECK(unused.view.resource_count == 0);

    // No entry point for the stage asked for.
    rbtrend_reflection_store wrong_stage;
    error.clear();
    CHECK(!reflect(spirv_test_vert, CRGPU_SHADER_STAGE_FRAGMENT, &wrong_stage, &error));
    CHECK(error.find("entry point") != std::string::npos);

    // Not SPIR-V.
    static const uint32_t garbage[] = {1, 2, 3, 4, 5, 6};
    rbtrend_reflection_store not_spirv;
    error.clear();
    CHECK(!reflect(garbage, CRGPU_SHADER_STAGE_VERTEX, &not_spirv, &error));
    CHECK(!error.empty());
}

static void test_write_member()
{
    current_test = "test_write_member";
    rbtrend_reflection_store store;
    std::string error;
    CHECK(reflect(spirv_test_frag, CRGPU_SHADER_STAGE_FRAGMENT, &store, &error));
    const RBTREND_SHADER_RESOURCE *material = find_resource(store, "u_Material");
    if (!material)
        return;
    uint8_t block[224];

    // An array of floats, tightly packed by the caller, 16 apart in the block.
    memset(block, 0xAB, sizeof(block));
    const float weights[3] = {1.0f, 2.0f, 3.0f};
    const RBTREND_SHADER_MEMBER *w = find_member(store, material, "weights");
    rbtrend_write_block_member(*w, block, weights);
    for (int i = 0; i < 3; ++i)
    {
        float read;
        memcpy(&read, block + 128 + i * 16, 4);
        CHECK(read == weights[i]);
    }
    CHECK(block[128 + 4] == 0xAB); // the padding after an element is left alone

    // A column-major mat2: two columns of 8, each 16 apart.
    const float m2[4] = {1, 2, 3, 4};
    const RBTREND_SHADER_MEMBER *uv = find_member(store, material, "uv_transform");
    rbtrend_write_block_member(*uv, block, m2);
    float column1[2];
    memcpy(column1, block + 96 + 16, 8);
    CHECK(column1[0] == 3 && column1[1] == 4);

    // A row-major mat3 given column-major: transposed into rows 16 apart.
    // Column c, row r of the caller's is 10 * c + r.
    float m3[9];
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            m3[c * 3 + r] = (float)(10 * c + r);
    const RBTREND_SHADER_MEMBER *rm = find_member(store, material, "rm");
    rbtrend_write_block_member(*rm, block, m3);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
        {
            float read;
            memcpy(&read, block + 176 + r * 16 + c * 4, 4);
            CHECK(read == (float)(10 * c + r));
        }
}

// Slang's SPIR-V read as the source declares it (RBTREND_TODO.md, item 22):
// block names without _std140, arrays seen through the struct Slang wraps
// them in, matrices column-major, inputs by their field's name, and a
// storage buffer read only unless the code writes through it.
static void test_slang_shaders()
{
    current_test = "test_slang_shaders";
    std::string error;
    {
        rbtrend_reflection_store store;
        CHECK(reflect(rbtrend_lit_frag_spirv, CRGPU_SHADER_STAGE_FRAGMENT, &store, &error));
        CHECK(store.view.entry_point && strcmp(store.view.entry_point, "rbtrend_lit_frag") == 0);
        const RBTREND_SHADER_RESOURCE *scene = find_resource(store, "u_lit_scene");
        CHECK(scene && strcmp(scene->block_name, "RbtrendLitScene") == 0);
        const RBTREND_SHADER_MEMBER *matrix = find_member(store, scene, "view_projection");
        CHECK(matrix && matrix->type == RBTREND_UNIFORM_TYPE_MAT4 && !matrix->row_major && matrix->matrix_stride == 16 && matrix->offset == 0);
        // A matrix in an array of structs: each element's members by name,
        // 80 bytes apart (a matrix and a rectangle).
        const RBTREND_SHADER_MEMBER *view = find_member(store, scene, "shadow_views[1].view_projection");
        CHECK(view && view->type == RBTREND_UNIFORM_TYPE_MAT4 && view->offset == 128 + 80 && !view->row_major && view->matrix_stride == 16);
        const RBTREND_SHADER_MEMBER *rect = find_member(store, scene, "shadow_views[1].rect");
        CHECK(rect && rect->type == RBTREND_UNIFORM_TYPE_VEC4 && rect->offset == 128 + 80 + 64);
        const RBTREND_SHADER_MEMBER *light = find_member(store, scene, "lights[1].radiance_type");
        CHECK(light && light->type == RBTREND_UNIFORM_TYPE_VEC4 && light->offset == 7824 + 64 + 16);
        // An array of vectors: one member, its count and stride.
        const RBTREND_SHADER_MEMBER *rotation = find_member(store, scene, "environment_rotation");
        CHECK(rotation && rotation->type == RBTREND_UNIFORM_TYPE_VEC4 && rotation->array_count == 3 && rotation->array_stride == 16 &&
              rotation->offset == 128 + 80 * 64 + 80 * 4 + 32 * 64 + 80);
        CHECK(find_member(store, scene, "lights.data[0].radiance_type") == nullptr);
        const RBTREND_SHADER_RESOURCE *material = find_resource(store, "u_lit_material");
        CHECK(material && strcmp(material->block_name, "RbtrendLitMaterial") == 0);
        const RBTREND_SHADER_MEMBER *uv = find_member(store, material, "uv_transform");
        CHECK(uv && uv->type == RBTREND_UNIFORM_TYPE_VEC4 && uv->array_count == 2 && uv->array_stride == 16 && uv->offset == 96);
        const RBTREND_SHADER_RESOURCE *atlas = find_resource(store, "shadow_atlas");
        CHECK(atlas && atlas->image_shape == RBTREND_TEXTURE_VIEW_SHAPE_2D && atlas->image_depth_compare);
        const RBTREND_SHADER_RESOURCE *base = find_resource(store, "base_color_map");
        CHECK(base && base->image_shape == RBTREND_TEXTURE_VIEW_SHAPE_2D && !base->image_depth_compare);
        const RBTREND_SHADER_RESOURCE *tiles = find_resource(store, "RbtrendLitTiles");
        CHECK(tiles && tiles->type == CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER && strcmp(tiles->block_name, "RbtrendLitTiles") == 0 &&
              tiles->read_only);
    }
    {
        rbtrend_reflection_store store;
        CHECK(reflect(rbtrend_lit_cull_comp_spirv, CRGPU_SHADER_STAGE_COMPUTE, &store, &error));
        const RBTREND_SHADER_RESOURCE *tiles = find_resource(store, "RbtrendLitTiles");
        CHECK(tiles && !tiles->read_only);
        CHECK(store.view.workgroup_size[0] == 64 && store.view.workgroup_size[1] == 1 && store.view.workgroup_size[2] == 1);
    }
    {
        rbtrend_reflection_store store;
        CHECK(reflect(rbtrend_exposure_histogram_comp_spirv, CRGPU_SHADER_STAGE_COMPUTE, &store, &error));
        const RBTREND_SHADER_RESOURCE *bins = find_resource(store, "RbtrendExposureBins");
        CHECK(bins && !bins->read_only && strcmp(bins->block_name, "RbtrendExposureBins") == 0 && bins->runtime_array_stride == 4);
    }
    {
        rbtrend_reflection_store store;
        CHECK(reflect(rbtrend_exposure_adapt_histogram_frag_spirv, CRGPU_SHADER_STAGE_FRAGMENT, &store, &error));
        const RBTREND_SHADER_RESOURCE *bins = find_resource(store, "RbtrendExposureBins");
        CHECK(bins && bins->read_only);
    }
    {
        rbtrend_reflection_store store;
        CHECK(reflect(rbtrend_lit_vert_spirv, CRGPU_SHADER_STAGE_VERTEX, &store, &error));
        const RBTREND_SHADER_INPUT *position = find_input(store, "position");
        CHECK(position && position->location == 0 && position->components == 3 && position->used);
        const RBTREND_SHADER_INPUT *transform = find_input(store, "transform3");
        CHECK(transform && transform->location == 7);
        const RBTREND_SHADER_RESOURCE *scene = find_resource(store, "u_lit_scene");
        CHECK(scene && strcmp(scene->block_name, "RbtrendLitScene") == 0);
    }
}

static void test_type_sizes()
{
    current_test = "test_type_sizes";
    CHECK(rbtrend_uniform_type_get_size(RBTREND_UNIFORM_TYPE_MAT2) == 16);
    CHECK(rbtrend_uniform_type_get_size(RBTREND_UNIFORM_TYPE_MAT3) == 36);
    CHECK(rbtrend_uniform_type_get_size(RBTREND_UNIFORM_TYPE_UNKNOWN) == 0);
    CHECK(rbtrend_uniform_type_columns(RBTREND_UNIFORM_TYPE_MAT3) == 3);
    CHECK(rbtrend_uniform_type_column_size(RBTREND_UNIFORM_TYPE_MAT3) == 12);
    CHECK(rbtrend_uniform_type_column_size(RBTREND_UNIFORM_TYPE_VEC3) == 12);
    CHECK(rbtrend_uniform_type_columns(RBTREND_UNIFORM_TYPE_VEC4) == 1);
}

// The tables the build wrote for Rend's own shaders (tools/rbtrend_reflect,
// through cmake/rbtrend_slang.cmake) are the writer's, byte for byte.
static void test_tool_output()
{
    current_test = "test_tool_output";
    struct shader
    {
        const uint32_t *spirv;
        size_t words;
        CRGPU_SHADER_STAGE stage;
        const uint32_t *table;
        uint64_t table_size;
    } shaders[5] = {
        {rbtrend_lit_frag_spirv, sizeof(rbtrend_lit_frag_spirv) / 4, CRGPU_SHADER_STAGE_FRAGMENT, rbtrend_lit_frag_reflection, rbtrend_lit_frag_reflection_size},
        {rbtrend_lit_vert_spirv, sizeof(rbtrend_lit_vert_spirv) / 4, CRGPU_SHADER_STAGE_VERTEX, rbtrend_lit_vert_reflection, rbtrend_lit_vert_reflection_size},
        {rbtrend_lit_cull_comp_spirv, sizeof(rbtrend_lit_cull_comp_spirv) / 4, CRGPU_SHADER_STAGE_COMPUTE, rbtrend_lit_cull_comp_reflection,
         rbtrend_lit_cull_comp_reflection_size},
        {rbtrend_exposure_histogram_comp_spirv, sizeof(rbtrend_exposure_histogram_comp_spirv) / 4, CRGPU_SHADER_STAGE_COMPUTE,
         rbtrend_exposure_histogram_comp_reflection, rbtrend_exposure_histogram_comp_reflection_size},
        {rbtrend_exposure_adapt_histogram_frag_spirv, sizeof(rbtrend_exposure_adapt_histogram_frag_spirv) / 4, CRGPU_SHADER_STAGE_FRAGMENT,
         rbtrend_exposure_adapt_histogram_frag_reflection, rbtrend_exposure_adapt_histogram_frag_reflection_size},
    };
    for (const shader &sh : shaders)
    {
        rbtrend_reflection_store store;
        std::string error;
        std::vector<uint8_t> table;
        CHECK(rbtrend_reflect_spirv(sh.spirv, sh.words, sh.stage, &store, &error));
        CHECK(rbtrend_reflect_table_write(store, &table, &error));
        CHECK(table.size() == sh.table_size && memcmp(table.data(), sh.table, table.size()) == 0);
    }
}

// A vertex shader whose SPIR-V drops inputs it declares (card A51):
// rbtrend_lit_shadow_vert reads neither its normal (location 1) nor its
// tangent (3). Its SPIR-V's reflection lacks both; completed from Slang's
// reflection JSON it lists them, not used. The table the build wrote is the
// completed one where the build's packages carry DXIL, and the SPIR-V's
// otherwise, byte for byte. Then the JSON's refusals, and the JSON cut at
// every length, in a buffer of exactly that length.
static std::string shadow_json(const std::string &fields)
{
    return "{\"entryPoints\": [{\"name\": \"rbtrend_lit_shadow_vert\", \"stage\": \"vertex\", \"parameters\": [{\"name\": \"input\", "
           "\"binding\": {\"kind\": \"varyingInput\", \"index\": 0, \"count\": 8}, \"type\": {\"kind\": \"struct\", \"fields\": [" +
           fields + "]}}]}]}";
}

static std::string shadow_field(const char *name, int location, const char *type)
{
    char text[512];
    snprintf(text, sizeof(text), "{\"name\": \"%s\", \"type\": %s, \"binding\": {\"kind\": \"varyingInput\", \"index\": %d}}", name, type,
             location);
    return text;
}

static void test_declared_inputs()
{
    current_test = "test_declared_inputs";
    const CRGPU_SHADER_STAGE vertex = CRGPU_SHADER_STAGE_VERTEX;
    const char *json = (const char *)rbtrend_lit_shadow_vert_reflection_json;
    const size_t json_size = (size_t)rbtrend_lit_shadow_vert_reflection_json_size;
    const size_t words = sizeof(rbtrend_lit_shadow_vert_spirv) / 4;
    rbtrend_reflection_store store;
    std::string error;
    CHECK(rbtrend_reflect_spirv(rbtrend_lit_shadow_vert_spirv, words, vertex, &store, &error));
    CHECK(store.view.input_count == 6);
    CHECK(!find_input(store, "normal") && !find_input(store, "tangent"));
    std::vector<uint8_t> spirv_table;
    CHECK(rbtrend_reflect_table_write(store, &spirv_table, &error));
    CHECK(rbtrend_reflect_declared_inputs(json, json_size, &store, &error));
    CHECK(store.view.input_count == 8);
    CHECK(store.view.inputs == store.inputs.data());
    for (uint32_t i = 0; i < store.view.input_count; ++i)
    {
        const RBTREND_SHADER_INPUT &input = store.view.inputs[i];
        CHECK(input.location == i && input.location_count == 1 && input.scalar_type == RBTREND_SHADER_SCALAR_TYPE_FLOAT);
        CHECK(input.used == (i != 1 && i != 3));
    }
    const RBTREND_SHADER_INPUT *normal = find_input(store, "normal");
    const RBTREND_SHADER_INPUT *tangent = find_input(store, "tangent");
    CHECK(normal && normal->location == 1 && normal->components == 3);
    CHECK(tangent && tangent->location == 3 && tangent->components == 4);
    std::vector<uint8_t> table;
    CHECK(rbtrend_reflect_table_write(store, &table, &error));
    const std::vector<uint8_t> &built = RBTREND_TEST_PACKAGES_DXIL ? table : spirv_table;
    CHECK(built.size() == rbtrend_lit_shadow_vert_reflection_size &&
          memcmp(built.data(), rbtrend_lit_shadow_vert_reflection, built.size()) == 0);
    rbtrend_reflection_store read;
    CHECK(rbtrend_reflect_table_read(table.data(), table.size(), vertex, "rbtrend_lit_shadow_vert", &read, &error) == RBT_RESULT_OK);
    check_same(store, read);

    // Another stage's reflection is left as it is.
    rbtrend_reflection_store fragment;
    CHECK(rbtrend_reflect_spirv(rbtrend_lit_frag_spirv, sizeof(rbtrend_lit_frag_spirv) / 4, CRGPU_SHADER_STAGE_FRAGMENT, &fragment, &error));
    CHECK(rbtrend_reflect_declared_inputs("not json", 8, &fragment, &error));
    CHECK(fragment.view.input_count == 0);

    // [text] completing the shader's SPIR-V reflection: true if accepted,
    // with [inputs] its input count; a refusal says why.
    auto complete = [&](const std::string &text, uint32_t *inputs) {
        rbtrend_reflection_store s;
        std::string e;
        CHECK(rbtrend_reflect_spirv(rbtrend_lit_shadow_vert_spirv, words, vertex, &s, &e));
        const std::vector<char> bytes(text.begin(), text.end());
        const bool ok = rbtrend_reflect_declared_inputs(bytes.data(), bytes.size(), &s, &e);
        CHECK(ok || !e.empty());
        if (inputs)
            *inputs = s.view.input_count;
        return ok;
    };
    const char *vec2 = "{\"kind\": \"vector\", \"elementCount\": 2, \"elementType\": {\"kind\": \"scalar\", \"scalarType\": \"float32\"}}";
    const char *vec3 = "{\"kind\": \"vector\", \"elementCount\": 3, \"elementType\": {\"kind\": \"scalar\", \"scalarType\": \"float32\"}}";
    const char *vec4 = "{\"kind\": \"vector\", \"elementCount\": 4, \"elementType\": {\"kind\": \"scalar\", \"scalarType\": \"float32\"}}";
    const char *mat = "{\"kind\": \"matrix\", \"rowCount\": 4, \"columnCount\": 4, \"elementType\": {\"kind\": \"scalar\", \"scalarType\": \"float32\"}}";
    const std::string transforms = shadow_field("transform0", 4, vec4) + ", " + shadow_field("transform1", 5, vec4) + ", " +
                                   shadow_field("transform2", 6, vec4) + ", " + shadow_field("transform3", 7, vec4);
    const std::string position = shadow_field("position", 0, vec3);
    const std::string uv = shadow_field("uv", 2, vec2);
    uint32_t inputs = 0;
    CHECK(complete(shadow_json(position + ", " + shadow_field("normal", 1, vec3) + ", " + uv + ", " + shadow_field("tangent", 3, vec4) + ", " +
                               transforms),
                   &inputs));
    CHECK(inputs == 8);
    // Only what the SPIR-V has: nothing added.
    CHECK(complete(shadow_json(position + ", " + uv + ", " + transforms), &inputs));
    CHECK(inputs == 6);
    // A system value is no input.
    CHECK(complete(shadow_json(position + ", " + uv + ", " + transforms +
                               ", {\"name\": \"id\", \"semanticName\": \"SV_VertexID\", \"type\": {\"kind\": \"scalar\", \"scalarType\": "
                               "\"uint32\"}, \"binding\": {\"kind\": \"varyingInput\", \"index\": 9}}"),
                   &inputs));
    CHECK(inputs == 6);
    // Refused: an input the SPIR-V has, not declared, or declared elsewhere
    // or by another name; a dropped matrix; inputs that overlap.
    CHECK(!complete(shadow_json(position + ", " + transforms), nullptr));
    CHECK(!complete(shadow_json(position + ", " + shadow_field("uv", 3, vec2) + ", " + transforms), nullptr));
    CHECK(!complete(shadow_json(position + ", " + shadow_field("texcoord", 2, vec2) + ", " + transforms), nullptr));
    CHECK(!complete(shadow_json(position + ", " + uv + ", " + shadow_field("tangent", 3, mat) + ", " + transforms), nullptr));
    CHECK(!complete(shadow_json(position + ", " + uv + ", " + shadow_field("extra", 7, vec4) + ", " + transforms), nullptr));
    // Refused: no JSON, no entry points, not this entry point, bytes after,
    // nesting past the reader's depth.
    CHECK(!complete("not json", nullptr));
    CHECK(!complete("{}", nullptr));
    CHECK(!complete("{\"entryPoints\": []}", nullptr));
    CHECK(!complete(shadow_json(position + ", " + uv + ", " + transforms) + " }", nullptr));
    CHECK(!complete(std::string(100, '['), nullptr));

    // The real JSON cut at every length: accepted only once all of its
    // value is there.
    const std::string whole(json, json_size);
    const size_t last = whole.find_last_of('}');
    uint32_t wrongly = 0;
    for (size_t length = 0; length < json_size; ++length)
        if (complete(whole.substr(0, length), nullptr) != (length > last))
            ++wrongly;
    CHECK(wrongly == 0);
}

// The reader's refusals (rbtrend_reflect_table_read), each with its code, and
// a table cut at every length, in a buffer of exactly that length, so a
// read past its end is an AddressSanitizer report.
static void test_table_refusals()
{
    current_test = "test_table_refusals";
    const uint8_t *good = (const uint8_t *)rbtrend_lit_frag_reflection;
    const size_t size = (size_t)rbtrend_lit_frag_reflection_size;
    const char *entry = "rbtrend_lit_frag";
    std::vector<uint8_t> bytes(good, good + size);
    rbtrend_reflect_table_header header;
    memcpy(&header, good, sizeof(header));
    auto read = [&](const std::vector<uint8_t> &table, CRGPU_SHADER_STAGE stage, const char *entry_point) {
        rbtrend_reflection_store store;
        std::string error;
        const RBT_RESULT result = rbtrend_reflect_table_read(table.data(), table.size(), stage, entry_point, &store, &error);
        CHECK(result == RBT_RESULT_OK || !error.empty());
        return result;
    };
    CHECK(read(bytes, CRGPU_SHADER_STAGE_FRAGMENT, entry) == RBT_RESULT_OK);
    CHECK(read(bytes, CRGPU_SHADER_STAGE_VERTEX, entry) == RBT_RESULT_CORRUPT_DATA);
    CHECK(read(bytes, CRGPU_SHADER_STAGE_FRAGMENT, "rbtrend_other_frag") == RBT_RESULT_CORRUPT_DATA);
    bytes[0] ^= 1u;
    CHECK(read(bytes, CRGPU_SHADER_STAGE_FRAGMENT, entry) == RBT_RESULT_CORRUPT_DATA);
    bytes.assign(good, good + size);
    bytes[4] += 1u;
    CHECK(read(bytes, CRGPU_SHADER_STAGE_FRAGMENT, entry) == RBT_RESULT_REND_REFLECTION_MISMATCH);
    // Every string unterminated: the string area filled.
    bytes.assign(good, good + size);
    memset(bytes.data() + header.string_offset, 'x', header.string_size);
    CHECK(read(bytes, CRGPU_SHADER_STAGE_FRAGMENT, entry) == RBT_RESULT_CORRUPT_DATA);
    // A resource's members past the members, and its block's first member past its block.
    bytes.assign(good, good + size);
    {
        rbtrend_reflect_table_resource resource;
        memcpy(&resource, good + header.resource_offset, sizeof(resource));
        resource.first_member = header.member_count;
        memcpy(bytes.data() + header.resource_offset, &resource, sizeof(resource));
        CHECK(read(bytes, CRGPU_SHADER_STAGE_FRAGMENT, entry) == RBT_RESULT_CORRUPT_DATA);
    }
    // An array without a size, which only a storage block's last member may
    // be: refused in the middle of a block, and as a uniform block's last
    // (RBTREND_TODO.md item 22).
    {
        rbtrend_reflect_table_resource resource;
        uint32_t found = UINT32_MAX;
        for (uint32_t i = 0; i < header.resource_count && found == UINT32_MAX; ++i)
        {
            memcpy(&resource, good + header.resource_offset + i * sizeof(resource), sizeof(resource));
            if (resource.type == CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER && resource.member_count > 1)
                found = i;
        }
        CHECK(found != UINT32_MAX);
        const uint32_t which[2] = {0, found != UINT32_MAX ? resource.member_count - 1 : 0};
        const char *messages[2] = {"an array without a size isn't its block's last member",
                                   "an array without a size ends a block that isn't a storage buffer's"};
        for (int c = 0; c < 2 && found != UINT32_MAX; ++c)
        {
            bytes.assign(good, good + size);
            rbtrend_reflect_table_member member;
            const size_t at = header.member_offset + (size_t)(resource.first_member + which[c]) * sizeof(member);
            memcpy(&member, good + at, sizeof(member));
            member.array_count = 0;
            memcpy(bytes.data() + at, &member, sizeof(member));
            rbtrend_reflection_store store;
            std::string error;
            CHECK(rbtrend_reflect_table_read(bytes.data(), bytes.size(), CRGPU_SHADER_STAGE_FRAGMENT, entry, &store, &error) == RBT_RESULT_CORRUPT_DATA);
            CHECK(error.find(messages[c]) != std::string::npos);
            if (error.find(messages[c]) == std::string::npos)
                printf("  got \"%s\"\n", error.c_str());
        }
    }
    // Cut at every length: the stated size made the cut's, so the parts'
    // bounds are what refuse it, never a read past the end.
    uint32_t refused = 0;
    for (size_t cut = 0; cut < size; ++cut)
    {
        std::vector<uint8_t> cut_table(good, good + cut);
        if (cut >= sizeof(header))
        {
            const uint32_t stated = (uint32_t)cut;
            memcpy(cut_table.data() + offsetof(rbtrend_reflect_table_header, size), &stated, sizeof(stated));
        }
        refused += read(cut_table, CRGPU_SHADER_STAGE_FRAGMENT, entry) == RBT_RESULT_CORRUPT_DATA;
    }
    CHECK(refused == size);
    printf("rbtrendreflect_tests: a %zu-byte table cut at every length: %u refused, none read past its end\n", size, refused);
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
    test_slang_shaders();
    test_type_sizes();
    test_tool_output();
    test_declared_inputs();
    test_table_refusals();
    if (failures)
    {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("rbtrendreflect_tests: all passed\n");
    return 0;
}
