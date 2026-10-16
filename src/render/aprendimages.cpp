
#include "render/aprendimages.h"
#include "aprend_internal.hpp"
#include "aprendimages_internal.hpp"

// aprend_texture*_create pass desc->usage straight through as
// SPUDGPU_IMAGE_USAGE, which is only right while the bits line up.
static_assert(APREND_TEXTURE_USAGE_BITS(APREND_TEXTURE_USAGE_BIT_SHADER_RESOURCE) == SPUDGPU_IMAGE_USAGE(SPUDGPU_IMAGE_USAGE_SAMPLED));
static_assert(APREND_TEXTURE_USAGE_BITS(APREND_TEXTURE_USAGE_BIT_RENDER_TARGET) == SPUDGPU_IMAGE_USAGE(SPUDGPU_IMAGE_USAGE_COLOR_ATTACHMENT));
static_assert(APREND_TEXTURE_USAGE_BITS(APREND_TEXTURE_USAGE_BIT_DEPTH_STENCIL) == SPUDGPU_IMAGE_USAGE(SPUDGPU_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT));
static_assert(APREND_TEXTURE_USAGE_BITS(APREND_TEXTURE_USAGE_BIT_STORAGE) == SPUDGPU_IMAGE_USAGE(SPUDGPU_IMAGE_USAGE_STORAGE));

// aprend_texture_view_create_* pass desc->aspect straight through as
// SPUDGPU_IMAGE_ASPECT bits.
static_assert(APREND_TEXTURE_VIEW_ASPECT(APREND_TEXTURE_VIEW_ASPECT_COLOR) == SPUDGPU_IMAGE_ASPECT(SPUDGPU_IMAGE_ASPECT_COLOR));
static_assert(APREND_TEXTURE_VIEW_ASPECT(APREND_TEXTURE_VIEW_ASPECT_DEPTH) == SPUDGPU_IMAGE_ASPECT(SPUDGPU_IMAGE_ASPECT_DEPTH));
static_assert(APREND_TEXTURE_VIEW_ASPECT(APREND_TEXTURE_VIEW_ASPECT_STENCIL) == SPUDGPU_IMAGE_ASPECT(SPUDGPU_IMAGE_ASPECT_STENCIL));
static_assert(APREND_TEXTURE_VIEW_ASPECT(APREND_TEXTURE_VIEW_ASPECT_DEPTH_STENCIL) == (SPUDGPU_IMAGE_ASPECT_DEPTH | SPUDGPU_IMAGE_ASPECT_STENCIL));

// Every aspect [format] has, as SPUDGPU_IMAGE_ASPECT bits and as the
// APREND_TEXTURE_VIEW_ASPECT of a view of all of them.
static uint64_t aprend_texture2d_view_aspect_mask(SPUDGPU_FORMAT format) {
	switch (format) {
	case SPUDGPU_FORMAT_D24_UNORM_S8_UINT:
	case SPUDGPU_FORMAT_D32_FLOAT_S8X24_UINT:
		return SPUDGPU_IMAGE_ASPECT_DEPTH | SPUDGPU_IMAGE_ASPECT_STENCIL;
	case SPUDGPU_FORMAT_D16_UNORM:
	case SPUDGPU_FORMAT_D32_FLOAT:
		return SPUDGPU_IMAGE_ASPECT_DEPTH;
	default:
		return SPUDGPU_IMAGE_ASPECT_COLOR;
	}
}

static uint32_t aprend_compute_mip_levels(
    uint32_t width,
    uint32_t height,
    uint32_t depth = 1) {
	uint32_t max_dim = width;
	if (height > max_dim)
		max_dim = height;
	if (depth > max_dim)
		max_dim = depth;
	uint32_t levels = 1;
	while (max_dim > 1) {
		max_dim >>= 1;
		levels++;
	}
	return levels;
}

// aprend_record_immediate is shared — see aprendimages_internal.hpp.

/* Where an upload to an image is staged: [buffer] at [offset], written
 * through [data]. Either a span of the current frame's transient memory,
 * which the frame takes back once the GPU has finished with it, or, where
 * the instance has none to give or the copy's offset rule can't be met in
 * it, a buffer of the upload's own ([own]), mapped until
 * aprend_upload_staging_end and retired by aprend_upload_staging_release. */
struct aprend_upload_staging {
	spudgpu_buffer buffer{nullptr};
	uint64_t offset{0};
	uint64_t size{0};
	uint8_t *data{nullptr};
	bool own{false};
};

/* Staging for an upload of [size] bytes to [image]. */
static bool aprend_upload_staging_begin(
    aprend_instance instance,
    spudgpu_image image,
    uint64_t size,
    aprend_upload_staging *out) {
	*out      = {};
	out->size = size;
	uint64_t alignment = 0;
	SPUDRESULT sr      = spudgpu_get_image_buffer_copy_offset_alignment(image, &alignment);
	if (SPUDFAIL(sr)) {
		printf("apricot: a texture update can't be staged: %s\n", spudresult_str(sr));
		return false;
	}
	// The transient arena aligns to powers of two only.
	if ((alignment & (alignment - 1)) == 0) {
		aprend_transient_span span{};
		if (aprend_transient_allocate_staging(instance, size, alignment, &span)) {
			out->buffer = ((aprend_transient_chunk *)span._chunk)->buffer;
			out->offset = span._offset;
			out->data   = (uint8_t *)span.data;
			return true;
		}
	}
	spudgpu_buffer_desc staging_desc{};
	staging_desc.usage       = SPUDGPU_BUFFER_USAGE_TRANSFER_SRC;
	staging_desc.memory_kind = SPUDGPU_MEMORY_KIND_UPLOAD;
	staging_desc.size        = size;
	if (SPUDFAIL(spudgpu_create_buffer(instance->desc.device, &staging_desc, &out->buffer))) {
		out->buffer = nullptr;
		return false;
	}
	void *mapped = nullptr;
	if (SPUDFAIL(spudgpu_map_buffer(out->buffer, 0, size, &mapped))) {
		spudgpu_destroy_buffer(out->buffer);
		out->buffer = nullptr;
		return false;
	}
	out->data = (uint8_t *)mapped;
	out->own  = true;
	return true;
}

/* After the bytes are written. A span is flushed by the submission that
 * reads it; a buffer of the upload's own is flushed and unmapped here. */
static void aprend_upload_staging_end(aprend_upload_staging *staging) {
	if (!staging->own)
		return;
	spudgpu_flush_buffer(staging->buffer, 0, staging->size);
	spudgpu_unmap_buffer(staging->buffer);
	staging->data = nullptr;
}

/* Once the copy is recorded, or has failed to be: a buffer of the upload's
 * own is released after the batch that reads it has run. */
static void aprend_upload_staging_release(aprend_instance instance, aprend_upload_staging *staging) {
	if (staging->own)
		aprend_instance_retire(instance, staging->buffer, &aprend_release_spudgpu_buffer);
	*staging = {};
}

