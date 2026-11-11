// Headless tests of Rend's contract (rbtrendcontext.h, "Results and
// errors"): what each converted call refuses before it touches a device,
// with its code, its message, and its handle written NULL. Calls are added
// here as their headers are converted (RBTREND_TODO.md, "The contract
// conversion"); what needs a device is tested in a caller's GPU tests.
//
// Written in C, to prove the headers compile and link as C.

#include "rbtrend.h"
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

// A device the checks never reach: each refusal below comes before Rend
// calls Carrottus GPU, so the handle is only compared with NULL.
#define UNREACHED_DEVICE ((crgpu_device)(uintptr_t)0x10)

// An error struct holding text, so a call that leaves it alone is seen.
static void dirty(RBTREND_ERROR *error)
{
    memset(error->message, 'x', sizeof(error->message) - 1);
    error->message[sizeof(error->message) - 1] = '\0';
}

static RBTREND_INSTANCE_DESC valid_instance_desc(void)
{
    RBTREND_INSTANCE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_REND_INSTANCE_DESC;
    desc.device = UNREACHED_DEVICE;
    desc.frames_in_flight = 2;
    return desc;
}

// [desc] refused with [result] and [message], the instance NULL.
static void instance_create_refused(const RBTREND_INSTANCE_DESC *desc, RBT_RESULT result, const char *message)
{
    RBTREND_ERROR error;
    dirty(&error);
    rbtrend_instance instance = (rbtrend_instance)(uintptr_t)0x20;
    CHECK(rbtrend_instance_create(desc, &instance, &error) == result);
    CHECK(instance == NULL);
    CHECK(strcmp(error.message, message) == 0);
    if (strcmp(error.message, message) != 0)
        printf("  got \"%s\"\n", error.message);
    // The same without an error struct: the code alone.
    instance = (rbtrend_instance)(uintptr_t)0x20;
    CHECK(rbtrend_instance_create(desc, &instance, NULL) == result);
    CHECK(instance == NULL);
}

static void test_instance_create(void)
{
    current_test = "instance_create";
    instance_create_refused(NULL, RBT_RESULT_INVALID_ARGUMENT, "desc is NULL");

    RBTREND_INSTANCE_DESC desc = valid_instance_desc();
    desc.base.type = RBT_STRUCT_TYPE_REND_TEXTURE2D_DESC;
    instance_create_refused(&desc, RBT_RESULT_STRUCT_WRONG_TYPE, "desc has a base.type other than its own");

    RBT_STRUCT_BASE chained;
    chained.type = RBT_STRUCT_TYPE_REND_TEXTURE2D_DESC;
    chained.next = NULL;
    desc = valid_instance_desc();
    desc.base.next = &chained;
    instance_create_refused(&desc, RBT_RESULT_STRUCT_UNKNOWN_IN_CHAIN, "desc has a struct chained on, and nothing can be chained onto it in this version");

    desc = valid_instance_desc();
    desc.device = NULL;
    instance_create_refused(&desc, RBT_RESULT_INVALID_ARGUMENT, "device is NULL");

    desc = valid_instance_desc();
    desc.frames_in_flight = 0;
    instance_create_refused(&desc, RBT_RESULT_INVALID_ARGUMENT, "frames_in_flight is 0");

    desc = valid_instance_desc();
    desc.frames_in_flight = RBTREND_MAX_FRAMES_IN_FLIGHT + 1;
    instance_create_refused(&desc, RBT_RESULT_INVALID_ARGUMENT, "frames_in_flight 5 is above RBTREND_MAX_FRAMES_IN_FLIGHT (4)");

    desc = valid_instance_desc();
    desc.transient_memory_limit = 1024;
    instance_create_refused(&desc, RBT_RESULT_INVALID_ARGUMENT, "transient_memory_limit is set and transient_chunk_size is 0");

    desc = valid_instance_desc();
    desc.transient_chunk_size = 2048;
    desc.transient_memory_limit = 1024;
    instance_create_refused(&desc, RBT_RESULT_INVALID_ARGUMENT, "transient_memory_limit 1024 is below transient_chunk_size 2048");

    // Last in parameter order: every other argument is right.
    desc = valid_instance_desc();
    RBTREND_ERROR error;
    dirty(&error);
    CHECK(rbtrend_instance_create(&desc, NULL, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "out_instance is NULL") == 0);
}

