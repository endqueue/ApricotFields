// Headless tests of Aprend's contract (aprendcontext.h, "Results and
// errors"): what each converted call refuses before it touches a device,
// with its code, its message, and its handle written NULL. Calls are added
// here as their headers are converted (APREND_TODO.md, "The contract
// conversion"); what needs a device is tested in a caller's GPU tests.
//
// Written in C, to prove the headers compile and link as C.

#include "aprend.h"
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

// A device the checks never reach: each refusal below comes before Aprend
// calls SpudGPU, so the handle is only compared with NULL.
#define UNREACHED_DEVICE ((spudgpu_device)(uintptr_t)0x10)

// An error struct holding text, so a call that leaves it alone is seen.
static void dirty(APREND_ERROR *error)
{
    memset(error->message, 'x', sizeof(error->message) - 1);
    error->message[sizeof(error->message) - 1] = '\0';
}

static APREND_INSTANCE_DESC valid_instance_desc(void)
{
    APREND_INSTANCE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APREND_INSTANCE_DESC;
    desc.device = UNREACHED_DEVICE;
    desc.frames_in_flight = 2;
    return desc;
}

// [desc] refused with [result] and [message], the instance NULL.
static void instance_create_refused(const APREND_INSTANCE_DESC *desc, APRESULT result, const char *message)
{
    APREND_ERROR error;
    dirty(&error);
    aprend_instance instance = (aprend_instance)(uintptr_t)0x20;
    CHECK(aprend_instance_create(desc, &instance, &error) == result);
    CHECK(instance == NULL);
    CHECK(strcmp(error.message, message) == 0);
    if (strcmp(error.message, message) != 0)
        printf("  got \"%s\"\n", error.message);
    // The same without an error struct: the code alone.
    instance = (aprend_instance)(uintptr_t)0x20;
    CHECK(aprend_instance_create(desc, &instance, NULL) == result);
    CHECK(instance == NULL);
}

static void test_instance_create(void)
{
    current_test = "instance_create";
    instance_create_refused(NULL, APRESULT_INVALID_ARGUMENT, "desc is NULL");

    APREND_INSTANCE_DESC desc = valid_instance_desc();
    desc.base.type = APRI_STRUCT_TYPE_APREND_TEXTURE2D_DESC;
    instance_create_refused(&desc, APRESULT_STRUCT_WRONG_TYPE, "desc has a base.type other than its own");

    APRI_STRUCT_BASE chained;
    chained.type = APRI_STRUCT_TYPE_APREND_TEXTURE2D_DESC;
    chained.next = NULL;
    desc = valid_instance_desc();
    desc.base.next = &chained;
    instance_create_refused(&desc, APRESULT_STRUCT_UNKNOWN_IN_CHAIN, "desc has a struct chained on, and nothing can be chained onto it in this version");

    desc = valid_instance_desc();
    desc.device = NULL;
    instance_create_refused(&desc, APRESULT_INVALID_ARGUMENT, "device is NULL");

    desc = valid_instance_desc();
    desc.frames_in_flight = 0;
    instance_create_refused(&desc, APRESULT_INVALID_ARGUMENT, "frames_in_flight is 0");

    desc = valid_instance_desc();
    desc.frames_in_flight = APREND_MAX_FRAMES_IN_FLIGHT + 1;
    instance_create_refused(&desc, APRESULT_INVALID_ARGUMENT, "frames_in_flight 5 is above APREND_MAX_FRAMES_IN_FLIGHT (4)");

    desc = valid_instance_desc();
    desc.transient_memory_limit = 1024;
    instance_create_refused(&desc, APRESULT_INVALID_ARGUMENT, "transient_memory_limit is set and transient_chunk_size is 0");

    desc = valid_instance_desc();
    desc.transient_chunk_size = 2048;
    desc.transient_memory_limit = 1024;
    instance_create_refused(&desc, APRESULT_INVALID_ARGUMENT, "transient_memory_limit 1024 is below transient_chunk_size 2048");

    // Last in parameter order: every other argument is right.
    desc = valid_instance_desc();
    APREND_ERROR error;
    dirty(&error);
    CHECK(aprend_instance_create(&desc, NULL, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "out_instance is NULL") == 0);
}