// A texture that failed to create may have no image, and a 3D one no view.
aprend_texture2d_t::~aprend_texture2d_t() {
	if (this->image)
		spudgpu_destroy_image(this->image);
	free(this->tracking.layouts);
}
aprend_texture3d_t::~aprend_texture3d_t() {
	// View first: SpudGPU requires an image's views to be gone before it is.
	if (this->image_view)
		spudgpu_destroy_image_view(this->image_view);
	if (this->image)
		spudgpu_destroy_image(this->image);
	free(this->tracking.layouts);
}

// The view a 3D texture owns: every mip of [image].
static SPUDRESULT aprend_texture3d_create_image_view(
    spudgpu_image image,
    SPUDGPU_FORMAT format,
    uint32_t mip_levels,
    spudgpu_image_view *out_view) {
	spudgpu_image_view_desc view_desc{};
	view_desc.parent_image                        = image;
	view_desc.type                                = SPUDGPU_IMAGE_VIEW_TYPE_3D;
	view_desc.subresource_range.aspect_mask       = aprend_texture2d_view_aspect_mask(format);
	view_desc.subresource_range.base_mip_level    = 0;
	view_desc.subresource_range.mip_level_count   = mip_levels;
	view_desc.subresource_range.base_array_layer  = 0;
	view_desc.subresource_range.array_layer_count = 1; // 3D textures do not support array layers
	return spudgpu_create_image_view(image, &view_desc, out_view);
}

// What a resize asks SpudGPU for: [texture_image] as it was created, at a new
// size. SpudGPU's own record of the image is the source, so the debug name is
// its copy rather than a pointer the caller handed over at creation.
// [requested_mip_levels] of 0 means a full chain at the new size; an explicit
// count the new size can't hold fails.
static bool aprend_resized_image_desc(
    spudgpu_image texture_image,
    uint32_t requested_mip_levels,
    uint32_t new_width,
    uint32_t new_height,
    uint32_t new_depth,
    spudgpu_image_desc *out_desc) {
	SPUDRESULT sr = spudgpu_get_image_desc(texture_image, out_desc);
	if (SPUDFAIL(sr)) {
		printf("apricot: texture resize couldn't read the image's descriptor: %s\n", spudresult_str(sr));
		return false;
	}
	const uint32_t full_chain = aprend_compute_mip_levels(new_width, new_height, new_depth);
	if (requested_mip_levels > full_chain) {
		printf("apricot: texture resize to %ux%ux%u can't hold the %u mip levels the texture was created with\n", new_width, new_height,
		       new_depth, requested_mip_levels);
		return false;
	}
	out_desc->gpu_address_location = 0;
	out_desc->width                = new_width;
	out_desc->height               = new_height;
	out_desc->depth                = new_depth;
	out_desc->mip_levels           = requested_mip_levels ? requested_mip_levels : full_chain;
	return true;
}

// The usage bit a texture must have been created with for a view of [type],
// APREND_TEXTURE_USAGE_BIT_NONE if [type] is not a view type.
static APREND_TEXTURE_USAGE_BITS aprend_texture_view_type_usage(APREND_TEXTURE_VIEW_TYPE type) {
	switch (type) {
	case APREND_TEXTURE_VIEW_TYPE_UNORDERED_ACCESS:
		return APREND_TEXTURE_USAGE_BIT_STORAGE;
	case APREND_TEXTURE_VIEW_TYPE_RENDER_TAGET:
		return APREND_TEXTURE_USAGE_BIT_RENDER_TARGET;
	case APREND_TEXTURE_VIEW_TYPE_DEPTH_STENCIL:
		return APREND_TEXTURE_USAGE_BIT_DEPTH_STENCIL;
	case APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE:
		return APREND_TEXTURE_USAGE_BIT_SHADER_RESOURCE;
	default:
		return APREND_TEXTURE_USAGE_BIT_NONE;
	}
}

aprend_texture_view_t::~aprend_texture_view_t() {
	// A view that failed to create has nothing to destroy.
	if (this->image_view)
		spudgpu_destroy_image_view(this->image_view);
}
aprend_sampler_t::~aprend_sampler_t() {
	// A sampler that failed to create has nothing to destroy.
	if (this->sampler)
		spudgpu_destroy_sampler(this->sampler);
}