// The calls that take an instance refuse a NULL one, and write what they
// promise to on failure.
static void test_null_instance(void)
{
    current_test = "null_instance";
    RBTREND_ERROR error;

    dirty(&error);
    CHECK(rbtrend_instance_wait_idle(NULL, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "instance is NULL") == 0);
    CHECK(rbtrend_instance_wait_idle(NULL, NULL) == RBT_RESULT_INVALID_ARGUMENT);

    RBTREND_INSTANCE_DESC desc;
    CHECK(rbtrend_instance_get_desc(NULL, &desc) == RBT_RESULT_INVALID_ARGUMENT);

    RBTREND_RENDER_TARGET_POOL_INFO info;
    memset(&info, 0, sizeof(info));
    info.base.type = RBT_STRUCT_TYPE_REND_RENDER_TARGET_POOL_INFO;
    CHECK(rbtrend_instance_get_render_target_pool(NULL, &info) == RBT_RESULT_INVALID_ARGUMENT);

    uint32_t trimmed = 7;
    CHECK(rbtrend_instance_trim_render_targets(NULL, 0, &trimmed) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(trimmed == 0);
    CHECK(rbtrend_instance_trim_render_targets(NULL, 0, NULL) == RBT_RESULT_INVALID_ARGUMENT);

    dirty(&error);
    CHECK(rbtrend_instance_set_render_target_retirement(NULL, 1, 0, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "instance is NULL") == 0);

    uint32_t index = 7;
    dirty(&error);
    CHECK(rbtrend_instance_next_frame(NULL, UINT64_MAX, &index, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(index == 7);
    CHECK(strcmp(error.message, "instance is NULL") == 0);
    CHECK(rbtrend_instance_get_frame_index(NULL) == 0);
    rbtrend_instance_destroy(NULL);
}

// Every image call refuses a NULL handle with its code (and its message,
// where it takes an error struct), and writes NULL to a handle it was to
// make; the getters write nothing.
static void test_images_null(void)
{
    current_test = "images_null";
    RBTREND_ERROR error;
    uint8_t texels[4] = {0};

    RBTREND_TEXTURE2D_DESC desc2d;
    memset(&desc2d, 0, sizeof(desc2d));
    desc2d.base.type = RBT_STRUCT_TYPE_REND_TEXTURE2D_DESC;
    rbtrend_texture2d texture2d = (rbtrend_texture2d)(uintptr_t)0x20;
    dirty(&error);
    CHECK(rbtrend_texture2d_create(NULL, &desc2d, &texture2d, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(texture2d == NULL);
    CHECK(strcmp(error.message, "instance is NULL") == 0);
    CHECK(rbtrend_texture2d_get_desc(NULL, &desc2d) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtrend_texture2d_update(NULL, NULL, 0, 0, 0, 0, 1, 1, texels, 4, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "texture is NULL") == 0);
    CHECK(rbtrend_texture2d_get_data(NULL, NULL, 0, 0, 0, 0, 1, 1, texels, 4, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtrend_texture2d_generate_mips(NULL, NULL, 0, CRGPU_FILTER_LINEAR, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtrend_texture2d_resize(NULL, 1, 1, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtrend_texture2d_get_crgpu_image(NULL) == NULL);
    rbtrend_texture2d_destroy(NULL);

    RBTREND_TEXTURE3D_DESC desc3d;
    memset(&desc3d, 0, sizeof(desc3d));
    desc3d.base.type = RBT_STRUCT_TYPE_REND_TEXTURE3D_DESC;
    rbtrend_texture3d texture3d = (rbtrend_texture3d)(uintptr_t)0x20;
    CHECK(rbtrend_texture3d_create(NULL, &desc3d, &texture3d, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(texture3d == NULL);
    CHECK(rbtrend_texture3d_get_desc(NULL, &desc3d) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtrend_texture3d_update(NULL, NULL, 0, 0, 0, 0, 1, 1, 1, texels, 4, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtrend_texture3d_get_data(NULL, NULL, 0, 0, 0, 0, 1, 1, 1, texels, 4, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtrend_texture3d_resize(NULL, 1, 1, 1, &error) == RBT_RESULT_INVALID_ARGUMENT);
    rbtrend_texture3d_destroy(NULL);

    RBTREND_TEXTURE_VIEW_DESC view_desc;
    memset(&view_desc, 0, sizeof(view_desc));
    view_desc.base.type = RBT_STRUCT_TYPE_REND_TEXTURE_VIEW_DESC;
    rbtrend_texture_view view = (rbtrend_texture_view)(uintptr_t)0x20;
    CHECK(rbtrend_texture_view_create_2d(NULL, &view_desc, &view, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(view == NULL);
    view = (rbtrend_texture_view)(uintptr_t)0x20;
    CHECK(rbtrend_texture_view_create_3d(NULL, &view_desc, &view, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(view == NULL);
    CHECK(rbtrend_texture_view_get_desc(NULL, &view_desc) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtrend_texture_view_get_type(NULL) == RBTREND_TEXTURE_VIEW_TYPE_NONE);
    CHECK(rbtrend_texture_view_get_texture2d(NULL) == NULL);
    rbtrend_texture_view_destroy(NULL);

    RBTREND_SAMPLER_DESC sampler_desc;
    memset(&sampler_desc, 0, sizeof(sampler_desc));
    sampler_desc.base.type = RBT_STRUCT_TYPE_REND_SAMPLER_DESC;
    rbtrend_sampler sampler = (rbtrend_sampler)(uintptr_t)0x20;
    CHECK(rbtrend_sampler_create(NULL, &sampler_desc, &sampler, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(sampler == NULL);
    CHECK(rbtrend_sampler_get_desc(NULL, &sampler_desc) == RBT_RESULT_INVALID_ARGUMENT);
    rbtrend_sampler_destroy(NULL);
}

// The shader and pipeline calls refuse a NULL handle or desc with their
// code and message, and write NULL to the handle they were to make; the
// lookups refuse a NULL layout.
static void test_pipelines_null(void)
{
    current_test = "pipelines_null";
    RBTREND_ERROR error;
    const uint32_t words[4] = {0};

    // rbtrend_shader_create's first refusal, the only one before a device:
    // the rest, in the header's order, are ../apricottests'
    // test_shader_create_refusals.
    RBTREND_SHADER_PACKAGE package;
    memset(&package, 0, sizeof(package));
    package.base.type = RBT_STRUCT_TYPE_REND_SHADER_PACKAGE;
    package.stage = CRGPU_SHADER_STAGE_VERTEX;
    package.entry_point = "main_vert";
    package.spirv = words;
    package.spirv_size = sizeof(words);
    rbtrend_shader shader = (rbtrend_shader)(uintptr_t)0x20;
    dirty(&error);
    CHECK(rbtrend_shader_create(NULL, &package, &shader, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(shader == NULL);
    CHECK(strcmp(error.message, "instance is NULL") == 0);
    shader = (rbtrend_shader)(uintptr_t)0x20;
    CHECK(rbtrend_shader_create(NULL, NULL, &shader, NULL) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(shader == NULL);
    shader = (rbtrend_shader)(uintptr_t)0x20;
    CHECK(rbtrend_instance_get_fullscreen_vertex_shader(NULL, &shader, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(shader == NULL);
    CHECK(rbtrend_shader_get_reflection(NULL) == NULL);
    CHECK(rbtrend_shader_find_resource(NULL, "x") == NULL);
    rbtrend_shader_destroy(NULL);

    RBTREND_BINDING_DESC binding;
    memset(&binding, 0, sizeof(binding));
    rbtrend_binding_layout layout = (rbtrend_binding_layout)(uintptr_t)0x20;
    CHECK(rbtrend_binding_layout_create(NULL, &binding, 1, &layout, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(layout == NULL);
    layout = (rbtrend_binding_layout)(uintptr_t)0x20;
    CHECK(rbtrend_binding_layout_create_from_shaders(NULL, &shader, 1, 0, &layout, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(layout == NULL);
    uint32_t number = 0;
    CHECK(rbtrend_binding_layout_find(NULL, "x", &number, &binding) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtrend_binding_layout_get_binding(NULL, 0, &binding) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtrend_binding_layout_get_block(NULL, 0, NULL, NULL, NULL) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtrend_binding_layout_get_binding_count(NULL) == 0);
    rbtrend_binding_layout_destroy(NULL);

    RBTREND_GRAPHICS_PIPELINE_DESC graphics;
    memset(&graphics, 0, sizeof(graphics));
    graphics.base.type = RBT_STRUCT_TYPE_REND_GRAPHICS_PIPELINE_DESC;
    rbtrend_graphics_pipeline graphics_pipeline = (rbtrend_graphics_pipeline)(uintptr_t)0x20;
    CHECK(rbtrend_graphics_pipeline_create(NULL, &graphics, &graphics_pipeline, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(graphics_pipeline == NULL);
    CHECK(rbtrend_graphics_pipeline_get_desc(NULL, &graphics) == RBT_RESULT_INVALID_ARGUMENT);
    rbtrend_graphics_pipeline_destroy(NULL);

    RBTREND_COMPUTE_PIPELINE_DESC compute;
    memset(&compute, 0, sizeof(compute));
    compute.base.type = RBT_STRUCT_TYPE_REND_COMPUTE_PIPELINE_DESC;
    rbtrend_compute_pipeline compute_pipeline = (rbtrend_compute_pipeline)(uintptr_t)0x20;
    CHECK(rbtrend_compute_pipeline_create(NULL, &compute, &compute_pipeline, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(compute_pipeline == NULL);
    CHECK(rbtrend_compute_pipeline_get_desc(NULL, &compute) == RBT_RESULT_INVALID_ARGUMENT);
    rbtrend_compute_pipeline_destroy(NULL);

    RBTREND_BINDING_SET_ENTRY entry;
    memset(&entry, 0, sizeof(entry));
    rbtrend_binding_set set = (rbtrend_binding_set)(uintptr_t)0x20;
    CHECK(rbtrend_binding_set_create(NULL, &entry, 1, &set, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(set == NULL);
    CHECK(strcmp(error.message, "layout is NULL") == 0);
    CHECK(rbtrend_binding_set_update(NULL, &entry, 1, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "set is NULL") == 0);
    rbtrend_binding_set_destroy(NULL);
}

// Framebuffers, passes, timers and transient memory refuse a NULL handle
// with their code, writing NULL to a handle they were to make.
static void test_frames_and_passes_null(void)
{
    current_test = "frames_and_passes_null";
    RBTREND_ERROR error;

    RBTREND_FRAMEBUFFER_DESC framebuffer_desc;
    memset(&framebuffer_desc, 0, sizeof(framebuffer_desc));
    framebuffer_desc.base.type = RBT_STRUCT_TYPE_REND_FRAMEBUFFER_DESC;
    rbtrend_framebuffer framebuffer = (rbtrend_framebuffer)(uintptr_t)0x20;
    dirty(&error);
    CHECK(rbtrend_framebuffer_create(NULL, &framebuffer_desc, &framebuffer, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(framebuffer == NULL);
    CHECK(strcmp(error.message, "instance is NULL") == 0);
    CHECK(rbtrend_framebuffer_get_desc(NULL, &framebuffer_desc) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtrend_framebuffer_resize(NULL, 1, 1, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtrend_framebuffer_clear_colors(NULL, NULL, 0.0f, 0.0f, 0.0f, 1.0f, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtrend_framebuffer_clear_depth(NULL, NULL, true, false, 1.0f, 0, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtrend_framebuffer_get_color_attachment(NULL, 0) == NULL);
    CHECK(rbtrend_framebuffer_get_depth_attachment(NULL) == NULL);
    rbtrend_framebuffer_destroy(NULL);

    RBTREND_RENDER_PASS_DESC render_desc;
    memset(&render_desc, 0, sizeof(render_desc));
    render_desc.base.type = RBT_STRUCT_TYPE_REND_RENDER_PASS_DESC;
    rbtrend_render_pass render_pass = (rbtrend_render_pass)(uintptr_t)0x20;
    CHECK(rbtrend_render_pass_create(NULL, &render_desc, &render_pass, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(render_pass == NULL);
    CHECK(rbtrend_render_pass_set_binding_set(NULL, 0, NULL, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "pass is NULL") == 0);
    uint32_t width = 7;
    CHECK(rbtrend_render_pass_get_size(NULL, &width, NULL) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(width == 7);
    CHECK(rbtrend_render_pass_get_pipeline(NULL) == NULL);
    rbtrend_render_pass_destroy(NULL);

    RBTREND_COMPUTE_PASS_DESC compute_desc;
    memset(&compute_desc, 0, sizeof(compute_desc));
    compute_desc.base.type = RBT_STRUCT_TYPE_REND_COMPUTE_PASS_DESC;
    rbtrend_compute_pass compute_pass = (rbtrend_compute_pass)(uintptr_t)0x20;
    CHECK(rbtrend_compute_pass_create(NULL, &compute_desc, &compute_pass, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(compute_pass == NULL);
    CHECK(rbtrend_compute_pass_set_binding_set(NULL, 0, NULL, &error) == RBT_RESULT_INVALID_ARGUMENT);
    rbtrend_compute_pass_destroy(NULL);

    RBTREND_GPU_TIMER_DESC timer_desc;
    memset(&timer_desc, 0, sizeof(timer_desc));
    timer_desc.base.type = RBT_STRUCT_TYPE_REND_GPU_TIMER_DESC;
    rbtrend_gpu_timer timer = (rbtrend_gpu_timer)(uintptr_t)0x20;
    CHECK(rbtrend_gpu_timer_create(NULL, &timer_desc, &timer, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(timer == NULL);
    double ms = -1.0;
    CHECK(rbtrend_gpu_timer_get_ms(NULL, 0, &ms, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(ms == -1.0);
    rbtrend_gpu_timer_destroy(NULL);

    RBTREND_TRANSIENT_SPAN span;
    memset(&span, 0x5a, sizeof(span));
    CHECK(rbtrend_transient_allocate(NULL, 16, 16, &span, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "instance is NULL") == 0);
    CHECK(span.data == NULL && span.size == 0);
    CHECK(rbtrend_transient_get_frame_usage(NULL) == 0);
}

// Materials, the lit calls and exposures refuse a NULL handle with their
// code, writing NULL to a handle they were to make.
static void test_materials_and_lit_null(void)
{
    current_test = "materials_and_lit_null";
    RBTREND_ERROR error;
    const float values[16] = {0};

    RBTREND_MATERIAL_DESC material_desc;
    memset(&material_desc, 0, sizeof(material_desc));
    material_desc.base.type = RBT_STRUCT_TYPE_REND_MATERIAL_DESC;
    rbtrend_material material = (rbtrend_material)(uintptr_t)0x20;
    dirty(&error);
    CHECK(rbtrend_material_create(NULL, &material_desc, &material, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(material == NULL);
    CHECK(strcmp(error.message, "instance is NULL") == 0);
    material = (rbtrend_material)(uintptr_t)0x20;
    CHECK(rbtrend_material_copy(NULL, &material, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(material == NULL);
    CHECK(rbtrend_material_set_uniform(NULL, "x", values, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "material is NULL") == 0);
    RBTREND_MATERIAL_UNIFORM uniform;
    memset(&uniform, 0, sizeof(uniform));
    CHECK(rbtrend_material_find_uniform(NULL, "x", &uniform, NULL, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtrend_material_set_uniform_value(NULL, uniform, values, &error) == RBT_RESULT_INVALID_ARGUMENT);
    float out[16];
    CHECK(rbtrend_material_get_uniform_value(NULL, uniform, out, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtrend_material_set_uniform_bytes(NULL, "x", 0, 4, values, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtrend_material_set_uniform_buffer(NULL, "x", 0, NULL, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtrend_material_set_storage_buffer(NULL, "x", 0, NULL, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtrend_material_set_texture(NULL, "x", 0, NULL, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtrend_material_set_sampler(NULL, "x", 0, NULL, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(!rbtrend_material_is_complete(NULL));
    CHECK(rbtrend_material_get_layout(NULL) == NULL);
    rbtrend_material_destroy(NULL);

    rbtrend_shader shader = (rbtrend_shader)(uintptr_t)0x20;
    CHECK(rbtrend_lit_get_shader(NULL, RBTREND_LIT_SHADER_SURFACE_VERTEX, &shader, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(shader == NULL);
    rbtrend_binding_layout layout = (rbtrend_binding_layout)(uintptr_t)0x20;
    CHECK(rbtrend_lit_get_scene_layout(NULL, &layout, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(layout == NULL);
    CHECK(rbtrend_lit_get_shadow_layout(NULL, &layout, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtrend_lit_get_material_layout(NULL, &layout, &error) == RBT_RESULT_INVALID_ARGUMENT);
    material = (rbtrend_material)(uintptr_t)0x20;
    CHECK(rbtrend_lit_material_create(NULL, &material, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(material == NULL);
    RBTREND_LIT_MATERIAL_VALUES material_values;
    memset(&material_values, 0, sizeof(material_values));
    CHECK(rbtrend_lit_material_set_values(NULL, &material_values, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "material is NULL") == 0);
    CHECK(rbtrend_lit_material_get_values(NULL, &material_values, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtrend_lit_material_set_map(NULL, RBTREND_LIT_MAP_BASE_COLOR, NULL, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtrend_lit_material_set_sampler(NULL, NULL, &error) == RBT_RESULT_INVALID_ARGUMENT);

    RBTREND_LIT_ENVIRONMENT_DESC environment_desc;
    memset(&environment_desc, 0, sizeof(environment_desc));
    environment_desc.base.type = RBT_STRUCT_TYPE_REND_LIT_ENVIRONMENT_DESC;
    rbtrend_lit_environment environment = (rbtrend_lit_environment)(uintptr_t)0x20;
    CHECK(rbtrend_lit_environment_create(NULL, &environment_desc, &environment, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(environment == NULL);
    rbtrend_lit_environment_destroy(NULL);

    rbtrend_lit_scene scene = (rbtrend_lit_scene)(uintptr_t)0x20;
    CHECK(rbtrend_lit_scene_create(NULL, &scene, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(scene == NULL);
    CHECK(rbtrend_lit_scene_set_lights(NULL, NULL, 0, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtrend_lit_scene_set_bent_normals(NULL, true, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtrend_lit_scene_get_material(NULL) == NULL);
    rbtrend_lit_scene_destroy(NULL);

    float matrix[16];
    CHECK(rbtrend_lit_spot_shadow_view_projection(NULL, 0.1f, false, matrix, &error) == RBT_RESULT_INVALID_ARGUMENT);

    RBTREND_EXPOSURE_DESC exposure_desc;
    memset(&exposure_desc, 0, sizeof(exposure_desc));
    exposure_desc.base.type = RBT_STRUCT_TYPE_REND_EXPOSURE_DESC;
    rbtrend_exposure exposure = (rbtrend_exposure)(uintptr_t)0x20;
    CHECK(rbtrend_exposure_create(NULL, &exposure_desc, &exposure, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(exposure == NULL);
    rbtrend_exposure_destroy(NULL);
}

// Draw lists and swap chains refuse a NULL handle with their code, writing
// NULL to a handle they were to make.
static void test_draw_lists_and_swap_chains_null(void)
{
    current_test = "draw_lists_and_swap_chains_null";
    RBTREND_ERROR error;

    RBTREND_DRAW_LIST_DESC list_desc;
    memset(&list_desc, 0, sizeof(list_desc));
    list_desc.base.type = RBT_STRUCT_TYPE_REND_DRAW_LIST_DESC;
    rbtrend_draw_list list = (rbtrend_draw_list)(uintptr_t)0x20;
    dirty(&error);
    CHECK(rbtrend_draw_list_create(NULL, &list_desc, &list, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(list == NULL);
    CHECK(strcmp(error.message, "instance is NULL") == 0);
    RBTREND_DRAW_MESH mesh;
    memset(&mesh, 0, sizeof(mesh));
    CHECK(rbtrend_draw_list_add(NULL, &mesh, NULL, 1, NULL, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "list is NULL") == 0);
    CHECK(rbtrend_draw_list_build(NULL, &error) == RBT_RESULT_INVALID_ARGUMENT);
    uint32_t draws = 7;
    CHECK(rbtrend_draw_list_get_counts(NULL, &draws, NULL, NULL) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(draws == 7);
    rbtrend_draw_list_reset(NULL);
    rbtrend_draw_list_destroy(NULL);

    RBTREND_SWAP_CHAIN_DESC swap_desc;
    memset(&swap_desc, 0, sizeof(swap_desc));
    swap_desc.base.type = RBT_STRUCT_TYPE_REND_SWAP_CHAIN_DESC;
    rbtrend_swap_chain swap_chain = (rbtrend_swap_chain)(uintptr_t)0x20;
    CHECK(rbtrend_swap_chain_create(NULL, &swap_desc, &swap_chain, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(swap_chain == NULL);
    CHECK(rbtrend_swap_chain_resize(NULL, 1, 1, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtrend_swap_chain_get_desc(NULL, &swap_desc) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtrend_swap_chain_acquire(NULL, &error) == RBT_RESULT_INVALID_ARGUMENT);
    uint32_t width = 0;
    uint32_t height = 0;
    CHECK(rbtrend_swap_chain_get_back_buffer_size(NULL, &width, &height) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtrend_swap_chain_present(NULL, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "swap_chain is NULL") == 0);
    rbtrend_swap_chain_destroy(NULL);
}

// Command lists refuse a NULL handle with their code.
static void test_command_lists_null(void)
{
    current_test = "command_lists_null";
    RBTREND_ERROR error;
    rbtrend_command_list list = (rbtrend_command_list)(uintptr_t)0x20;
    dirty(&error);
    CHECK(rbtrend_command_list_create(NULL, &list, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(list == NULL);
    CHECK(strcmp(error.message, "instance is NULL") == 0);
    CHECK(rbtrend_command_list_compile(NULL, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "cmd_list is NULL") == 0);
    CHECK(rbtrend_command_list_submit(NULL, NULL, &error) == RBT_RESULT_INVALID_ARGUMENT);
    RBTREND_COMMAND command;
    memset(&command, 0, sizeof(command));
    rbtrend_command_list_send(NULL, command);
    rbtrend_command_list_reset(NULL);
    rbtrend_command_list_destroy(NULL);
}

// rbtrendbuffers.h: every call refuses a NULL handle, writing its out_
// parameter NULL, and the uniform layout calls, which take no device, work
// on their own.
static void test_buffers(void)
{
    current_test = "buffers";
    RBTREND_ERROR error;

    RBTREND_BUFFER_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_REND_BUFFER_DESC;
    desc.size = 64;
    desc.usage = RBTREND_BUFFER_USAGE_VERTEX;
    desc.writes = RBTREND_BUFFER_WRITES_ONCE;
    rbtrend_buffer buffer = (rbtrend_buffer)(uintptr_t)0x20;
    dirty(&error);
    CHECK(rbtrend_buffer_create(NULL, &desc, &buffer, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(buffer == NULL);
    CHECK(strcmp(error.message, "instance is NULL") == 0);
    rbtrend_buffer_destroy(NULL);
    CHECK(rbtrend_buffer_get_desc(NULL, &desc) == RBT_RESULT_INVALID_ARGUMENT);
    const uint8_t bytes[4] = {0};
    dirty(&error);
    CHECK(rbtrend_buffer_write(NULL, 0, bytes, sizeof(bytes), &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "buffer is NULL") == 0);
    CHECK(rbtrend_buffer_write(NULL, 0, bytes, sizeof(bytes), NULL) == RBT_RESULT_INVALID_ARGUMENT);

    // A block of a vec4, a float, a mat3 and an array of two vec2s, as
    // std140 lays them out: 16, 4, 48 (three columns of 16) and 32 (16 an
    // element).
    RBTREND_UNIFORM uniforms[4];
    memset(uniforms, 0, sizeof(uniforms));
    uniforms[0].name = "color";
    uniforms[0].type = RBTREND_UNIFORM_TYPE_VEC4;
    uniforms[0].offset = 0;
    uniforms[1].name = "strength";
    uniforms[1].type = RBTREND_UNIFORM_TYPE_FLOAT;
    uniforms[1].offset = 16;
    uniforms[2].name = "basis";
    uniforms[2].type = RBTREND_UNIFORM_TYPE_MAT3;
    uniforms[2].offset = 32;
    uniforms[3].name = "points";
    uniforms[3].type = RBTREND_UNIFORM_TYPE_VEC2;
    uniforms[3].array_count = 2;
    uniforms[3].offset = 80;
    RBTREND_UNIFORM_LAYOUT layout;
    layout.count = 4;
    layout.uniforms = uniforms;
    layout.size = 0;

    uint32_t index = 7;
    CHECK(rbtrend_uniform_layout_get_uniform_index(&layout, "basis", &index) == RBT_RESULT_OK);
    CHECK(index == 2);
    CHECK(rbtrend_uniform_layout_get_uniform_index(&layout, "nothing", &index) == RBT_RESULT_NOT_FOUND);
    CHECK(index == UINT32_MAX);
    CHECK(rbtrend_uniform_layout_get_uniform_index(NULL, "color", &index) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtrend_uniform_layout_get_uniform_index(&layout, NULL, &index) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtrend_uniform_layout_get_uniform_index(&layout, "color", NULL) == RBT_RESULT_INVALID_ARGUMENT);

    // The points end at 80 + 32 = 112, a multiple of 16 already.
    uint32_t size = 1;
    CHECK(rbtrend_uniform_layout_get_size(&layout, &size, &error) == RBT_RESULT_OK);
    CHECK(size == 112);
    CHECK(error.message[0] == '\0');
    // A declared size past the furthest uniform wins, rounded up to 16.
    layout.size = 120;
    CHECK(rbtrend_uniform_layout_get_size(&layout, &size, NULL) == RBT_RESULT_OK);
    CHECK(size == 128);
    layout.size = 0;
    uniforms[1].type = 99;
    size = 1;
    CHECK(rbtrend_uniform_layout_get_size(&layout, &size, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(size == 0);
    CHECK(strcmp(error.message, "uniform 1 has type 99, which is not an RBTREND_UNIFORM_TYPE") == 0);
    uniforms[1].type = RBTREND_UNIFORM_TYPE_FLOAT;
    RBTREND_UNIFORM_LAYOUT empty;
    memset(&empty, 0, sizeof(empty));
    CHECK(rbtrend_uniform_layout_get_size(&empty, &size, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "the layout has no uniforms and a size of 0") == 0);
    // Only the declared size: a block of members with no uniform type.
    empty.size = 40;
    CHECK(rbtrend_uniform_layout_get_size(&empty, &size, NULL) == RBT_RESULT_OK);
    CHECK(size == 48);
    RBTREND_UNIFORM_LAYOUT far;
    far.count = 1;
    far.uniforms = uniforms;
    far.size = UINT32_MAX;
    CHECK(rbtrend_uniform_layout_get_size(&far, &size, &error) == RBT_RESULT_LIMIT);
    CHECK(rbtrend_uniform_layout_get_size(NULL, &size, NULL) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtrend_uniform_layout_get_size(&layout, NULL, NULL) == RBT_RESULT_INVALID_ARGUMENT);
    layout.uniforms = NULL;
    CHECK(rbtrend_uniform_layout_get_size(&layout, &size, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "layout's uniforms is NULL with a count of 4") == 0);
    layout.uniforms = uniforms;

    const float color[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    dirty(&error);
    CHECK(rbtrend_buffer_write_uniform(NULL, &layout, 0, color, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "buffer is NULL") == 0);
    CHECK(rbtrend_uniform_type_get_size(RBTREND_UNIFORM_TYPE_MAT3) == 36);
    CHECK(rbtrend_uniform_type_get_size(99) == 0);

    // The pools.
    RBTREND_VERTEX_POOL_DESC vertex_desc;
    memset(&vertex_desc, 0, sizeof(vertex_desc));
    vertex_desc.base.type = RBT_STRUCT_TYPE_REND_VERTEX_POOL_DESC;
    vertex_desc.vertex_stride = 12;
    vertex_desc.block_vertex_count = 1024;
    rbtrend_vertex_pool vertex_pool = (rbtrend_vertex_pool)(uintptr_t)0x20;
    dirty(&error);
    CHECK(rbtrend_vertex_pool_create(NULL, &vertex_desc, &vertex_pool, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(vertex_pool == NULL);
    CHECK(strcmp(error.message, "instance is NULL") == 0);
    RBTREND_VERTEX_RANGE vertex_range;
    memset(&vertex_range, 0xFF, sizeof(vertex_range));
    CHECK(rbtrend_vertex_pool_allocate(NULL, 3, bytes, &vertex_range, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(vertex_range.buffer == NULL);
    CHECK(vertex_range.vertex_count == 0);
    CHECK(strcmp(error.message, "pool is NULL") == 0);
    CHECK(rbtrend_vertex_pool_free(NULL, &vertex_range, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtrend_vertex_pool_get_usage(NULL, NULL, NULL) == RBT_RESULT_INVALID_ARGUMENT);
    rbtrend_vertex_pool_trim(NULL);
    rbtrend_vertex_pool_destroy(NULL);

    RBTREND_INDEX_POOL_DESC index_desc;
    memset(&index_desc, 0, sizeof(index_desc));
    index_desc.base.type = RBT_STRUCT_TYPE_REND_INDEX_POOL_DESC;
    index_desc.stride = RBTREND_INDEX_STRIDE_UINT32;
    index_desc.block_index_count = 1024;
    rbtrend_index_pool index_pool = (rbtrend_index_pool)(uintptr_t)0x20;
    CHECK(rbtrend_index_pool_create(NULL, &index_desc, &index_pool, NULL) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(index_pool == NULL);
    RBTREND_INDEX_RANGE index_range;
    memset(&index_range, 0xFF, sizeof(index_range));
    CHECK(rbtrend_index_pool_allocate(NULL, 3, bytes, &index_range, NULL) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(index_range.buffer == NULL);
    CHECK(rbtrend_index_pool_free(NULL, &index_range, NULL) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtrend_index_pool_get_usage(NULL, NULL, NULL) == RBT_RESULT_INVALID_ARGUMENT);
    rbtrend_index_pool_trim(NULL);
    rbtrend_index_pool_destroy(NULL);
}

// ---- Lit: vertex colors, cascades and the light helpers (cards A35, A46) ----

// The bits rbtrend_lit_fit_sun_cascades() gave for three framings before card
// A35 renamed it (captured 2026-11-01 from the Debug and Release builds of
// Rabbittus a2bbbee's lit code, which gave the same bits): each row a
// cascade's matrix, column after column, then its split.
static const uint32_t s_fit_perspective_4[][17] = {
    {0x3d829439u, 0xbdbcf5a5u, 0x3bfe40dbu, 0x00000000u, 0x00000000u, 0xbd5237afu, 0xbc9ee888u, 0x00000000u, 0x3dd0ed28u, 0x3d6c330fu, 0xbb9ee888u, 0x00000000u, 0xbe640000u, 0x3e8d0000u, 0x3f53f576u, 0x3f800000u, 0x40feb9c1u},
    {0x3d002b77u, 0xbd397923u, 0x3bb9970au, 0x00000000u, 0x00000000u, 0xbcce56c6u, 0xbc67fccbu, 0x00000000u, 0x3d4d1258u, 0x3ce7d76du, 0xbb67fccdu, 0x00000000u, 0xbf138000u, 0xbca00000u, 0x3f3b4a48u, 0x3f800000u, 0x418ce276u},
    {0x3c70ca56u, 0xbcae3900u, 0x3b68286bu, 0x00000000u, 0x00000000u, 0xbc41d29eu, 0xbc111942u, 0x00000000u, 0x3cc0a1deu, 0x3c59c741u, 0xbb111943u, 0x00000000u, 0xbf290000u, 0xbe0e0000u, 0x3f22de72u, 0x3f800000u, 0x421730cbu},
    {0x3b941e94u, 0xbbd6579au, 0x3ab3466au, 0x00000000u, 0x00000000u, 0xbb6e74a8u, 0xbb601803u, 0x00000000u, 0x3becfdbau, 0x3b85f6bfu, 0xba601803u, 0x00000000u, 0xbf1d0000u, 0xbe380000u, 0x3f0a8adcu, 0x3f800000u, 0x42f00000u},
};
static const uint32_t s_fit_parallel_3[][17] = {
    {0x3c8f876au, 0xbccfb308u, 0x3b82fd59u, 0x00000000u, 0x00000000u, 0xbc6710c5u, 0xbc23bcafu, 0x00000000u, 0x3ce5a577u, 0x3c81cfe5u, 0xbb23bcafu, 0x00000000u, 0xbf0f8000u, 0xbdb80000u, 0x3f289088u, 0x3f800000u, 0x42200000u},
    {0x3c8949e1u, 0xbcc6ab40u, 0x3b7e40dcu, 0x00000000u, 0x00000000u, 0xbc5d04ecu, 0xbc1ee889u, 0x00000000u, 0x3cdba968u, 0x3c785611u, 0xbb1ee88au, 0x00000000u, 0xbfd68000u, 0xbeee0000u, 0x3f2127bbu, 0x3f800000u, 0x42a00000u},
    {0x3c8949dfu, 0xbcc6ab40u, 0x3b7e40dcu, 0x00000000u, 0x00000000u, 0xbc5d04eau, 0xbc1ee889u, 0x00000000u, 0x3cdba968u, 0x3c78560du, 0xbb1ee888u, 0x00000000u, 0xc037e000u, 0xbf5d0000u, 0x3f1a9d74u, 0x3f800000u, 0x42f00000u},
};
static const uint32_t s_fit_reversed_2[][17] = {
    {0x3ced179cu, 0xbd2b8c01u, 0xbbb1edfcu, 0x00000000u, 0x00000000u, 0xbcbed88eu, 0x3c5e697bu, 0x00000000u, 0x3d3dac7du, 0x3cd66f02u, 0x3b5e697bu, 0x00000000u, 0xbeb40000u, 0x3d300000u, 0x3e8cb292u, 0x3f800000u, 0x418ce276u},
    {0x3b8f4e92u, 0xbbcf60c2u, 0xbaae177du, 0x00000000u, 0x00000000u, 0xbb66b540u, 0x3b599d5cu, 0x00000000u, 0x3be54a82u, 0x3b819c7au, 0x3a599d5eu, 0x00000000u, 0xbf040000u, 0xbe160000u, 0x3eea68c2u, 0x3f800000u, 0x42f00000u},
};

// The framings those bits are of: [orthographic], [cascades], [reversed].
static RBTREND_LIT_CASCADE_FIT fit_of(int orthographic, uint32_t cascades, int reversed)
{
    RBTREND_LIT_CASCADE_FIT fit;
    memset(&fit, 0, sizeof(fit));
    fit.base.type = RBT_STRUCT_TYPE_REND_LIT_CASCADE_FIT;
    const float eye[3] = {1.5f, 2.0f, -3.25f}, dir[3] = {0.3f, -0.2f, 1.0f}, up[3] = {0.0f, 1.0f, 0.0f}, light[3] = {-0.4f, 1.0f, 0.25f};
    memcpy(fit.eye_position, eye, sizeof(eye));
    memcpy(fit.view_direction, dir, sizeof(dir));
    memcpy(fit.view_up, up, sizeof(up));
    memcpy(fit.light_direction, light, sizeof(light));
    fit.orthographic = orthographic != 0;
    fit.fov_y = 0.9f;
    fit.half_height = 12.0f;
    fit.aspect = 1.6f;
    fit.near_depth = orthographic ? 0.0f : 0.1f;
    fit.far_depth = 120.0f;
    fit.cascade_count = cascades;
    fit.split_lambda = 0.75f;
    fit.caster_distance = 30.0f;
    fit.map_size = 1024;
    fit.reversed_depth = reversed != 0;
    return fit;
}

// The fit of [fit] into maps whose own fields hold markers: the framing is
// [expected]'s bits, the views and splits past the count are 0, and the
// rectangles, biases and softness are the markers still.
static void check_fit(const RBTREND_LIT_CASCADE_FIT *fit, const uint32_t (*expected)[17])
{
    RBTREND_LIT_SHADOW_MAPS maps;
    memset(&maps, 0x7f, sizeof(maps));
    maps.base.type = RBT_STRUCT_TYPE_REND_LIT_SHADOW_MAPS;
    maps.base.next = NULL;
    maps._rects[2]._x = 77;
    maps._depth_bias = 0.25f;
    maps._normal_offset = 0.5f;
    maps._softness = 1.5f;
    RBTREND_ERROR error;
    dirty(&error);
    CHECK(rbtrend_lit_fit_cascades(fit, &maps, &error) == RBT_RESULT_OK);
    CHECK(maps._view_count == fit->cascade_count);
    for (uint32_t c = 0; c < fit->cascade_count; ++c)
    {
        uint32_t bits[17];
        memcpy(bits, maps._view_projection[c], sizeof(float) * 16);
        memcpy(&bits[16], &maps._split_depths[c], sizeof(float));
        CHECK(memcmp(bits, expected[c], sizeof(bits)) == 0);
    }
    for (uint32_t c = fit->cascade_count; c < 6; ++c)
        for (int i = 0; i < 16; ++i)
            CHECK(maps._view_projection[c][i] == 0.0f);
    for (uint32_t c = fit->cascade_count; c < RBTREND_LIT_MAX_CASCADES; ++c)
        CHECK(maps._split_depths[c] == 0.0f);
    CHECK(maps._rects[2]._x == 77);
    CHECK(maps._depth_bias == 0.25f);
    CHECK(maps._normal_offset == 0.5f);
    CHECK(maps._softness == 1.5f);
}

// rbtrend_lit_fit_cascades() frames the same bits as
// rbtrend_lit_fit_sun_cascades() did, into RBTREND_LIT_SHADOW_MAPS, and
// refuses what it did.
static void test_lit_fit_cascades(void)
{
    current_test = "lit_fit_cascades";
    RBTREND_LIT_CASCADE_FIT fit = fit_of(0, 4, 0);
    check_fit(&fit, s_fit_perspective_4);
    fit = fit_of(1, 3, 0);
    check_fit(&fit, s_fit_parallel_3);
    fit = fit_of(0, 2, 1);
    check_fit(&fit, s_fit_reversed_2);

    RBTREND_LIT_SHADOW_MAPS maps;
    memset(&maps, 0, sizeof(maps));
    maps.base.type = RBT_STRUCT_TYPE_REND_LIT_SHADOW_MAPS;
    RBTREND_ERROR error;
    fit = fit_of(0, 4, 0);
    dirty(&error);
    CHECK(rbtrend_lit_fit_cascades(NULL, &maps, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "fit is NULL") == 0);
    dirty(&error);
    CHECK(rbtrend_lit_fit_cascades(&fit, NULL, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "out_maps is NULL") == 0);
    maps.base.type = RBT_STRUCT_TYPE_REND_LIT_LIGHT_SHADOW;
    dirty(&error);
    CHECK(rbtrend_lit_fit_cascades(&fit, &maps, &error) == RBT_RESULT_STRUCT_WRONG_TYPE);
    CHECK(strcmp(error.message, "out_maps has a base.type other than its own") == 0);
    maps.base.type = RBT_STRUCT_TYPE_REND_LIT_SHADOW_MAPS;
    fit.cascade_count = 0;
    dirty(&error);
    CHECK(rbtrend_lit_fit_cascades(&fit, &maps, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "cascade_count is 0") == 0);
    CHECK(maps._view_count == 0);
    fit = fit_of(0, 4, 0);
    fit.light_direction[0] = fit.light_direction[1] = fit.light_direction[2] = 0.0f;
    dirty(&error);
    CHECK(rbtrend_lit_fit_cascades(&fit, &maps, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "light_direction is 0") == 0);
    fit = fit_of(0, 4, 0);
    fit.map_size = 0;
    dirty(&error);
    CHECK(rbtrend_lit_fit_cascades(&fit, &maps, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "map_size is 0") == 0);
}

// rbtrend_get_lit_vertex_color_layout(): one VEC4 named color at 0, a stride of
// 16 (card A46).
static void test_lit_vertex_color_layout(void)
{
    current_test = "lit_vertex_color_layout";
    const RBTREND_VERTEX_LAYOUT layout = rbtrend_get_lit_vertex_color_layout();
    CHECK(layout.count == 1);
    CHECK(layout.stride == 16);
    CHECK(layout.elements != NULL);
    if (layout.elements)
    {
        CHECK(layout.elements[0].type == RBTREND_VERTEX_ELEMENT_TYPE_VEC4);
        CHECK(layout.elements[0].offset == 0);
    }
    CHECK(rbtrend_vertex_layout_get_element_index(&layout, "color") == 0);
}

// A light the view-projection helpers refuse, as rbtrend_lit_scene_set_lights()
// does (they share its check): [light] refused by [call] with [message].
static void light_refused(const RBTREND_LIT_SCENE_LIGHT *light, int spot, const char *message)
{
    RBTREND_ERROR error;
    float matrix[16];
    dirty(&error);
    const RBT_RESULT result = spot ? rbtrend_lit_spot_shadow_view_projection(light, 0.1f, false, matrix, &error)
                                 : rbtrend_lit_point_shadow_view_projection(light, 0, 0.1f, false, matrix, &error);
    CHECK(result == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, message) == 0);
    if (strcmp(error.message, message) != 0)
        printf("  got \"%s\"\n", error.message);
}

// The light checks set_lights and the helpers share, through the helpers,
// which need no scene: a radius on a point light, a disc on a spot, a
// directional light where a spot is wanted. A scene's own refusals (a fifth
// directional light, a 129th point or spot light, the shadows') need a
// device, and are apricottests' (test_lit_light_refusals).
static void test_lit_light_checks(void)
{
    current_test = "lit_light_checks";
    RBTREND_LIT_SCENE_LIGHT point;
    memset(&point, 0, sizeof(point));
    point._type = RBTREND_LIT_LIGHT_TYPE_POINT;
    point._radiance[0] = point._radiance[1] = point._radiance[2] = 1.0f;
    point._range = 10.0f;
    RBTREND_ERROR error;
    float matrix[16];
    dirty(&error);
    CHECK(rbtrend_lit_point_shadow_view_projection(&point, 0, 0.1f, false, matrix, &error) == RBT_RESULT_OK);
    point._radius = 0.1f;
    light_refused(&point, 0, "light 0, a point or spot light, has a _radius that is not 0 (card A38)");

    RBTREND_LIT_SCENE_LIGHT spot = point;
    spot._type = RBTREND_LIT_LIGHT_TYPE_SPOT;
    spot._radius = 0.0f;
    spot._direction[1] = -1.0f;
    spot._inner_angle = 0.2f;
    spot._outer_angle = 0.4f;
    dirty(&error);
    CHECK(rbtrend_lit_spot_shadow_view_projection(&spot, 0.1f, false, matrix, &error) == RBT_RESULT_OK);
    spot._sky_disc = true;
    light_refused(&spot, 1, "light 0, a point or spot light, has _sky_disc set");
    spot._sky_disc = false;
    spot._outer_angle = 2.0f;
    light_refused(&spot, 1, "spot light 0's _outer_angle is past pi / 2");

    RBTREND_LIT_SCENE_LIGHT directional = spot;
    directional._type = RBTREND_LIT_LIGHT_TYPE_DIRECTIONAL;
    directional._outer_angle = 0.4f;
    directional._radius = 0.00465f;
    directional._sky_disc = true;
    light_refused(&directional, 1, "the light is not a spot");
    directional._type = 3;
    light_refused(&directional, 1, "the light is not a spot");

    // The scene's calls refuse a NULL scene first.
    dirty(&error);
    CHECK(rbtrend_lit_scene_set_shadow_atlas(NULL, NULL, false, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "scene is NULL") == 0);
    dirty(&error);
    CHECK(rbtrend_lit_scene_set_light_shadow(NULL, 0, NULL, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "scene is NULL") == 0);
    CHECK(rbtrend_lit_scene_get_shadow_pass_material(NULL, 0, 0) == NULL);
}

// The reflection commands' parameters fit RBTREND_COMMAND's 120 bytes, at
// the sizes the approved draft gives on a 64-bit build, and the command
// stays 128 bytes; RBTREND_LIT_SHADER_SURFACE_REFLECT_FRAGMENT is 8. Every
// refusal of the three reads a view, so needs a device: apricottests'
// test_lit_reflection_refusals.
static void test_reflection_commands(void)
{
    current_test = "reflection_commands";
    RBTREND_COMMAND command;
    CHECK(sizeof(RBTREND_COMMAND) == 128);
    CHECK(RBTREND_COMMAND_LIT_SSR == 45);
    CHECK(RBTREND_COMMAND_LIT_TRACE_REFLECTIONS == 46);
    CHECK(RBTREND_COMMAND_LIT_REFLECT == 47);
    CHECK(RBTREND_LIT_SHADER_SURFACE_REFLECT_FRAGMENT == 8);
    if (sizeof(void *) == 8)
    {
        CHECK(sizeof(command._params._lit_ssr) == 64);
        CHECK(sizeof(command._params._lit_trace_reflections) == 56);
        CHECK(sizeof(command._params._lit_reflect) == 48);
    }
    CHECK(sizeof(command._params._lit_ssr) <= sizeof(command._params._padding));
    CHECK(sizeof(command._params._lit_trace_reflections) <= sizeof(command._params._padding));
    CHECK(sizeof(command._params._lit_reflect) <= sizeof(command._params._padding));
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
    test_lit_fit_cascades();
    test_lit_vertex_color_layout();
    test_lit_light_checks();
    test_reflection_commands();
    if (failures)
    {
        printf("rbtrend_contract_tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("rbtrend_contract_tests: all passed\n");
    return 0;
}