// The calls that take an instance refuse a NULL one, and write what they
// promise to on failure.
static void test_null_instance(void)
{
    current_test = "null_instance";
    APREND_ERROR error;

    dirty(&error);
    CHECK(aprend_instance_wait_idle(NULL, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "instance is NULL") == 0);
    CHECK(aprend_instance_wait_idle(NULL, NULL) == APRESULT_INVALID_ARGUMENT);

    APREND_INSTANCE_DESC desc;
    CHECK(aprend_instance_get_desc(NULL, &desc) == APRESULT_INVALID_ARGUMENT);

    APREND_RENDER_TARGET_POOL_INFO info;
    memset(&info, 0, sizeof(info));
    info.base.type = APRI_STRUCT_TYPE_APREND_RENDER_TARGET_POOL_INFO;
    CHECK(aprend_instance_get_render_target_pool(NULL, &info) == APRESULT_INVALID_ARGUMENT);

    uint32_t trimmed = 7;
    CHECK(aprend_instance_trim_render_targets(NULL, 0, &trimmed) == APRESULT_INVALID_ARGUMENT);
    CHECK(trimmed == 0);
    CHECK(aprend_instance_trim_render_targets(NULL, 0, NULL) == APRESULT_INVALID_ARGUMENT);

    dirty(&error);
    CHECK(aprend_instance_set_render_target_retirement(NULL, 1, 0, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "instance is NULL") == 0);

    uint32_t index = 7;
    dirty(&error);
    CHECK(aprend_instance_next_frame(NULL, UINT64_MAX, &index, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(index == 7);
    CHECK(strcmp(error.message, "instance is NULL") == 0);
    CHECK(aprend_instance_get_frame_index(NULL) == 0);
    aprend_instance_destroy(NULL);
}

// Every image call refuses a NULL handle with its code (and its message,
// where it takes an error struct), and writes NULL to a handle it was to
// make; the getters write nothing.
static void test_images_null(void)
{
    current_test = "images_null";
    APREND_ERROR error;
    uint8_t texels[4] = {0};

    APREND_TEXTURE2D_DESC desc2d;
    memset(&desc2d, 0, sizeof(desc2d));
    desc2d.base.type = APRI_STRUCT_TYPE_APREND_TEXTURE2D_DESC;
    aprend_texture2d texture2d = (aprend_texture2d)(uintptr_t)0x20;
    dirty(&error);
    CHECK(aprend_texture2d_create(NULL, &desc2d, &texture2d, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(texture2d == NULL);
    CHECK(strcmp(error.message, "instance is NULL") == 0);
    CHECK(aprend_texture2d_get_desc(NULL, &desc2d) == APRESULT_INVALID_ARGUMENT);
    CHECK(aprend_texture2d_update(NULL, NULL, 0, 0, 0, 0, 1, 1, texels, 4, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "texture is NULL") == 0);
    CHECK(aprend_texture2d_get_data(NULL, NULL, 0, 0, 0, 0, 1, 1, texels, 4, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(aprend_texture2d_generate_mips(NULL, NULL, 0, SPUDGPU_FILTER_LINEAR, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(aprend_texture2d_resize(NULL, 1, 1, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(aprend_texture2d_get_spudgpu_image(NULL) == NULL);
    aprend_texture2d_destroy(NULL);

    APREND_TEXTURE3D_DESC desc3d;
    memset(&desc3d, 0, sizeof(desc3d));
    desc3d.base.type = APRI_STRUCT_TYPE_APREND_TEXTURE3D_DESC;
    aprend_texture3d texture3d = (aprend_texture3d)(uintptr_t)0x20;
    CHECK(aprend_texture3d_create(NULL, &desc3d, &texture3d, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(texture3d == NULL);
    CHECK(aprend_texture3d_get_desc(NULL, &desc3d) == APRESULT_INVALID_ARGUMENT);
    CHECK(aprend_texture3d_update(NULL, NULL, 0, 0, 0, 0, 1, 1, 1, texels, 4, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(aprend_texture3d_get_data(NULL, NULL, 0, 0, 0, 0, 1, 1, 1, texels, 4, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(aprend_texture3d_resize(NULL, 1, 1, 1, &error) == APRESULT_INVALID_ARGUMENT);
    aprend_texture3d_destroy(NULL);

    APREND_TEXTURE_VIEW_DESC view_desc;
    memset(&view_desc, 0, sizeof(view_desc));
    view_desc.base.type = APRI_STRUCT_TYPE_APREND_TEXTURE_VIEW_DESC;
    aprend_texture_view view = (aprend_texture_view)(uintptr_t)0x20;
    CHECK(aprend_texture_view_create_2d(NULL, &view_desc, &view, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(view == NULL);
    view = (aprend_texture_view)(uintptr_t)0x20;
    CHECK(aprend_texture_view_create_3d(NULL, &view_desc, &view, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(view == NULL);
    CHECK(aprend_texture_view_get_desc(NULL, &view_desc) == APRESULT_INVALID_ARGUMENT);
    CHECK(aprend_texture_view_get_type(NULL) == APREND_TEXTURE_VIEW_TYPE_NONE);
    CHECK(aprend_texture_view_get_texture2d(NULL) == NULL);
    aprend_texture_view_destroy(NULL);

    APREND_SAMPLER_DESC sampler_desc;
    memset(&sampler_desc, 0, sizeof(sampler_desc));
    sampler_desc.base.type = APRI_STRUCT_TYPE_APREND_SAMPLER_DESC;
    aprend_sampler sampler = (aprend_sampler)(uintptr_t)0x20;
    CHECK(aprend_sampler_create(NULL, &sampler_desc, &sampler, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(sampler == NULL);
    CHECK(aprend_sampler_get_desc(NULL, &sampler_desc) == APRESULT_INVALID_ARGUMENT);
    aprend_sampler_destroy(NULL);
}

// The shader and pipeline calls refuse a NULL handle or desc with their
// code and message, and write NULL to the handle they were to make; the
// lookups refuse a NULL layout.
static void test_pipelines_null(void)
{
    current_test = "pipelines_null";
    APREND_ERROR error;
    const uint32_t words[4] = {0};

    aprend_shader shader = (aprend_shader)(uintptr_t)0x20;
    dirty(&error);
    CHECK(aprend_shader_create_spirv(NULL, words, sizeof(words), SPUDGPU_SHADER_STAGE_VERTEX, &shader, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(shader == NULL);
    CHECK(strcmp(error.message, "instance is NULL") == 0);
    shader = (aprend_shader)(uintptr_t)0x20;
    CHECK(aprend_shader_read_from_file_spirv(NULL, "x.spv", SPUDGPU_SHADER_STAGE_VERTEX, &shader, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(shader == NULL);
    shader = (aprend_shader)(uintptr_t)0x20;
    CHECK(aprend_get_fullscreen_vertex_shader(NULL, &shader, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(shader == NULL);
    CHECK(aprend_shader_get_reflection(NULL) == NULL);
    CHECK(aprend_shader_find_resource(NULL, "x") == NULL);
    aprend_shader_destroy(NULL);

    APREND_BINDING_DESC binding;
    memset(&binding, 0, sizeof(binding));
    aprend_binding_layout layout = (aprend_binding_layout)(uintptr_t)0x20;
    CHECK(aprend_binding_layout_create(NULL, &binding, 1, &layout, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(layout == NULL);
    layout = (aprend_binding_layout)(uintptr_t)0x20;
    CHECK(aprend_binding_layout_create_from_shaders(NULL, &shader, 1, 0, &layout, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(layout == NULL);
    uint32_t number = 0;
    CHECK(aprend_binding_layout_find(NULL, "x", &number, &binding) == APRESULT_INVALID_ARGUMENT);
    CHECK(aprend_binding_layout_get_binding(NULL, 0, &binding) == APRESULT_INVALID_ARGUMENT);
    CHECK(aprend_binding_layout_get_block(NULL, 0, NULL, NULL, NULL) == APRESULT_INVALID_ARGUMENT);
    CHECK(aprend_binding_layout_get_binding_count(NULL) == 0);
    aprend_binding_layout_destroy(NULL);

    APREND_GRAPHICS_PIPELINE_DESC graphics;
    memset(&graphics, 0, sizeof(graphics));
    graphics.base.type = APRI_STRUCT_TYPE_APREND_GRAPHICS_PIPELINE_DESC;
    aprend_graphics_pipeline graphics_pipeline = (aprend_graphics_pipeline)(uintptr_t)0x20;
    CHECK(aprend_graphics_pipeline_create(NULL, &graphics, &graphics_pipeline, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(graphics_pipeline == NULL);
    CHECK(aprend_graphics_pipeline_get_desc(NULL, &graphics) == APRESULT_INVALID_ARGUMENT);
    aprend_graphics_pipeline_destroy(NULL);

    APREND_COMPUTE_PIPELINE_DESC compute;
    memset(&compute, 0, sizeof(compute));
    compute.base.type = APRI_STRUCT_TYPE_APREND_COMPUTE_PIPELINE_DESC;
    aprend_compute_pipeline compute_pipeline = (aprend_compute_pipeline)(uintptr_t)0x20;
    CHECK(aprend_compute_pipeline_create(NULL, &compute, &compute_pipeline, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(compute_pipeline == NULL);
    CHECK(aprend_compute_pipeline_get_desc(NULL, &compute) == APRESULT_INVALID_ARGUMENT);
    aprend_compute_pipeline_destroy(NULL);

    APREND_BINDING_SET_ENTRY entry;
    memset(&entry, 0, sizeof(entry));
    aprend_binding_set set = (aprend_binding_set)(uintptr_t)0x20;
    CHECK(aprend_binding_set_create(NULL, &entry, 1, &set, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(set == NULL);
    CHECK(strcmp(error.message, "layout is NULL") == 0);
    CHECK(aprend_binding_set_update(NULL, &entry, 1, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "set is NULL") == 0);
    aprend_binding_set_destroy(NULL);
}

// Framebuffers, passes, timers and transient memory refuse a NULL handle
// with their code, writing NULL to a handle they were to make.
static void test_frames_and_passes_null(void)
{
    current_test = "frames_and_passes_null";
    APREND_ERROR error;

    APREND_FRAMEBUFFER_DESC framebuffer_desc;
    memset(&framebuffer_desc, 0, sizeof(framebuffer_desc));
    framebuffer_desc.base.type = APRI_STRUCT_TYPE_APREND_FRAMEBUFFER_DESC;
    aprend_framebuffer framebuffer = (aprend_framebuffer)(uintptr_t)0x20;
    dirty(&error);
    CHECK(aprend_framebuffer_create(NULL, &framebuffer_desc, &framebuffer, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(framebuffer == NULL);
    CHECK(strcmp(error.message, "instance is NULL") == 0);
    CHECK(aprend_framebuffer_get_desc(NULL, &framebuffer_desc) == APRESULT_INVALID_ARGUMENT);
    CHECK(aprend_framebuffer_resize(NULL, 1, 1, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(aprend_framebuffer_clear_colors(NULL, NULL, 0.0f, 0.0f, 0.0f, 1.0f, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(aprend_framebuffer_clear_depth(NULL, NULL, true, false, 1.0f, 0, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(aprend_framebuffer_get_color_attachment(NULL, 0) == NULL);
    CHECK(aprend_framebuffer_get_depth_attachment(NULL) == NULL);
    aprend_framebuffer_destroy(NULL);

    APREND_RENDER_PASS_DESC render_desc;
    memset(&render_desc, 0, sizeof(render_desc));
    render_desc.base.type = APRI_STRUCT_TYPE_APREND_RENDER_PASS_DESC;
    aprend_render_pass render_pass = (aprend_render_pass)(uintptr_t)0x20;
    CHECK(aprend_render_pass_create(NULL, &render_desc, &render_pass, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(render_pass == NULL);
    CHECK(aprend_render_pass_set_binding_set(NULL, 0, NULL, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "pass is NULL") == 0);
    uint32_t width = 7;
    CHECK(aprend_render_pass_get_size(NULL, &width, NULL) == APRESULT_INVALID_ARGUMENT);
    CHECK(width == 7);
    CHECK(aprend_render_pass_get_pipeline(NULL) == NULL);
    aprend_render_pass_destroy(NULL);

    APREND_COMPUTE_PASS_DESC compute_desc;
    memset(&compute_desc, 0, sizeof(compute_desc));
    compute_desc.base.type = APRI_STRUCT_TYPE_APREND_COMPUTE_PASS_DESC;
    aprend_compute_pass compute_pass = (aprend_compute_pass)(uintptr_t)0x20;
    CHECK(aprend_compute_pass_create(NULL, &compute_desc, &compute_pass, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(compute_pass == NULL);
    CHECK(aprend_compute_pass_set_binding_set(NULL, 0, NULL, &error) == APRESULT_INVALID_ARGUMENT);
    aprend_compute_pass_destroy(NULL);

    APREND_GPU_TIMER_DESC timer_desc;
    memset(&timer_desc, 0, sizeof(timer_desc));
    timer_desc.base.type = APRI_STRUCT_TYPE_APREND_GPU_TIMER_DESC;
    aprend_gpu_timer timer = (aprend_gpu_timer)(uintptr_t)0x20;
    CHECK(aprend_gpu_timer_create(NULL, &timer_desc, &timer, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(timer == NULL);
    double ms = -1.0;
    CHECK(aprend_gpu_timer_get_ms(NULL, 0, &ms, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(ms == -1.0);
    aprend_gpu_timer_destroy(NULL);

    APREND_TRANSIENT_SPAN span;
    memset(&span, 0x5a, sizeof(span));
    CHECK(aprend_transient_allocate(NULL, 16, 16, &span, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "instance is NULL") == 0);
    CHECK(span.data == NULL && span.size == 0);
    CHECK(aprend_transient_get_frame_usage(NULL) == 0);
}

// Materials, the lit calls and exposures refuse a NULL handle with their
// code, writing NULL to a handle they were to make.
static void test_materials_and_lit_null(void)
{
    current_test = "materials_and_lit_null";
    APREND_ERROR error;
    const float values[16] = {0};

    APREND_MATERIAL_DESC material_desc;
    memset(&material_desc, 0, sizeof(material_desc));
    material_desc.base.type = APRI_STRUCT_TYPE_APREND_MATERIAL_DESC;
    aprend_material material = (aprend_material)(uintptr_t)0x20;
    dirty(&error);
    CHECK(aprend_material_create(NULL, &material_desc, &material, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(material == NULL);
    CHECK(strcmp(error.message, "instance is NULL") == 0);
    material = (aprend_material)(uintptr_t)0x20;
    CHECK(aprend_material_copy(NULL, &material, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(material == NULL);
    CHECK(aprend_material_set_uniform(NULL, "x", values, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "material is NULL") == 0);
    APREND_MATERIAL_UNIFORM uniform;
    memset(&uniform, 0, sizeof(uniform));
    CHECK(aprend_material_find_uniform(NULL, "x", &uniform, NULL, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(aprend_material_set_uniform_value(NULL, uniform, values, &error) == APRESULT_INVALID_ARGUMENT);
    float out[16];
    CHECK(aprend_material_get_uniform_value(NULL, uniform, out, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(aprend_material_set_uniform_bytes(NULL, "x", 0, 4, values, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(aprend_material_set_uniform_buffer(NULL, "x", 0, NULL, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(aprend_material_set_storage_buffer(NULL, "x", 0, NULL, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(aprend_material_set_texture(NULL, "x", 0, NULL, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(aprend_material_set_sampler(NULL, "x", 0, NULL, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(!aprend_material_is_complete(NULL));
    CHECK(aprend_material_get_layout(NULL) == NULL);
    aprend_material_destroy(NULL);

    aprend_shader shader = (aprend_shader)(uintptr_t)0x20;
    CHECK(aprend_lit_get_shader(NULL, APREND_LIT_SHADER_SURFACE_VERTEX, &shader, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(shader == NULL);
    aprend_binding_layout layout = (aprend_binding_layout)(uintptr_t)0x20;
    CHECK(aprend_lit_get_scene_layout(NULL, &layout, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(layout == NULL);
    CHECK(aprend_lit_get_shadow_layout(NULL, &layout, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(aprend_lit_get_material_layout(NULL, &layout, &error) == APRESULT_INVALID_ARGUMENT);
    material = (aprend_material)(uintptr_t)0x20;
    CHECK(aprend_lit_material_create(NULL, &material, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(material == NULL);
    APREND_LIT_MATERIAL_VALUES material_values;
    memset(&material_values, 0, sizeof(material_values));
    CHECK(aprend_lit_material_set_values(NULL, &material_values, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "material is NULL") == 0);
    CHECK(aprend_lit_material_get_values(NULL, &material_values, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(aprend_lit_material_set_map(NULL, APREND_LIT_MAP_BASE_COLOR, NULL, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(aprend_lit_material_set_sampler(NULL, NULL, &error) == APRESULT_INVALID_ARGUMENT);

    APREND_LIT_ENVIRONMENT_DESC environment_desc;
    memset(&environment_desc, 0, sizeof(environment_desc));
    environment_desc.base.type = APRI_STRUCT_TYPE_APREND_LIT_ENVIRONMENT_DESC;
    aprend_lit_environment environment = (aprend_lit_environment)(uintptr_t)0x20;
    CHECK(aprend_lit_environment_create(NULL, &environment_desc, &environment, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(environment == NULL);
    aprend_lit_environment_destroy(NULL);

    aprend_lit_scene scene = (aprend_lit_scene)(uintptr_t)0x20;
    CHECK(aprend_lit_scene_create(NULL, &scene, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(scene == NULL);
    CHECK(aprend_lit_scene_set_lights(NULL, NULL, 0, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(aprend_lit_scene_set_bent_normals(NULL, true, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(aprend_lit_scene_get_material(NULL) == NULL);
    aprend_lit_scene_destroy(NULL);

    float matrix[16];
    CHECK(aprend_lit_spot_shadow_view_projection(NULL, 0.1f, false, matrix, &error) == APRESULT_INVALID_ARGUMENT);

    APREND_EXPOSURE_DESC exposure_desc;
    memset(&exposure_desc, 0, sizeof(exposure_desc));
    exposure_desc.base.type = APRI_STRUCT_TYPE_APREND_EXPOSURE_DESC;
    aprend_exposure exposure = (aprend_exposure)(uintptr_t)0x20;
    CHECK(aprend_exposure_create(NULL, &exposure_desc, &exposure, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(exposure == NULL);
    aprend_exposure_destroy(NULL);
}

// Draw lists and swap chains refuse a NULL handle with their code, writing
// NULL to a handle they were to make.
static void test_draw_lists_and_swap_chains_null(void)
{
    current_test = "draw_lists_and_swap_chains_null";
    APREND_ERROR error;

    APREND_DRAW_LIST_DESC list_desc;
    memset(&list_desc, 0, sizeof(list_desc));
    list_desc.base.type = APRI_STRUCT_TYPE_APREND_DRAW_LIST_DESC;
    aprend_draw_list list = (aprend_draw_list)(uintptr_t)0x20;
    dirty(&error);
    CHECK(aprend_draw_list_create(NULL, &list_desc, &list, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(list == NULL);
    CHECK(strcmp(error.message, "instance is NULL") == 0);
    APREND_DRAW_MESH mesh;
    memset(&mesh, 0, sizeof(mesh));
    CHECK(aprend_draw_list_add(NULL, &mesh, NULL, 1, NULL, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "list is NULL") == 0);
    CHECK(aprend_draw_list_build(NULL, &error) == APRESULT_INVALID_ARGUMENT);
    uint32_t draws = 7;
    CHECK(aprend_draw_list_get_counts(NULL, &draws, NULL, NULL) == APRESULT_INVALID_ARGUMENT);
    CHECK(draws == 7);
    aprend_draw_list_reset(NULL);
    aprend_draw_list_destroy(NULL);

    APREND_SWAP_CHAIN_DESC swap_desc;
    memset(&swap_desc, 0, sizeof(swap_desc));
    swap_desc.base.type = APRI_STRUCT_TYPE_APREND_SWAP_CHAIN_DESC;
    aprend_swap_chain swap_chain = (aprend_swap_chain)(uintptr_t)0x20;
    CHECK(aprend_swap_chain_create(NULL, &swap_desc, &swap_chain, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(swap_chain == NULL);
    CHECK(aprend_swap_chain_resize(NULL, 1, 1, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(aprend_swap_chain_get_desc(NULL, &swap_desc) == APRESULT_INVALID_ARGUMENT);
    CHECK(aprend_swap_chain_acquire(NULL, &error) == APRESULT_INVALID_ARGUMENT);
    uint32_t width = 0;
    uint32_t height = 0;
    CHECK(aprend_swap_chain_get_back_buffer_size(NULL, &width, &height) == APRESULT_INVALID_ARGUMENT);
    CHECK(aprend_swap_chain_present(NULL, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "swap_chain is NULL") == 0);
    aprend_swap_chain_destroy(NULL);
}

// Command lists refuse a NULL handle with their code.
static void test_command_lists_null(void)
{
    current_test = "command_lists_null";
    APREND_ERROR error;
    aprend_command_list list = (aprend_command_list)(uintptr_t)0x20;
    dirty(&error);
    CHECK(aprend_command_list_create(NULL, &list, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(list == NULL);
    CHECK(strcmp(error.message, "instance is NULL") == 0);
    CHECK(aprend_command_list_compile(NULL, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "cmd_list is NULL") == 0);
    CHECK(aprend_command_list_submit(NULL, NULL, &error) == APRESULT_INVALID_ARGUMENT);
    APREND_COMMAND command;
    memset(&command, 0, sizeof(command));
    aprend_command_list_send(NULL, command);
    aprend_command_list_reset(NULL);
    aprend_command_list_destroy(NULL);
}

// aprendbuffers.h: every call refuses a NULL handle, writing its out_
// parameter NULL, and the uniform layout calls, which take no device, work
// on their own.
static void test_buffers(void)
{
    current_test = "buffers";
    APREND_ERROR error;

    APREND_BUFFER_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APREND_BUFFER_DESC;
    desc.size = 64;
    desc.usage = APREND_BUFFER_USAGE_VERTEX;
    desc.writes = APREND_BUFFER_WRITES_ONCE;
    aprend_buffer buffer = (aprend_buffer)(uintptr_t)0x20;
    dirty(&error);
    CHECK(aprend_buffer_create(NULL, &desc, &buffer, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(buffer == NULL);
    CHECK(strcmp(error.message, "instance is NULL") == 0);
    aprend_buffer_destroy(NULL);
    CHECK(aprend_buffer_get_desc(NULL, &desc) == APRESULT_INVALID_ARGUMENT);
    const uint8_t bytes[4] = {0};
    dirty(&error);
    CHECK(aprend_buffer_write(NULL, 0, bytes, sizeof(bytes), &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "buffer is NULL") == 0);
    CHECK(aprend_buffer_write(NULL, 0, bytes, sizeof(bytes), NULL) == APRESULT_INVALID_ARGUMENT);

    // A block of a vec4, a float, a mat3 and an array of two vec2s, as
    // std140 lays them out: 16, 4, 48 (three columns of 16) and 32 (16 an
    // element).
    APREND_UNIFORM uniforms[4];
    memset(uniforms, 0, sizeof(uniforms));
    uniforms[0].name = "color";
    uniforms[0].type = APREND_UNIFORM_TYPE_VEC4;
    uniforms[0].offset = 0;
    uniforms[1].name = "strength";
    uniforms[1].type = APREND_UNIFORM_TYPE_FLOAT;
    uniforms[1].offset = 16;
    uniforms[2].name = "basis";
    uniforms[2].type = APREND_UNIFORM_TYPE_MAT3;
    uniforms[2].offset = 32;
    uniforms[3].name = "points";
    uniforms[3].type = APREND_UNIFORM_TYPE_VEC2;
    uniforms[3].array_count = 2;
    uniforms[3].offset = 80;
    APREND_UNIFORM_LAYOUT layout;
    layout.count = 4;
    layout.uniforms = uniforms;
    layout.size = 0;

    uint32_t index = 7;
    CHECK(aprend_uniform_layout_get_uniform_index(&layout, "basis", &index) == APRESULT_OK);
    CHECK(index == 2);
    CHECK(aprend_uniform_layout_get_uniform_index(&layout, "nothing", &index) == APRESULT_NOT_FOUND);
    CHECK(index == UINT32_MAX);
    CHECK(aprend_uniform_layout_get_uniform_index(NULL, "color", &index) == APRESULT_INVALID_ARGUMENT);
    CHECK(aprend_uniform_layout_get_uniform_index(&layout, NULL, &index) == APRESULT_INVALID_ARGUMENT);
    CHECK(aprend_uniform_layout_get_uniform_index(&layout, "color", NULL) == APRESULT_INVALID_ARGUMENT);

    // The points end at 80 + 32 = 112, a multiple of 16 already.
    uint32_t size = 1;
    CHECK(aprend_uniform_layout_get_size(&layout, &size, &error) == APRESULT_OK);
    CHECK(size == 112);
    CHECK(error.message[0] == '\0');
    // A declared size past the furthest uniform wins, rounded up to 16.
    layout.size = 120;
    CHECK(aprend_uniform_layout_get_size(&layout, &size, NULL) == APRESULT_OK);
    CHECK(size == 128);
    layout.size = 0;
    uniforms[1].type = 99;
    size = 1;
    CHECK(aprend_uniform_layout_get_size(&layout, &size, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(size == 0);
    CHECK(strcmp(error.message, "uniform 1 has type 99, which is not an APREND_UNIFORM_TYPE") == 0);
    uniforms[1].type = APREND_UNIFORM_TYPE_FLOAT;
    APREND_UNIFORM_LAYOUT empty;
    memset(&empty, 0, sizeof(empty));
    CHECK(aprend_uniform_layout_get_size(&empty, &size, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "the layout has no uniforms and a size of 0") == 0);
    // Only the declared size: a block of members with no uniform type.
    empty.size = 40;
    CHECK(aprend_uniform_layout_get_size(&empty, &size, NULL) == APRESULT_OK);
    CHECK(size == 48);
    APREND_UNIFORM_LAYOUT far;
    far.count = 1;
    far.uniforms = uniforms;
    far.size = UINT32_MAX;
    CHECK(aprend_uniform_layout_get_size(&far, &size, &error) == APRESULT_LIMIT);
    CHECK(aprend_uniform_layout_get_size(NULL, &size, NULL) == APRESULT_INVALID_ARGUMENT);
    CHECK(aprend_uniform_layout_get_size(&layout, NULL, NULL) == APRESULT_INVALID_ARGUMENT);
    layout.uniforms = NULL;
    CHECK(aprend_uniform_layout_get_size(&layout, &size, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "layout's uniforms is NULL with a count of 4") == 0);
    layout.uniforms = uniforms;

    const float color[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    dirty(&error);
    CHECK(aprend_buffer_write_uniform(NULL, &layout, 0, color, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "buffer is NULL") == 0);
    CHECK(aprend_uniform_type_get_size(APREND_UNIFORM_TYPE_MAT3) == 36);
    CHECK(aprend_uniform_type_get_size(99) == 0);

    // The pools.
    APREND_VERTEX_POOL_DESC vertex_desc;
    memset(&vertex_desc, 0, sizeof(vertex_desc));
    vertex_desc.base.type = APRI_STRUCT_TYPE_APREND_VERTEX_POOL_DESC;
    vertex_desc.vertex_stride = 12;
    vertex_desc.block_vertex_count = 1024;
    aprend_vertex_pool vertex_pool = (aprend_vertex_pool)(uintptr_t)0x20;
    dirty(&error);
    CHECK(aprend_vertex_pool_create(NULL, &vertex_desc, &vertex_pool, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(vertex_pool == NULL);
    CHECK(strcmp(error.message, "instance is NULL") == 0);
    APREND_VERTEX_RANGE vertex_range;
    memset(&vertex_range, 0xFF, sizeof(vertex_range));
    CHECK(aprend_vertex_pool_allocate(NULL, 3, bytes, &vertex_range, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(vertex_range.buffer == NULL);
    CHECK(vertex_range.vertex_count == 0);
    CHECK(strcmp(error.message, "pool is NULL") == 0);
    CHECK(aprend_vertex_pool_free(NULL, &vertex_range, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(aprend_vertex_pool_get_usage(NULL, NULL, NULL) == APRESULT_INVALID_ARGUMENT);
    aprend_vertex_pool_trim(NULL);
    aprend_vertex_pool_destroy(NULL);

    APREND_INDEX_POOL_DESC index_desc;
    memset(&index_desc, 0, sizeof(index_desc));
    index_desc.base.type = APRI_STRUCT_TYPE_APREND_INDEX_POOL_DESC;
    index_desc.stride = APREND_INDEX_STRIDE_UINT32;
    index_desc.block_index_count = 1024;
    aprend_index_pool index_pool = (aprend_index_pool)(uintptr_t)0x20;
    CHECK(aprend_index_pool_create(NULL, &index_desc, &index_pool, NULL) == APRESULT_INVALID_ARGUMENT);
    CHECK(index_pool == NULL);
    APREND_INDEX_RANGE index_range;
    memset(&index_range, 0xFF, sizeof(index_range));
    CHECK(aprend_index_pool_allocate(NULL, 3, bytes, &index_range, NULL) == APRESULT_INVALID_ARGUMENT);
    CHECK(index_range.buffer == NULL);
    CHECK(aprend_index_pool_free(NULL, &index_range, NULL) == APRESULT_INVALID_ARGUMENT);
    CHECK(aprend_index_pool_get_usage(NULL, NULL, NULL) == APRESULT_INVALID_ARGUMENT);
    aprend_index_pool_trim(NULL);
    aprend_index_pool_destroy(NULL);
}

int main(void)
{
    test_instance_create();
    test_null_instance();
    test_images_null();
    test_pipelines_null();
    test_frames_and_passes_null();
    test_materials_and_lit_null();
    test_draw_lists_and_swap_chains_null();
    test_command_lists_null();
    test_buffers();
    if (failures)
    {
        printf("aprend_contract_tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("aprend_contract_tests: all passed\n");
    return 0;
}