extern "C" {
aprend_texture2d aprend_texture2d_create(
    aprend_instance instance,
    const aprend_texture2d_desc *desc) {
	/* Implementation-specific texture creation logic goes here. */
	if (!instance || !desc)
		return nullptr;
	if (!(desc->width && desc->height && desc->format))
		return nullptr;
	if (desc->sample_count > 1)
		return nullptr; // MSAA not yet supported by SpudGPU
	if (desc->cube_capable) {
		if (desc->width != desc->height) {
			printf("apricot: aprend_texture2d_create: a cube-capable texture is square, and this is %ux%u\n", desc->width, desc->height);
			return nullptr;
		}
		if (desc->array_layers == 0) {
			printf("apricot: aprend_texture2d_create: a cube-capable texture has array_layers in sixes, and this has 0\n");
			return nullptr;
		}
		if (desc->array_layers % 6 != 0) {
			printf("apricot: aprend_texture2d_create: a cube-capable texture has array_layers in sixes, and this has %u\n", desc->array_layers);
			return nullptr;
		}
	}

	aprend_texture2d_t *result = (aprend_texture2d_t *)malloc(sizeof(aprend_texture2d_t));
	if (result)
		result = new (result) aprend_texture2d_t();
	else
		return nullptr;

	result->instance             = instance;
	result->desc                 = *desc;
	result->requested_mip_levels = desc->mip_levels;

	spudgpu_image_desc image_desc{};
	image_desc.usage        = (SPUDGPU_IMAGE_USAGE)desc->usage | SPUDGPU_IMAGE_USAGE_TRANSFER_DST | SPUDGPU_IMAGE_USAGE_TRANSFER_SRC;
	image_desc.type         = SPUDGPU_IMAGE_TYPE_2D;
	image_desc.format       = desc->format;
	image_desc.width        = desc->width;
	image_desc.height       = desc->height;
	image_desc.depth        = 1;
	image_desc.array_layers = desc->array_layers ? desc->array_layers : 1;
	image_desc.mip_levels   = desc->mip_levels ? desc->mip_levels : aprend_compute_mip_levels(desc->width, desc->height);
	image_desc.cube_capable = desc->cube_capable;
#if _DEBUG
	image_desc.debug_name = desc->debug_name;
#endif

	SPUDRESULT sr = SPUDRESULT_OUT_OF_MEMORY;
	if (!aprend_texture_layouts_reset(&result->tracking, image_desc.mip_levels, image_desc.array_layers))
		goto failedattempt;
	sr = spudgpu_create_image(result->instance->desc.device, &image_desc, &result->image);
	if (sr != SPUD_SUCCESS) {
		result->image = nullptr;
		goto failedattempt;
	}

	// desc->mip_levels/array_layers may have been 0 (meaning "auto"); record
	// what was actually allocated so aprend_texture2d_get_desc and later view
	// creation see the real counts instead of the sentinel.
	result->desc.mip_levels   = image_desc.mip_levels;
	result->desc.array_layers = image_desc.array_layers;

	return result;
failedattempt:
	printf("apricot: aprend_texture2d_create failed (%ux%u): %s\n", desc->width, desc->height, spudresult_str(sr));
	result->~aprend_texture2d_t();
	free(result);
	return nullptr;
}
void aprend_texture2d_destroy(aprend_texture2d texture) {
	if (!texture)
		return;
	// Released once the GPU has finished everything submitted so far.
	aprend_instance_retire(texture->instance, texture, &aprend_release_handle<aprend_texture2d_t>);
}
aprend_texture2d_desc aprend_texture2d_get_desc(aprend_texture2d texture) { return texture ? texture->desc : aprend_texture2d_desc{}; }
spudgpu_image aprend_texture2d_get_spudgpu_image(aprend_texture2d texture) { return texture ? texture->image : nullptr; }
bool aprend_texture2d_update(
    aprend_texture2d texture,
    spudgpu_command_queue queue,
    uint32_t mip_level,
    uint32_t array_layer,
    uint32_t x_offset,
    uint32_t y_offset,
    uint32_t width,
    uint32_t height,
    void **ppData) {
	if (!(texture && width && height && ppData))
		return false;
	if (!queue)
		return false;
	if (!(*ppData))
		return false;
	if (mip_level >= texture->desc.mip_levels)
		return false;
	if (array_layer >= texture->desc.array_layers)
		return false;
	// The region is in texels of the level, which is at least one texel a side.
	const uint32_t mip_width  = aprend_mip_size(texture->desc.width, mip_level);
	const uint32_t mip_height = aprend_mip_size(texture->desc.height, mip_level);
	if (x_offset > mip_width)
		return false;
	if (width > mip_width - x_offset)
		return false;
	if (y_offset > mip_height)
		return false;
	if (height > mip_height - y_offset)
		return false;

	uint32_t bytes_per_pixel = spudgpu_format_bit_count(texture->desc.format) / 8;
	if (bytes_per_pixel == 0 || spudgpu_format_bit_count(texture->desc.format) % 8 != 0) {
		printf("apricot: aprend_texture2d_update: the texture's format has no whole number of bytes to a texel; a block-compressed format has no update path\n");
		return false;
	}
	uint32_t tight_row_pitch = width * bytes_per_pixel;

	// D3D12 requires the buffer-side row pitch to be 256-byte aligned; Vulkan
	// doesn't care. Query the backend's actual required pitch rather than
	// assuming tightly-packed rows work everywhere.
	uint64_t aligned_row_pitch = 0, unused_full_mip_size = 0;
	spudgpu_get_image_buffer_copy_size(texture->image, mip_level, &aligned_row_pitch, &unused_full_mip_size);
	uint64_t region_size = aligned_row_pitch * height;

	aprend_upload_staging staging{};
	if (!aprend_upload_staging_begin(texture->instance, texture->image, region_size, &staging))
		return false;
	// Re-pack the caller's tightly-packed rows into the backend's (possibly padded) row pitch.
	const uint8_t *src = (const uint8_t *)*ppData;
	uint8_t *dst       = staging.data;
	for (uint32_t row = 0; row < height; ++row)
		memcpy(dst + row * aligned_row_pitch, src + row * tight_row_pitch, tight_row_pitch);
	aprend_upload_staging_end(&staging);

	uint32_t buffer_row_length_texels = (uint32_t)(aligned_row_pitch / bytes_per_pixel);

	// Recorded into the upload batch and not waited for: the copy runs ahead
	// of anything submitted after it.
	bool ok = aprend_immediate_on_texture(texture, queue, false, mip_level, array_layer, SPUDGPU_IMAGE_LAYOUT_TRANSFER_DST, SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, [&](spudgpu_command_list cmd) {
		spudgpu_image_buffer_copy_desc copy_desc{};
		copy_desc.buffer_offset     = staging.offset;
		copy_desc.mip_level         = mip_level;
		copy_desc.base_array_layer  = array_layer;
		copy_desc.array_layer_count = 1;
		copy_desc.buffer_row_length = buffer_row_length_texels;
		copy_desc.image_x           = x_offset;
		copy_desc.image_y           = y_offset;
		copy_desc.width             = width;
		copy_desc.height            = height;
		copy_desc.depth             = 1;
		spudgpu_cmd_copy_buffer_to_image(cmd, staging.buffer, texture->image, &copy_desc);
	});

	aprend_upload_staging_release(texture->instance, &staging);
	return ok;
}
bool aprend_texture2d_get_data(
    aprend_texture2d texture,
    spudgpu_command_queue queue,
    uint32_t mip_level,
    uint32_t array_layer,
    uint32_t x_offset,
    uint32_t y_offset,
    uint32_t width,
    uint32_t height,
    void **ppData) {
	if (!(texture && width && height && ppData))
		return false;
	if (!queue)
		return false;
	if (!(*ppData))
		return false;
	if (mip_level >= texture->desc.mip_levels)
		return false;
	if (array_layer >= texture->desc.array_layers)
		return false;
	// The region is in texels of the level, which is at least one texel a side.
	const uint32_t mip_width  = aprend_mip_size(texture->desc.width, mip_level);
	const uint32_t mip_height = aprend_mip_size(texture->desc.height, mip_level);
	if (x_offset > mip_width)
		return false;
	if (width > mip_width - x_offset)
		return false;
	if (y_offset > mip_height)
		return false;
	if (height > mip_height - y_offset)
		return false;

	uint32_t bytes_per_pixel = spudgpu_format_bit_count(texture->desc.format) / 8;
	if (bytes_per_pixel == 0 || spudgpu_format_bit_count(texture->desc.format) % 8 != 0) {
		printf("apricot: aprend_texture2d_get_data: the texture's format has no whole number of bytes to a texel; a block-compressed format has no readback path\n");
		return false;
	}
	uint32_t tight_row_pitch = width * bytes_per_pixel;

	uint64_t aligned_row_pitch = 0, unused_full_mip_size = 0;
	spudgpu_get_image_buffer_copy_size(texture->image, mip_level, &aligned_row_pitch, &unused_full_mip_size);
	uint64_t region_size = aligned_row_pitch * height;

	spudgpu_buffer_desc staging_desc{};
	staging_desc.usage        = SPUDGPU_BUFFER_USAGE_TRANSFER_DST;
	staging_desc.memory_kind  = SPUDGPU_MEMORY_KIND_READBACK;
	staging_desc.size         = region_size;

	spudgpu_buffer staging_buffer;
	if (spudgpu_create_buffer(texture->instance->desc.device, &staging_desc, &staging_buffer) != SPUD_SUCCESS)
		return false;

	uint32_t buffer_row_length_texels = (uint32_t)(aligned_row_pitch / bytes_per_pixel);

	// Recorded into the upload batch, which is then submitted and waited
	// for: the caller reads what the copy wrote.
	bool ok = aprend_immediate_on_texture(texture, queue, true, mip_level, array_layer, SPUDGPU_IMAGE_LAYOUT_TRANSFER_SRC, SPUDGPU_IMAGE_LAYOUT_TRANSFER_SRC, [&](spudgpu_command_list cmd) {
		spudgpu_image_buffer_copy_desc copy_desc{};
		copy_desc.mip_level         = mip_level;
		copy_desc.base_array_layer  = array_layer;
		copy_desc.array_layer_count = 1;
		copy_desc.buffer_row_length = buffer_row_length_texels;
		copy_desc.image_x           = x_offset;
		copy_desc.image_y           = y_offset;
		copy_desc.width             = width;
		copy_desc.height            = height;
		copy_desc.depth             = 1;
		spudgpu_cmd_copy_image_to_buffer(cmd, texture->image, staging_buffer, &copy_desc);
	});
	if (!ok) {
		// The copy may be recorded and still run with a later submission.
		aprend_instance_retire(texture->instance, staging_buffer, &aprend_release_spudgpu_buffer);
		return false;
	}

	void *pMapped;
	if (spudgpu_map_buffer(staging_buffer, 0, region_size, &pMapped) != SPUD_SUCCESS) {
		spudgpu_destroy_buffer(staging_buffer);
		return false;
	}
	spudgpu_invalidate_buffer(staging_buffer, 0, region_size);

	// Unpack rows from the backend's (possibly padded) pitch into the caller's tightly-packed destination.
	const uint8_t *src = (const uint8_t *)pMapped;
	uint8_t *dst       = (uint8_t *)*ppData;
	for (uint32_t row = 0; row < height; ++row)
		memcpy(dst + row * tight_row_pitch, src + row * aligned_row_pitch, tight_row_pitch);
	spudgpu_unmap_buffer(staging_buffer);

	spudgpu_destroy_buffer(staging_buffer);
	return true;
}
bool aprend_texture2d_resize(
    aprend_texture2d texture,
    uint32_t new_width,
    uint32_t new_height) {
	if (!texture)
		return false;
	if (!new_width)
		return false;
	if (!new_height)
		return false;
	if (new_width == texture->desc.width && new_height == texture->desc.height)
		return true; // No resize needed
	if (texture->desc.cube_capable && new_width != new_height) {
		printf("apricot: aprend_texture2d_resize: a cube-capable texture stays square, and %ux%u is not\n", new_width, new_height);
		return false;
	}

	spudgpu_image_desc image_desc{};
	if (!aprend_resized_image_desc(texture->image, texture->requested_mip_levels, new_width, new_height, 1, &image_desc))
		return false;

	spudgpu_image new_image = nullptr;
	SPUDRESULT sr           = spudgpu_create_image(texture->instance->desc.device, &image_desc, &new_image);
	if (SPUDFAIL(sr)) {
		printf("apricot: aprend_texture2d_resize to %ux%u failed: %s\n", new_width, new_height, spudresult_str(sr));
		return false;
	}
	// A new image, with no contents: every mip of every layer is back to no
	// layout. Done before the old image goes, so running out of memory for
	// a different mip count still leaves the texture as it was.
	if (!aprend_texture_layouts_reset(&texture->tracking, image_desc.mip_levels, image_desc.array_layers)) {
		printf("apricot: aprend_texture2d_resize to %ux%u failed: out of memory\n", new_width, new_height);
		spudgpu_destroy_image(new_image);
		return false;
	}

	// The old image goes only now that its replacement exists, so a failed
	// resize leaves the texture exactly as it was. Submitted work may still
	// be using it, so it is released when that has finished.
	aprend_instance_retire(texture->instance, texture->image, &aprend_release_spudgpu_image);
	texture->image           = new_image;
	texture->desc.width      = new_width;
	texture->desc.height     = new_height;
	texture->desc.mip_levels = image_desc.mip_levels;
	return true;
}
aprend_texture3d aprend_texture3d_create(
    aprend_instance instance,
    const aprend_texture3d_desc *desc) {
	if (!instance || !desc)
		return nullptr;
	if (!(desc->width && desc->height && desc->depth && desc->format))
		return nullptr;

	aprend_texture3d_t *result = (aprend_texture3d_t *)malloc(sizeof(aprend_texture3d_t));
	if (result)
		result = new (result) aprend_texture3d_t();
	else
		return nullptr;

	result->instance             = instance;
	result->desc                 = *desc;
	result->requested_mip_levels = desc->mip_levels;
	spudgpu_image_desc image_desc{};
	image_desc.usage        = (SPUDGPU_IMAGE_USAGE)desc->usage | SPUDGPU_IMAGE_USAGE_TRANSFER_DST | SPUDGPU_IMAGE_USAGE_TRANSFER_SRC;
	image_desc.type         = SPUDGPU_IMAGE_TYPE_3D;
	image_desc.format       = desc->format;
	image_desc.width        = desc->width;
	image_desc.height       = desc->height;
	image_desc.depth        = desc->depth;
	image_desc.array_layers = 1; // 3D textures do not support array layers
	image_desc.mip_levels   = desc->mip_levels ? desc->mip_levels : aprend_compute_mip_levels(desc->width, desc->height, desc->depth);
#if _DEBUG
	image_desc.debug_name = desc->debug_name;
#endif

	SPUDRESULT sr = SPUDRESULT_OUT_OF_MEMORY;
	if (!aprend_texture_layouts_reset(&result->tracking, image_desc.mip_levels, 1))
		goto failedattempt;
	sr = spudgpu_create_image(result->instance->desc.device, &image_desc, &result->image);
	if (sr != SPUD_SUCCESS) {
		result->image = nullptr;
		goto failedattempt;
	}

	// desc->mip_levels may have been 0 (meaning "auto"); record what was
	// actually allocated so aprend_texture3d_get_desc and later view creation
	// see the real count instead of the sentinel.
	result->desc.mip_levels = image_desc.mip_levels;

	sr = aprend_texture3d_create_image_view(result->image, desc->format, image_desc.mip_levels, &result->image_view);
	if (sr != SPUD_SUCCESS) {
		result->image_view = nullptr;
		goto failedattempt;
	}

	return result;
failedattempt:
	printf("apricot: aprend_texture3d_create failed (%ux%ux%u): %s\n", desc->width, desc->height, desc->depth, spudresult_str(sr));
	result->~aprend_texture3d_t();
	free(result);
	return nullptr;
}
void aprend_texture3d_destroy(aprend_texture3d texture) {
	if (!texture)
		return;
	// Released once the GPU has finished everything submitted so far.
	aprend_instance_retire(texture->instance, texture, &aprend_release_handle<aprend_texture3d_t>);
}
aprend_texture3d_desc aprend_texture3d_get_desc(aprend_texture3d texture) { return texture ? texture->desc : aprend_texture3d_desc{}; }
spudgpu_image_view aprend_texture3d_get_spudgpu_image_view(aprend_texture3d texture) { return texture ? texture->image_view : NULL; }
spudgpu_image aprend_texture3d_get_spudgpu_image(aprend_texture3d texture) { return texture ? texture->image : NULL; }
bool aprend_texture3d_update(
    aprend_texture3d texture,
    spudgpu_command_queue queue,
    uint32_t mip_level,
    uint32_t x_offset,
    uint32_t y_offset,
    uint32_t z_offset,
    uint32_t width,
    uint32_t height,
    uint32_t depth,
    void **ppData) {
	if (!(texture && width && height && depth && ppData))
		return false;
	if (!queue)
		return false;
	if (!(*ppData))
		return false;
	if (mip_level >= texture->desc.mip_levels)
		return false;
	// The region is in texels of the level, which is at least one texel a side.
	const uint32_t mip_width  = aprend_mip_size(texture->desc.width, mip_level);
	const uint32_t mip_height = aprend_mip_size(texture->desc.height, mip_level);
	const uint32_t mip_depth  = aprend_mip_size(texture->desc.depth, mip_level);
	if (x_offset > mip_width)
		return false;
	if (width > mip_width - x_offset)
		return false;
	if (y_offset > mip_height)
		return false;
	if (height > mip_height - y_offset)
		return false;
	if (z_offset > mip_depth)
		return false;
	if (depth > mip_depth - z_offset)
		return false;

	uint32_t bytes_per_pixel   = spudgpu_format_bit_count(texture->desc.format) / 8;
	if (bytes_per_pixel == 0 || spudgpu_format_bit_count(texture->desc.format) % 8 != 0) {
		printf("apricot: aprend_texture3d_update: the texture's format has no whole number of bytes to a texel; a block-compressed format has no update path\n");
		return false;
	}
	uint32_t tight_row_pitch   = width * bytes_per_pixel;
	uint64_t tight_slice_pitch = (uint64_t)tight_row_pitch * height;

	// Only row pitch needs backend alignment (D3D12: 256 bytes); the stride
	// between depth slices in our own staging buffer just needs to match
	// whatever row pitch we actually used.
	uint64_t aligned_row_pitch = 0, unused_full_mip_size = 0;
	spudgpu_get_image_buffer_copy_size(texture->image, mip_level, &aligned_row_pitch, &unused_full_mip_size);
	uint64_t aligned_slice_pitch = aligned_row_pitch * height;
	uint64_t region_size         = aligned_slice_pitch * depth;

	aprend_upload_staging staging{};
	if (!aprend_upload_staging_begin(texture->instance, texture->image, region_size, &staging))
		return false;
	const uint8_t *src = (const uint8_t *)*ppData;
	uint8_t *dst       = staging.data;
	for (uint32_t z = 0; z < depth; ++z)
		for (uint32_t row = 0; row < height; ++row)
			memcpy(dst + z * aligned_slice_pitch + row * aligned_row_pitch, src + z * tight_slice_pitch + row * tight_row_pitch, tight_row_pitch);
	aprend_upload_staging_end(&staging);

	uint32_t buffer_row_length_texels = (uint32_t)(aligned_row_pitch / bytes_per_pixel);

	// Recorded into the upload batch and not waited for: the copy runs ahead
	// of anything submitted after it.
	bool ok = aprend_immediate_on_texture(texture, queue, false, mip_level, 0, SPUDGPU_IMAGE_LAYOUT_TRANSFER_DST, SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, [&](spudgpu_command_list cmd) {
		spudgpu_image_buffer_copy_desc copy_desc{};
		copy_desc.buffer_offset       = staging.offset;
		copy_desc.mip_level           = mip_level;
		copy_desc.array_layer_count   = 1;
		copy_desc.buffer_row_length   = buffer_row_length_texels;
		copy_desc.buffer_image_height = height;
		copy_desc.image_x             = x_offset;
		copy_desc.image_y             = y_offset;
		copy_desc.image_z             = z_offset;
		copy_desc.width               = width;
		copy_desc.height              = height;
		copy_desc.depth               = depth;
		spudgpu_cmd_copy_buffer_to_image(cmd, staging.buffer, texture->image, &copy_desc);
	});

	aprend_upload_staging_release(texture->instance, &staging);
	return ok;
}
bool aprend_texture3d_get_data(
    aprend_texture3d texture,
    spudgpu_command_queue queue,
    uint32_t mip_level,
    uint32_t x_offset,
    uint32_t y_offset,
    uint32_t z_offset,
    uint32_t width,
    uint32_t height,
    uint32_t depth,
    void **ppData) {
	if (!(texture && width && height && depth && ppData))
		return false;
	if (!queue)
		return false;
	if (!(*ppData))
		return false;
	if (mip_level >= texture->desc.mip_levels)
		return false;
	// The region is in texels of the level, which is at least one texel a side.
	const uint32_t mip_width  = aprend_mip_size(texture->desc.width, mip_level);
	const uint32_t mip_height = aprend_mip_size(texture->desc.height, mip_level);
	const uint32_t mip_depth  = aprend_mip_size(texture->desc.depth, mip_level);
	if (x_offset > mip_width)
		return false;
	if (width > mip_width - x_offset)
		return false;
	if (y_offset > mip_height)
		return false;
	if (height > mip_height - y_offset)
		return false;
	if (z_offset > mip_depth)
		return false;
	if (depth > mip_depth - z_offset)
		return false;

	uint32_t bytes_per_pixel   = spudgpu_format_bit_count(texture->desc.format) / 8;
	if (bytes_per_pixel == 0 || spudgpu_format_bit_count(texture->desc.format) % 8 != 0) {
		printf("apricot: aprend_texture3d_get_data: the texture's format has no whole number of bytes to a texel; a block-compressed format has no readback path\n");
		return false;
	}
	uint32_t tight_row_pitch   = width * bytes_per_pixel;
	uint64_t tight_slice_pitch = (uint64_t)tight_row_pitch * height;

	uint64_t aligned_row_pitch = 0, unused_full_mip_size = 0;
	spudgpu_get_image_buffer_copy_size(texture->image, mip_level, &aligned_row_pitch, &unused_full_mip_size);
	uint64_t aligned_slice_pitch = aligned_row_pitch * height;
	uint64_t region_size         = aligned_slice_pitch * depth;

	spudgpu_buffer_desc staging_desc{};
	staging_desc.usage        = SPUDGPU_BUFFER_USAGE_TRANSFER_DST;
	staging_desc.memory_kind  = SPUDGPU_MEMORY_KIND_READBACK;
	staging_desc.size         = region_size;

	spudgpu_buffer staging_buffer;
	if (spudgpu_create_buffer(texture->instance->desc.device, &staging_desc, &staging_buffer) != SPUD_SUCCESS)
		return false;

	uint32_t buffer_row_length_texels = (uint32_t)(aligned_row_pitch / bytes_per_pixel);

	// Recorded into the upload batch, which is then submitted and waited
	// for: the caller reads what the copy wrote.
	bool ok = aprend_immediate_on_texture(texture, queue, true, mip_level, 0, SPUDGPU_IMAGE_LAYOUT_TRANSFER_SRC, SPUDGPU_IMAGE_LAYOUT_TRANSFER_SRC, [&](spudgpu_command_list cmd) {
		spudgpu_image_buffer_copy_desc copy_desc{};
		copy_desc.mip_level           = mip_level;
		copy_desc.array_layer_count   = 1;
		copy_desc.buffer_row_length   = buffer_row_length_texels;
		copy_desc.buffer_image_height = height;
		copy_desc.image_x             = x_offset;
		copy_desc.image_y             = y_offset;
		copy_desc.image_z             = z_offset;
		copy_desc.width               = width;
		copy_desc.height              = height;
		copy_desc.depth               = depth;
		spudgpu_cmd_copy_image_to_buffer(cmd, texture->image, staging_buffer, &copy_desc);
	});
	if (!ok) {
		// The copy may be recorded and still run with a later submission.
		aprend_instance_retire(texture->instance, staging_buffer, &aprend_release_spudgpu_buffer);
		return false;
	}

	void *pMapped;
	if (spudgpu_map_buffer(staging_buffer, 0, region_size, &pMapped) != SPUD_SUCCESS) {
		spudgpu_destroy_buffer(staging_buffer);
		return false;
	}
	spudgpu_invalidate_buffer(staging_buffer, 0, region_size);

	const uint8_t *src = (const uint8_t *)pMapped;
	uint8_t *dst       = (uint8_t *)*ppData;
	for (uint32_t z = 0; z < depth; ++z)
		for (uint32_t row = 0; row < height; ++row)
			memcpy(dst + z * tight_slice_pitch + row * tight_row_pitch, src + z * aligned_slice_pitch + row * aligned_row_pitch, tight_row_pitch);
	spudgpu_unmap_buffer(staging_buffer);

	spudgpu_destroy_buffer(staging_buffer);
	return true;
}
bool aprend_texture3d_resize(
    aprend_texture3d texture,
    uint32_t new_width,
    uint32_t new_height,
    uint32_t new_depth) {
	if (!texture)
		return false;
	if (!new_width)
		return false;
	if (!new_height)
		return false;
	if (!new_depth)
		return false;
	if (new_width == texture->desc.width && new_height == texture->desc.height && new_depth == texture->desc.depth)
		return true; // No resize needed

	spudgpu_image_desc image_desc{};
	if (!aprend_resized_image_desc(texture->image, texture->requested_mip_levels, new_width, new_height, new_depth, &image_desc))
		return false;

	spudgpu_image new_image = nullptr;
	SPUDRESULT sr           = spudgpu_create_image(texture->instance->desc.device, &image_desc, &new_image);
	if (SPUDFAIL(sr)) {
		printf("apricot: aprend_texture3d_resize to %ux%ux%u failed: %s\n", new_width, new_height, new_depth, spudresult_str(sr));
		return false;
	}
	spudgpu_image_view new_view = nullptr;
	sr                          = aprend_texture3d_create_image_view(new_image, texture->desc.format, image_desc.mip_levels, &new_view);
	if (SPUDFAIL(sr)) {
		printf("apricot: aprend_texture3d_resize to %ux%ux%u failed (view): %s\n", new_width, new_height, new_depth, spudresult_str(sr));
		spudgpu_destroy_image(new_image);
		return false;
	}
	// As aprend_texture2d_resize: every mip is back to no layout.
	if (!aprend_texture_layouts_reset(&texture->tracking, image_desc.mip_levels, 1)) {
		printf("apricot: aprend_texture3d_resize to %ux%ux%u failed: out of memory\n", new_width, new_height, new_depth);
		spudgpu_destroy_image_view(new_view);
		spudgpu_destroy_image(new_image);
		return false;
	}

	// The old image and its view go only now that their replacements exist,
	// so a failed resize leaves the texture exactly as it was. Submitted
	// work may still be using them, so they are released when that has
	// finished, the view before its image.
	aprend_instance_retire(texture->instance, texture->image_view, &aprend_release_spudgpu_image_view);
	aprend_instance_retire(texture->instance, texture->image, &aprend_release_spudgpu_image);
	texture->image           = new_image;
	texture->image_view      = new_view;
	texture->desc.width      = new_width;
	texture->desc.height     = new_height;
	texture->desc.depth      = new_depth;
	texture->desc.mip_levels = image_desc.mip_levels;
	return true;
}

// What is common to a view of a 2D and of a 3D texture: the use is a view
// type the texture's usage allows, the mip range is inside [mip_levels], and
// a use that takes one mip is given one. [desc]'s mip_count is resolved from
// APREND_TEXTURE_VIEW_REMAINING in [out_desc]. [who] is the calling function,
// for the message.
static bool aprend_texture_view_desc_valid(
    const char *who,
    const aprend_texture_view_desc *desc,
    APREND_TEXTURE_USAGE_BITS texture_usage,
    SPUDGPU_FORMAT format,
    uint32_t mip_levels,
    aprend_texture_view_desc *out_desc) {
	const APREND_TEXTURE_USAGE_BITS needed_usage = aprend_texture_view_type_usage(desc->type);
	if (needed_usage == APREND_TEXTURE_USAGE_BIT_NONE) {
		printf("apricot: %s: type %u is not a view type\n", who, (unsigned)desc->type);
		return false;
	}
	if (!(texture_usage & needed_usage)) {
		printf("apricot: %s: a view of type %u needs usage bit %u, and the texture's usage is %u\n", who, (unsigned)desc->type,
		       (unsigned)needed_usage, (unsigned)texture_usage);
		return false;
	}
	if (desc->base_mip >= mip_levels) {
		printf("apricot: %s: base_mip %u is not below the texture's %u mip levels\n", who, desc->base_mip, mip_levels);
		return false;
	}
	// The aspect is one the format has. A view rendered to covers all of
	// them; a view a shader reads covers one.
	const uint64_t format_aspects = aprend_texture2d_view_aspect_mask(format);
	if (desc->aspect == APREND_TEXTURE_VIEW_ASPECT_NONE) {
		printf("apricot: %s: aspect is APREND_TEXTURE_VIEW_ASPECT_NONE\n", who);
		return false;
	}
	if (desc->aspect & ~format_aspects) {
		printf("apricot: %s: aspect %u is not in the texture's format, whose aspects are %u\n", who, (unsigned)desc->aspect, (unsigned)format_aspects);
		return false;
	}
	if (desc->type == APREND_TEXTURE_VIEW_TYPE_DEPTH_STENCIL && desc->aspect != format_aspects) {
		printf("apricot: %s: a view of type DEPTH_STENCIL covers every aspect of its format (%u), and aspect is %u\n", who, (unsigned)format_aspects,
		       (unsigned)desc->aspect);
		return false;
	}
	if (desc->type == APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE && desc->aspect == APREND_TEXTURE_VIEW_ASPECT_DEPTH_STENCIL) {
		printf("apricot: %s: a view of type SHADER_RESOURCE reads one aspect: APREND_TEXTURE_VIEW_ASPECT_DEPTH or _STENCIL\n", who);
		return false;
	}
	*out_desc = *desc;
	if (desc->mip_count == APREND_TEXTURE_VIEW_REMAINING)
		out_desc->mip_count = mip_levels - desc->base_mip;
	if (out_desc->mip_count == 0) {
		printf("apricot: %s: mip_count is 0\n", who);
		return false;
	}
	if (out_desc->mip_count > mip_levels - desc->base_mip) {
		printf("apricot: %s: %u mips from mip %u run past the texture's %u mip levels\n", who, out_desc->mip_count, desc->base_mip, mip_levels);
		return false;
	}
	// A pass renders to one mip, and a storage image slot is one mip.
	if (desc->type != APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE && out_desc->mip_count != 1) {
		printf("apricot: %s: a view of type %u covers one mip, and mip_count is %u\n", who, (unsigned)desc->type, out_desc->mip_count);
		return false;
	}
	return true;
}

aprend_texture_view aprend_texture_view_create_2d(
    aprend_texture2d texture,
    const aprend_texture_view_desc *desc) {
	static const char *who = "aprend_texture_view_create_2d";
	if (!texture)
		return nullptr;
	if (!desc)
		return nullptr;
	aprend_texture_view_desc resolved{};
	if (!aprend_texture_view_desc_valid(who, desc, texture->desc.usage, texture->desc.format, texture->desc.mip_levels, &resolved))
		return nullptr;

	const uint32_t array_layers = texture->desc.array_layers;
	if (desc->base_layer >= array_layers) {
		printf("apricot: %s: base_layer %u is not below the texture's %u array layers\n", who, desc->base_layer, array_layers);
		return nullptr;
	}
	if (desc->layer_count == APREND_TEXTURE_VIEW_REMAINING)
		resolved.layer_count = array_layers - desc->base_layer;
	if (resolved.layer_count == 0) {
		printf("apricot: %s: layer_count is 0\n", who);
		return nullptr;
	}
	if (resolved.layer_count > array_layers - desc->base_layer) {
		printf("apricot: %s: %u layers from layer %u run past the texture's %u array layers\n", who, resolved.layer_count, desc->base_layer,
		       array_layers);
		return nullptr;
	}

	// The shape decides the SpudGPU view type and how many layers it takes.
	uint32_t spud_view_type = SPUDGPU_IMAGE_VIEW_TYPE_NONE;
	switch (desc->shape) {
	case APREND_TEXTURE_VIEW_SHAPE_2D:
		if (resolved.layer_count != 1) {
			printf("apricot: %s: a 2D view is one layer, and layer_count is %u\n", who, resolved.layer_count);
			return nullptr;
		}
		spud_view_type = SPUDGPU_IMAGE_VIEW_TYPE_2D;
		break;
	case APREND_TEXTURE_VIEW_SHAPE_2D_ARRAY:
		spud_view_type = SPUDGPU_IMAGE_VIEW_TYPE_2D_ARRAY;
		break;
	case APREND_TEXTURE_VIEW_SHAPE_CUBE:
		if (!texture->desc.cube_capable) {
			printf("apricot: %s: a cube view of a texture that was not created cube_capable\n", who);
			return nullptr;
		}
		if (desc->base_layer != 0) {
			printf("apricot: %s: a cube view starts at layer 0, and base_layer is %u\n", who, desc->base_layer);
			return nullptr;
		}
		if (resolved.layer_count != 6) {
			printf("apricot: %s: a cube view is six layers, and layer_count is %u\n", who, resolved.layer_count);
			return nullptr;
		}
		spud_view_type = SPUDGPU_IMAGE_VIEW_TYPE_CUBE;
		break;
	case APREND_TEXTURE_VIEW_SHAPE_CUBE_ARRAY:
		if (!texture->desc.cube_capable) {
			printf("apricot: %s: a cube array view of a texture that was not created cube_capable\n", who);
			return nullptr;
		}
		if (resolved.layer_count % 6 != 0) {
			printf("apricot: %s: a cube array view is layers in sixes, and layer_count is %u\n", who, resolved.layer_count);
			return nullptr;
		}
		spud_view_type = SPUDGPU_IMAGE_VIEW_TYPE_CUBE_ARRAY;
		break;
	case APREND_TEXTURE_VIEW_SHAPE_3D:
		printf("apricot: %s: a 3D view of a 2D texture\n", who);
		return nullptr;
	default:
		printf("apricot: %s: shape %u is not a view shape\n", who, (unsigned)desc->shape);
		return nullptr;
	}
	// A pass renders to one layer through a 2D view, or to every layer of a
	// 2D array view at once (a layered pass). A cube is rendered to as the 2D
	// array its layers are: no graphics API has a cube render target.
	const bool is_target = desc->type == APREND_TEXTURE_VIEW_TYPE_RENDER_TAGET || desc->type == APREND_TEXTURE_VIEW_TYPE_DEPTH_STENCIL;
	if (is_target && desc->shape != APREND_TEXTURE_VIEW_SHAPE_2D && desc->shape != APREND_TEXTURE_VIEW_SHAPE_2D_ARRAY) {
		printf("apricot: %s: a view rendered to is APREND_TEXTURE_VIEW_SHAPE_2D or _2D_ARRAY, and shape is %u\n", who, (unsigned)desc->shape);
		return nullptr;
	}
	if (is_target && desc->shape == APREND_TEXTURE_VIEW_SHAPE_2D_ARRAY && !texture->instance->layered_rendering) {
		printf("apricot: %s: a 2D array view rendered to needs layered rendering, which the device doesn't support\n", who);
		return nullptr;
	}

	aprend_texture_view_t *result = (aprend_texture_view_t *)malloc(sizeof(aprend_texture_view_t));
	if (!result)
		return nullptr;
	result        = new (result) aprend_texture_view_t();

	result->instance     = texture->instance;
	result->dimension    = APREND_TEXTURE_DIMENSION_2D;
	result->view_type    = desc->type;
	result->texture._t2d = texture;
	result->desc         = resolved;

	spudgpu_image_view_desc imgvd{};
	imgvd.parent_image                        = texture->image;
	imgvd.type                                = spud_view_type;
	imgvd.subresource_range.aspect_mask       = desc->aspect;
	imgvd.subresource_range.base_mip_level    = resolved.base_mip;
	imgvd.subresource_range.mip_level_count   = resolved.mip_count;
	imgvd.subresource_range.base_array_layer  = resolved.base_layer;
	imgvd.subresource_range.array_layer_count = resolved.layer_count;
	SPUDRESULT sr = spudgpu_create_image_view(texture->image, &imgvd, &result->image_view);
	if (SPUDFAIL(sr)) {
		result->image_view = nullptr;
		goto failedattempt;
	}

	return result;
failedattempt:
	printf("apricot: %s failed: %s\n", who, spudresult_str(sr));
	result->~aprend_texture_view_t();
	free(result);
	return nullptr;
}
aprend_texture_view aprend_texture_view_create_3d(
    aprend_texture3d texture,
    const aprend_texture_view_desc *desc) {
	static const char *who = "aprend_texture_view_create_3d";
	if (!texture)
		return nullptr;
	if (!desc)
		return nullptr;
	aprend_texture_view_desc resolved{};
	if (!aprend_texture_view_desc_valid(who, desc, texture->desc.usage, texture->desc.format, texture->desc.mip_levels, &resolved))
		return nullptr;
	if (desc->type == APREND_TEXTURE_VIEW_TYPE_RENDER_TAGET || desc->type == APREND_TEXTURE_VIEW_TYPE_DEPTH_STENCIL) {
		printf("apricot: %s: a 3D texture can't be rendered to through a view\n", who);
		return nullptr;
	}
	if (desc->shape != APREND_TEXTURE_VIEW_SHAPE_3D) {
		printf("apricot: %s: a view of a 3D texture is APREND_TEXTURE_VIEW_SHAPE_3D, and shape is %u\n", who, (unsigned)desc->shape);
		return nullptr;
	}
	// A 3D texture has one layer.
	if (desc->base_layer != 0) {
		printf("apricot: %s: base_layer is %u, and a 3D texture has only layer 0\n", who, desc->base_layer);
		return nullptr;
	}
	if (desc->layer_count != 1 && desc->layer_count != APREND_TEXTURE_VIEW_REMAINING) {
		printf("apricot: %s: layer_count is %u, and a 3D texture has one layer\n", who, desc->layer_count);
		return nullptr;
	}
	resolved.layer_count = 1;

	aprend_texture_view_t *result = (aprend_texture_view_t *)malloc(sizeof(aprend_texture_view_t));
	if (!result)
		return nullptr;
	result        = new (result) aprend_texture_view_t();

	result->instance     = texture->instance;
	result->dimension    = APREND_TEXTURE_DIMENSION_3D;
	result->view_type    = desc->type;
	result->texture._t3d = texture;
	result->desc         = resolved;

	spudgpu_image_view_desc imgvd{};
	imgvd.parent_image                        = texture->image;
	imgvd.type                                = SPUDGPU_IMAGE_VIEW_TYPE_3D;
	imgvd.subresource_range.aspect_mask       = desc->aspect;
	imgvd.subresource_range.base_mip_level    = resolved.base_mip;
	imgvd.subresource_range.mip_level_count   = resolved.mip_count;
	imgvd.subresource_range.base_array_layer  = 0;
	imgvd.subresource_range.array_layer_count = 1; // 3D textures do not support array layers
	SPUDRESULT sr = spudgpu_create_image_view(texture->image, &imgvd, &result->image_view);
	if (SPUDFAIL(sr)) {
		result->image_view = nullptr;
		goto failedattempt;
	}

	return result;
failedattempt:
	printf("apricot: %s failed: %s\n", who, spudresult_str(sr));
	result->~aprend_texture_view_t();
	free(result);
	return nullptr;
}
void aprend_destroy_texture_view(aprend_texture_view view) {
	if (!view)
		return;
	// Released once the GPU has finished everything submitted so far.
	aprend_instance_retire(view->instance, view, &aprend_release_handle<aprend_texture_view_t>);
}
APREND_TEXTURE_VIEW_TYPE aprend_texture_view_get_type(aprend_texture_view view) { return view ? view->view_type : APREND_TEXTURE_VIEW_TYPE_NONE; }
aprend_texture_view_desc aprend_texture_view_get_desc(aprend_texture_view view) { return view ? view->desc : aprend_texture_view_desc{}; }
APREND_TEXTURE_DIMENSION aprend_texture_view_get_dimension(aprend_texture_view view) { return view ? view->dimension : APREND_TEXTURE_DIMENSION_1D; }
spudgpu_image_view aprend_texture_view_get_spudgpu_image_view(aprend_texture_view view) { return view ? view->image_view : nullptr; }
aprend_texture2d aprend_texture_view_get_texture2d(aprend_texture_view view) {
	return view && view->dimension == APREND_TEXTURE_DIMENSION_2D ? view->texture._t2d : nullptr;
}

aprend_sampler aprend_sampler_create(
    aprend_instance instance,
    const aprend_sampler_desc *desc) {
	if (!instance)
		return nullptr;
	if (!desc)
		return nullptr;

	APREND_MALLOC__T(result, aprend_sampler_t);
	if (!result)
		return nullptr;
	APREND_CONSTRUCT__T(result, aprend_sampler_t);
	result->instance = instance;
	result->desc     = *desc;

	spudgpu_sampler_desc sd{};
	sd.mag_filter     = desc->mag_filter;
	sd.min_filter     = desc->min_filter;
	sd.mipmap_filter  = desc->mipmap_filter;
	sd.address_mode_u = desc->address_mode_u;
	sd.address_mode_v = desc->address_mode_v;
	sd.address_mode_w = desc->address_mode_w;
	sd.mip_lod_bias   = desc->mip_lod_bias;
	sd.min_lod        = desc->min_lod;
	sd.max_lod        = desc->max_lod;
	sd.max_anisotropy = desc->max_anisotropy;
#if _DEBUG
	sd.debug_name = desc->debug_name;
#endif

	SPUDRESULT sr = spudgpu_create_sampler(instance->desc.device, &sd, &result->sampler);
	if (SPUDFAIL(sr)) {
		printf("apricot: aprend_sampler_create failed: %s\n", spudresult_str(sr));
		APREND_DESTRUCT__T(result, aprend_sampler_t);
		return nullptr;
	}
	return result;
}
void aprend_sampler_destroy(aprend_sampler sampler) {
	if (!sampler)
		return;
	// Released once the GPU has finished everything submitted so far.
	aprend_instance_retire(sampler->instance, sampler, &aprend_release_handle<aprend_sampler_t>);
}
aprend_sampler_desc aprend_sampler_get_desc(aprend_sampler sampler) { return sampler ? sampler->desc : aprend_sampler_desc{}; }
spudgpu_sampler aprend_sampler_get_spudgpu_sampler(aprend_sampler sampler) { return sampler ? sampler->sampler : nullptr; }
}
