
#include "render/rbtrendimages.h"
#include "rbtrend_internal.hpp"
#include "rbtrendimages_internal.hpp"

// rbtrend_texture*_create pass desc->usage straight through as
// CRGPU_IMAGE_USAGE, which is only right while the bits line up.
static_assert(RBTREND_TEXTURE_USAGE_BITS(RBTREND_TEXTURE_USAGE_BIT_SHADER_RESOURCE) == CRGPU_IMAGE_USAGE(CRGPU_IMAGE_USAGE_SAMPLED));
static_assert(RBTREND_TEXTURE_USAGE_BITS(RBTREND_TEXTURE_USAGE_BIT_RENDER_TARGET) == CRGPU_IMAGE_USAGE(CRGPU_IMAGE_USAGE_COLOR_ATTACHMENT));
static_assert(RBTREND_TEXTURE_USAGE_BITS(RBTREND_TEXTURE_USAGE_BIT_DEPTH_STENCIL) == CRGPU_IMAGE_USAGE(CRGPU_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT));
static_assert(RBTREND_TEXTURE_USAGE_BITS(RBTREND_TEXTURE_USAGE_BIT_STORAGE) == CRGPU_IMAGE_USAGE(CRGPU_IMAGE_USAGE_STORAGE));

// rbtrend_texture_view_create_* pass desc->aspect straight through as
// CRGPU_IMAGE_ASPECT bits.
static_assert(RBTREND_TEXTURE_VIEW_ASPECT(RBTREND_TEXTURE_VIEW_ASPECT_COLOR) == CRGPU_IMAGE_ASPECT(CRGPU_IMAGE_ASPECT_COLOR));
static_assert(RBTREND_TEXTURE_VIEW_ASPECT(RBTREND_TEXTURE_VIEW_ASPECT_DEPTH) == CRGPU_IMAGE_ASPECT(CRGPU_IMAGE_ASPECT_DEPTH));
static_assert(RBTREND_TEXTURE_VIEW_ASPECT(RBTREND_TEXTURE_VIEW_ASPECT_STENCIL) == CRGPU_IMAGE_ASPECT(CRGPU_IMAGE_ASPECT_STENCIL));
static_assert(RBTREND_TEXTURE_VIEW_ASPECT(RBTREND_TEXTURE_VIEW_ASPECT_DEPTH_STENCIL) == (CRGPU_IMAGE_ASPECT_DEPTH | CRGPU_IMAGE_ASPECT_STENCIL));

// Every aspect [format] has, as CRGPU_IMAGE_ASPECT bits and as the
// RBTREND_TEXTURE_VIEW_ASPECT of a view of all of them.
static uint64_t rbtrend_texture2d_view_aspect_mask(CRGPU_FORMAT format) {
	switch (format) {
	case CRGPU_FORMAT_D24_UNORM_S8_UINT:
	case CRGPU_FORMAT_D32_FLOAT_S8X24_UINT:
		return CRGPU_IMAGE_ASPECT_DEPTH | CRGPU_IMAGE_ASPECT_STENCIL;
	case CRGPU_FORMAT_D16_UNORM:
	case CRGPU_FORMAT_D32_FLOAT:
		return CRGPU_IMAGE_ASPECT_DEPTH;
	default:
		return CRGPU_IMAGE_ASPECT_COLOR;
	}
}

static uint32_t rbtrend_compute_mip_levels(
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

// rbtrend_record_immediate is shared — see rbtrendimages_internal.hpp.


/* Where an upload to an image is staged: [buffer] at [offset], written
 * through [data]. Either a span of the current frame's transient memory,
 * which the frame takes back once the GPU has finished with it, or, where
 * the instance has none to give or the copy's offset rule can't be met in
 * it, a buffer of the upload's own ([own]), mapped until
 * rbtrend_upload_staging_end and retired by rbtrend_upload_staging_release. */
struct rbtrend_upload_staging {
	crgpu_buffer buffer{nullptr};
	uint64_t offset{0};
	uint64_t size{0};
	uint8_t *data{nullptr};
	bool own{false};
};

/* Staging for an upload of [size] bytes to [image]. A buffer of the upload's
 * own has its retire promised here (rbtrend_instance_reserve_release), so
 * rbtrend_upload_staging_release can't fail. */
static RBT_RESULT rbtrend_upload_staging_begin(
    rbtrend_instance instance,
    crgpu_image image,
    uint64_t size,
    rbtrend_upload_staging *out,
    RBTREND_ERROR *error) {
	*out      = {};
	out->size = size;
	uint64_t alignment = 0;
	CR_RESULT sr      = crgpu_image_get_buffer_copy_offset_alignment(image, &alignment);
	if (CR_FAILED(sr))
		return rbtrend_fail_cr(error, sr, "reading the image's copy offset alignment failed");
	// The transient arena aligns to powers of two only.
	if ((alignment & (alignment - 1)) == 0) {
		RBTREND_TRANSIENT_SPAN span{};
		if (rbtrend_transient_allocate_staging(instance, size, alignment, &span)) {
			out->buffer = ((rbtrend_transient_chunk *)span._chunk)->buffer;
			out->offset = span._offset;
			out->data   = (uint8_t *)span.data;
			return RBT_RESULT_OK;
		}
	}
	if (rbtrend_instance_reserve_release(instance, 1) != RBT_RESULT_OK)
		return rbtrend_fail(error, RBT_RESULT_OUT_OF_MEMORY, "no memory to release a staging buffer later");
	crgpu_buffer_desc staging_desc{};
	staging_desc.base.type   = CR_STRUCT_TYPE_GPU_BUFFER_DESC;
	staging_desc.usage       = CRGPU_BUFFER_USAGE_TRANSFER_SRC;
	staging_desc.memory_kind = CRGPU_MEMORY_KIND_UPLOAD;
	staging_desc.size        = size;
	sr                       = crgpu_buffer_create(instance->desc.device, &staging_desc, &out->buffer);
	if (CR_FAILED(sr)) {
		out->buffer = nullptr;
		rbtrend_instance_unreserve_release(instance, 1);
		return rbtrend_fail_cr(error, sr, "creating a staging buffer of %llu bytes failed", (unsigned long long)size);
	}
	void *mapped = nullptr;
	sr           = crgpu_buffer_map(out->buffer, 0, size, &mapped);
	if (CR_FAILED(sr)) {
		crgpu_buffer_destroy(out->buffer);
		out->buffer = nullptr;
		rbtrend_instance_unreserve_release(instance, 1);
		return rbtrend_fail_cr(error, sr, "mapping a staging buffer failed");
	}
	out->data = (uint8_t *)mapped;
	out->own  = true;
	return RBT_RESULT_OK;
}

/* After the bytes are written. A span is flushed by the submission that
 * reads it; a buffer of the upload's own is flushed and unmapped here. */
static void rbtrend_upload_staging_end(rbtrend_upload_staging *staging) {
	if (!staging->own)
		return;
	crgpu_buffer_flush(staging->buffer, 0, staging->size);
	crgpu_buffer_unmap(staging->buffer);
	staging->data = nullptr;
}

/* Once the copy is recorded, or has failed to be: a buffer of the upload's
 * own is released after the batch that reads it has run. */
static void rbtrend_upload_staging_release(rbtrend_instance instance, rbtrend_upload_staging *staging) {
	if (staging->own)
		rbtrend_instance_retire(instance, staging->buffer, &rbtrend_release_crgpu_buffer);
	*staging = {};
}

/* Releases what [steps] hold, each set before the views it binds: retired,
 * for a chain the GPU may still be reading, or at once, from the texture's
 * destructor, when nothing is. */
static void rbtrend_mip_steps_release(std::vector<rbtrend_mip_step> &steps, bool retire) {
	for (rbtrend_mip_step &step : steps) {
		if (retire) {
			rbtrend_binding_set_destroy(step.set);
			rbtrend_texture_view_destroy(step.source);
			rbtrend_texture_view_destroy(step.target);
			continue;
		}
		if (step.set)
			rbtrend_release_handle<rbtrend_binding_set_t>(step.set);
		if (step.source)
			rbtrend_release_handle<rbtrend_texture_view_t>(step.source);
		if (step.target)
			rbtrend_release_handle<rbtrend_texture_view_t>(step.target);
	}
	steps.clear();
}

/* Every chain of [texture], as rbtrend_mip_steps_release. */
static void rbtrend_mip_chains_release(rbtrend_texture2d texture, bool retire) {
	for (rbtrend_mip_chain &chain : texture->mip_chains)
		rbtrend_mip_steps_release(chain.steps, retire);
	texture->mip_chains.clear();
}

// A texture that failed to create may have no image, and a 3D one no view.
rbtrend_texture2d_t::~rbtrend_texture2d_t() {
	// Its mip chains' views, and the view a present draws it through, go
	// before its image, as Carrottus GPU requires. The GPU has finished with them:
	// the texture is released only once it has.
	rbtrend_mip_chains_release(this, false);
	if (this->present_view)
		rbtrend_release_handle<rbtrend_texture_view_t>(this->present_view);
	if (this->image)
		crgpu_image_destroy(this->image);
	free(this->tracking.layouts);
}
rbtrend_texture3d_t::~rbtrend_texture3d_t() {
	// View first: Carrottus GPU requires an image's views to be gone before it is.
	if (this->image_view)
		crgpu_image_view_destroy(this->image_view);
	if (this->image)
		crgpu_image_destroy(this->image);
	free(this->tracking.layouts);
}

// The view a 3D texture owns: every mip of [image].
static CR_RESULT rbtrend_texture3d_create_image_view(
    crgpu_image image,
    CRGPU_FORMAT format,
    uint32_t mip_levels,
    crgpu_image_view *out_view) {
	crgpu_image_view_desc view_desc{};
	view_desc.base.type                           = CR_STRUCT_TYPE_GPU_IMAGE_VIEW_DESC;
	view_desc.parent_image                        = image;
	view_desc.type                                = CRGPU_IMAGE_VIEW_TYPE_3D;
	view_desc.subresource_range.aspect_mask       = rbtrend_texture2d_view_aspect_mask(format);
	view_desc.subresource_range.base_mip_level    = 0;
	view_desc.subresource_range.mip_level_count   = mip_levels;
	view_desc.subresource_range.base_array_layer  = 0;
	view_desc.subresource_range.array_layer_count = 1; // 3D textures do not support array layers
	return crgpu_image_view_create(image, &view_desc, out_view);
}

// What a resize asks Carrottus GPU for: [texture_image] as it was created, at a new
// size. Carrottus GPU's own record of the image is the source, so the debug name is
// its copy rather than a pointer the caller handed over at creation.
// [requested_mip_levels] of 0 means a full chain at the new size; an explicit
// count the new size can't hold fails.
static RBT_RESULT rbtrend_resized_image_desc(
    crgpu_image texture_image,
    uint32_t requested_mip_levels,
    uint32_t new_width,
    uint32_t new_height,
    uint32_t new_depth,
    crgpu_image_desc *out_desc,
    RBTREND_ERROR *error) {
	CR_RESULT sr = crgpu_image_get_desc(texture_image, out_desc);
	if (CR_FAILED(sr))
		return rbtrend_fail_cr(error, sr, "reading the image's descriptor failed");
	const uint32_t full_chain = rbtrend_compute_mip_levels(new_width, new_height, new_depth);
	if (requested_mip_levels > full_chain)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "%ux%ux%u can't hold the %u mip levels the texture was created with", new_width, new_height,
		                   new_depth, requested_mip_levels);
	out_desc->width      = new_width;
	out_desc->height     = new_height;
	out_desc->depth      = new_depth;
	out_desc->mip_levels = requested_mip_levels ? requested_mip_levels : full_chain;
	return RBT_RESULT_OK;
}

// The usage bit a texture must have been created with for a view of [type],
// RBTREND_TEXTURE_USAGE_BIT_NONE if [type] is not a view type.
static RBTREND_TEXTURE_USAGE_BITS rbtrend_texture_view_type_usage(RBTREND_TEXTURE_VIEW_TYPE type) {
	switch (type) {
	case RBTREND_TEXTURE_VIEW_TYPE_UNORDERED_ACCESS:
		return RBTREND_TEXTURE_USAGE_BIT_STORAGE;
	case RBTREND_TEXTURE_VIEW_TYPE_RENDER_TARGET:
		return RBTREND_TEXTURE_USAGE_BIT_RENDER_TARGET;
	case RBTREND_TEXTURE_VIEW_TYPE_DEPTH_STENCIL:
		return RBTREND_TEXTURE_USAGE_BIT_DEPTH_STENCIL;
	case RBTREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE:
		return RBTREND_TEXTURE_USAGE_BIT_SHADER_RESOURCE;
	default:
		return RBTREND_TEXTURE_USAGE_BIT_NONE;
	}
}

rbtrend_texture_view_t::~rbtrend_texture_view_t() {
	// The set that binds it goes first. The GPU has finished with both: the
	// view is released only once it has.
	if (this->bloom_set)
		rbtrend_release_handle<rbtrend_binding_set_t>(this->bloom_set);
	if (this->environment_set)
		rbtrend_release_handle<rbtrend_binding_set_t>(this->environment_set);
	if (this->cull_depth_set)
		rbtrend_release_handle<rbtrend_binding_set_t>(this->cull_depth_set);
	if (this->rt_set)
		rbtrend_release_handle<rbtrend_binding_set_t>(this->rt_set);
	for (rbtrend_binding_set set : this->present_sets)
		if (set)
			rbtrend_release_handle<rbtrend_binding_set_t>(set);
	// A view that failed to create has nothing to destroy.
	if (this->image_view)
		crgpu_image_view_destroy(this->image_view);
}
rbtrend_sampler_t::~rbtrend_sampler_t() {
	// A sampler that failed to create has nothing to destroy.
	if (this->sampler)
		crgpu_sampler_destroy(this->sampler);
}

/* The bytes of a texel of [format], for an update or a readback: 0 for a
 * format with no whole number of them (a block-compressed one, or one with
 * no known size), which has no update or readback path. */
static uint32_t rbtrend_texel_bytes(CRGPU_FORMAT format) {
	const uint32_t bits = crgpu_format_get_bit_count(format);
	if (bits == 0 || bits % 8 != 0)
		return 0;
	return bits / 8;
}

/* What an update or a readback of a region checks, past the handle, in the
 * order of their parameters: a queue, a level and a layer that exist, a
 * region inside the level, a format with whole bytes to a texel, and the
 * caller's memory, [data_size] bytes at [data], the region's bytes exactly.
 * [mip_*] is the texture's size, [layers] its layer count (1 for 3D). The
 * region's bytes, tightly packed, go to [out_texel_bytes] and
 * [out_region_bytes]. */
static RBT_RESULT rbtrend_texture_region_valid(
    crgpu_command_queue queue,
    uint32_t mip_levels,
    uint32_t layers,
    uint32_t mip_level,
    uint32_t array_layer,
    const uint32_t size[3],
    const uint32_t offset[3],
    const uint32_t extent[3],
    CRGPU_FORMAT format,
    const void *data,
    uint64_t data_size,
    uint32_t *out_texel_bytes,
    uint64_t *out_region_bytes,
    RBTREND_ERROR *error) {
	static const char axis[3][7] = {"width", "height", "depth"};
	static const char off[3][9]  = {"x_offset", "y_offset", "z_offset"};
	if (!queue)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "queue is NULL");
	if (mip_level >= mip_levels)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "mip_level %u is not below the texture's %u mip levels", mip_level, mip_levels);
	if (array_layer >= layers)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "array_layer %u is not below the texture's %u array layers", array_layer, layers);
	// The region is in texels of the level, which is at least one texel a
	// side.
	for (int i = 0; i < 3; ++i) {
		const uint32_t level = rbtrend_mip_size(size[i], mip_level);
		if (offset[i] > level)
			return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "%s %u is past the level's %s of %u", off[i], offset[i], axis[i], level);
	}
	for (int i = 0; i < 3; ++i) {
		const uint32_t level = rbtrend_mip_size(size[i], mip_level);
		if (extent[i] == 0)
			return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "%s is 0", axis[i]);
		if (extent[i] > level - offset[i])
			return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "%s %u from %s %u runs past the level's %s of %u", axis[i], extent[i], off[i], offset[i],
			                   axis[i], level);
	}
	const uint32_t texel_bytes = rbtrend_texel_bytes(format);
	if (texel_bytes == 0)
		return rbtrend_fail(error, RBT_RESULT_UNSUPPORTED, "format %d has no whole number of bytes to a texel; a block-compressed format has no update or readback path",
		                   (int)format);
	if (!data)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "data is NULL");
	const uint64_t region_bytes = (uint64_t)extent[0] * extent[1] * extent[2] * texel_bytes;
	if (data_size != region_bytes)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "data_size is %llu, and the region is %llu bytes", (unsigned long long)data_size,
		                   (unsigned long long)region_bytes);
	*out_texel_bytes  = texel_bytes;
	*out_region_bytes = region_bytes;
	return RBT_RESULT_OK;
}

/* Writes a region of [texture]'s [image] from the caller's tightly packed
 * texels: staged with the backend's row pitch, then a copy recorded into the
 * frame's batch. A 2D texture has [extent][2] 1; a 3D one ([three_d])
 * [array_layer] 0, and its slices' height given to the copy. */
template <typename Texture>
static RBT_RESULT rbtrend_texture_upload(
    Texture *texture,
    bool three_d,
    crgpu_command_queue queue,
    uint32_t mip_level,
    uint32_t array_layer,
    const uint32_t offset[3],
    const uint32_t extent[3],
    uint32_t texel_bytes,
    const void *data,
    RBTREND_ERROR *error) {
	const uint32_t tight_row_pitch   = extent[0] * texel_bytes;
	const uint64_t tight_slice_pitch = (uint64_t)tight_row_pitch * extent[1];
	// D3D12 requires the buffer-side row pitch to be 256-byte aligned; Vulkan
	// doesn't care. Query the backend's actual required pitch rather than
	// assuming tightly-packed rows work everywhere. Between depth slices the
	// stride in the staging is whatever row pitch was used.
	uint64_t aligned_row_pitch = 0, unused_full_mip_size = 0;
	crgpu_image_get_buffer_copy_size(texture->image, mip_level, &aligned_row_pitch, &unused_full_mip_size);
	const uint64_t aligned_slice_pitch = aligned_row_pitch * extent[1];
	const uint64_t region_size         = aligned_slice_pitch * extent[2];

	rbtrend_upload_staging staging{};
	if (const RBT_RESULT result = rbtrend_upload_staging_begin(texture->instance, texture->image, region_size, &staging, error))
		return result;
	// Re-pack the caller's tightly-packed rows into the backend's (possibly padded) row pitch.
	const uint8_t *src = (const uint8_t *)data;
	for (uint32_t z = 0; z < extent[2]; ++z)
		for (uint32_t row = 0; row < extent[1]; ++row)
			memcpy(staging.data + z * aligned_slice_pitch + row * aligned_row_pitch, src + z * tight_slice_pitch + row * tight_row_pitch, tight_row_pitch);
	rbtrend_upload_staging_end(&staging);

	const uint32_t buffer_row_length_texels = (uint32_t)(aligned_row_pitch / texel_bytes);

	// Recorded into the upload batch and not waited for: the copy runs ahead
	// of anything submitted after it.
	const RBT_RESULT result = rbtrend_immediate_on_texture(texture, queue, false, mip_level, array_layer, CRGPU_IMAGE_LAYOUT_TRANSFER_DST,
	                                                    rbtrend_texture_upload_rest_layout(texture->desc.usage), error, [&](crgpu_command_list cmd) {
		                                                    crgpu_image_buffer_copy_desc copy_desc{};
		                                                    copy_desc.buffer_offset       = staging.offset;
		                                                    copy_desc.mip_level           = mip_level;
		                                                    copy_desc.base_array_layer    = array_layer;
		                                                    copy_desc.array_layer_count   = 1;
		                                                    copy_desc.buffer_row_length   = buffer_row_length_texels;
		                                                    copy_desc.buffer_image_height = three_d ? extent[1] : 0;
		                                                    copy_desc.image_x             = offset[0];
		                                                    copy_desc.image_y             = offset[1];
		                                                    copy_desc.image_z             = offset[2];
		                                                    copy_desc.width               = extent[0];
		                                                    copy_desc.height              = extent[1];
		                                                    copy_desc.depth               = extent[2];
		                                                    crgpu_command_list_copy_buffer_to_image(cmd, staging.buffer, texture->image, &copy_desc);
	                                                    });

	rbtrend_upload_staging_release(texture->instance, &staging);
	return result;
}

/* Reads a region of [texture]'s [image] back into the caller's memory,
 * tightly packed: a copy recorded into the frame's batch, which is then
 * submitted and waited for. As rbtrend_texture_upload for the region. */
template <typename Texture>
static RBT_RESULT rbtrend_texture_readback(
    Texture *texture,
    bool three_d,
    crgpu_command_queue queue,
    uint32_t mip_level,
    uint32_t array_layer,
    const uint32_t offset[3],
    const uint32_t extent[3],
    uint32_t texel_bytes,
    void *out_data,
    RBTREND_ERROR *error) {
	const uint32_t tight_row_pitch   = extent[0] * texel_bytes;
	const uint64_t tight_slice_pitch = (uint64_t)tight_row_pitch * extent[1];
	uint64_t aligned_row_pitch = 0, unused_full_mip_size = 0;
	crgpu_image_get_buffer_copy_size(texture->image, mip_level, &aligned_row_pitch, &unused_full_mip_size);
	const uint64_t aligned_slice_pitch = aligned_row_pitch * extent[1];
	const uint64_t region_size         = aligned_slice_pitch * extent[2];

	// A failed copy may still run with a later submission, so its buffer is
	// then retired: the room for that is taken first.
	if (rbtrend_instance_reserve_release(texture->instance, 1) != RBT_RESULT_OK)
		return rbtrend_fail(error, RBT_RESULT_OUT_OF_MEMORY, "no memory to release a readback buffer later");
	crgpu_buffer_desc staging_desc{};
	staging_desc.base.type   = CR_STRUCT_TYPE_GPU_BUFFER_DESC;
	staging_desc.usage       = CRGPU_BUFFER_USAGE_TRANSFER_DST;
	staging_desc.memory_kind = CRGPU_MEMORY_KIND_READBACK;
	staging_desc.size        = region_size;
	crgpu_buffer staging_buffer = nullptr;
	CR_RESULT sr                 = crgpu_buffer_create(texture->instance->desc.device, &staging_desc, &staging_buffer);
	if (CR_FAILED(sr)) {
		rbtrend_instance_unreserve_release(texture->instance, 1);
		return rbtrend_fail_cr(error, sr, "creating a readback buffer of %llu bytes failed", (unsigned long long)region_size);
	}

	const uint32_t buffer_row_length_texels = (uint32_t)(aligned_row_pitch / texel_bytes);

	// Recorded into the upload batch, which is then submitted and waited
	// for: the caller reads what the copy wrote.
	const RBT_RESULT result = rbtrend_immediate_on_texture(texture, queue, true, mip_level, array_layer, CRGPU_IMAGE_LAYOUT_TRANSFER_SRC,
	                                                    CRGPU_IMAGE_LAYOUT_TRANSFER_SRC, error, [&](crgpu_command_list cmd) {
		                                                    crgpu_image_buffer_copy_desc copy_desc{};
		                                                    copy_desc.mip_level           = mip_level;
		                                                    copy_desc.base_array_layer    = array_layer;
		                                                    copy_desc.array_layer_count   = 1;
		                                                    copy_desc.buffer_row_length   = buffer_row_length_texels;
		                                                    copy_desc.buffer_image_height = three_d ? extent[1] : 0;
		                                                    copy_desc.image_x             = offset[0];
		                                                    copy_desc.image_y             = offset[1];
		                                                    copy_desc.image_z             = offset[2];
		                                                    copy_desc.width               = extent[0];
		                                                    copy_desc.height              = extent[1];
		                                                    copy_desc.depth               = extent[2];
		                                                    crgpu_command_list_copy_image_to_buffer(cmd, texture->image, staging_buffer, &copy_desc);
	                                                    });
	if (result != RBT_RESULT_OK) {
		// The copy may be recorded and still run with a later submission.
		rbtrend_instance_retire(texture->instance, staging_buffer, &rbtrend_release_crgpu_buffer);
		return result;
	}
	rbtrend_instance_unreserve_release(texture->instance, 1);

	void *mapped = nullptr;
	sr           = crgpu_buffer_map(staging_buffer, 0, region_size, &mapped);
	if (CR_FAILED(sr)) {
		crgpu_buffer_destroy(staging_buffer);
		return rbtrend_fail_cr(error, sr, "mapping the readback buffer failed");
	}
	crgpu_buffer_invalidate(staging_buffer, 0, region_size);

	// Unpack rows from the backend's (possibly padded) pitch into the caller's tightly-packed destination.
	const uint8_t *src = (const uint8_t *)mapped;
	uint8_t *dst       = (uint8_t *)out_data;
	for (uint32_t z = 0; z < extent[2]; ++z)
		for (uint32_t row = 0; row < extent[1]; ++row)
			memcpy(dst + z * tight_slice_pitch + row * tight_row_pitch, src + z * aligned_slice_pitch + row * aligned_row_pitch, tight_row_pitch);
	crgpu_buffer_unmap(staging_buffer);

	crgpu_buffer_destroy(staging_buffer);
	return RBT_RESULT_OK;
}

extern "C" {
RBT_RESULT rbtrend_texture2d_create(
    rbtrend_instance instance,
    const RBTREND_TEXTURE2D_DESC *desc,
    rbtrend_texture2d *out_texture,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (out_texture)
		*out_texture = nullptr;
	if (!instance)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "instance is NULL");
	if (RBT_STALE(instance, "rbtrend_texture2d_create", "instance"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "instance is destroyed");
	RBTREND_ENTER(instance, "rbtrend_texture2d_create");
	if (!desc)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc is NULL");
	if (const RBT_RESULT base = rbtrend_base_check(out_error, "desc", &desc->base, RBT_STRUCT_TYPE_REND_TEXTURE2D_DESC))
		return base;
	if (desc->width == 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "width is 0");
	if (desc->height == 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "height is 0");
	if (desc->format == CRGPU_FORMAT_UNKNOWN)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "format is CRGPU_FORMAT_UNKNOWN");
	if (desc->sample_count > 1)
		return rbtrend_fail(out_error, RBT_RESULT_UNSUPPORTED, "sample_count is %u, and MSAA is not supported yet", desc->sample_count);
	if (desc->cube_capable && desc->width != desc->height)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "a cube-capable texture is square, and this is %ux%u", desc->width, desc->height);
	if (desc->cube_capable && desc->array_layers == 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "a cube-capable texture has array_layers in sixes, and this has 0");
	if (desc->cube_capable && desc->array_layers % 6 != 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "a cube-capable texture has array_layers in sixes, and this has %u", desc->array_layers);
	if (!out_texture)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "out_texture is NULL");

	rbtrend_texture2d_t *result = (rbtrend_texture2d_t *)malloc(sizeof(rbtrend_texture2d_t));
	if (!result)
		return rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory for the texture");
	result = new (result) rbtrend_texture2d_t();

	result->instance             = instance;
	result->desc                 = *desc;
	result->requested_mip_levels = desc->mip_levels;

	crgpu_image_desc image_desc{};
	image_desc.base.type    = CR_STRUCT_TYPE_GPU_IMAGE_DESC;
	image_desc.usage        = (CRGPU_IMAGE_USAGE)desc->usage | CRGPU_IMAGE_USAGE_TRANSFER_DST | CRGPU_IMAGE_USAGE_TRANSFER_SRC;
	image_desc.type         = CRGPU_IMAGE_TYPE_2D;
	image_desc.format       = desc->format;
	image_desc.width        = desc->width;
	image_desc.height       = desc->height;
	image_desc.depth        = 1;
	image_desc.array_layers = desc->array_layers ? desc->array_layers : 1;
	image_desc.mip_levels   = desc->mip_levels ? desc->mip_levels : rbtrend_compute_mip_levels(desc->width, desc->height);
	image_desc.cube_capable = desc->cube_capable;
	image_desc.debug_name   = desc->debug_name;

	RBT_RESULT result_code = RBT_RESULT_OK;
	if (!rbtrend_texture_layouts_reset(&result->tracking, image_desc.mip_levels, image_desc.array_layers)) {
		result_code = rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory for the texture's layouts");
		goto failedattempt;
	}
	{
		const CR_RESULT sr = crgpu_image_create(result->instance->desc.device, &image_desc, &result->image);
		if (CR_FAILED(sr)) {
			result->image = nullptr;
			result_code   = rbtrend_fail_cr(out_error, sr, "creating a %ux%u image failed", desc->width, desc->height);
			goto failedattempt;
		}
	}
	// Its destroy's retire, promised now so the destroy can't fail.
	if (rbtrend_instance_reserve_release(instance, 1) != RBT_RESULT_OK) {
		result_code = rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory to release the texture later");
		goto failedattempt;
	}

	// desc->mip_levels/array_layers may have been 0 (meaning "auto"); record
	// what was actually allocated so rbtrend_texture2d_get_desc and later view
	// creation see the real counts instead of the sentinel.
	result->desc.mip_levels   = image_desc.mip_levels;
	result->desc.array_layers = image_desc.array_layers;

	*out_texture = result;
	return RBT_RESULT_OK;
failedattempt:
	result->~rbtrend_texture2d_t();
	free(result);
	return result_code;
}
void rbtrend_texture2d_destroy(rbtrend_texture2d texture) {
	if (!texture)
		return;
	if (RBT_STALE(texture, "rbtrend_texture2d_destroy", "texture"))
		return;
	// A list compiled against it and not submitted would read its layouts.
	rbtrend_instance_forget(texture->instance, &texture->tracking);
	// Released once the GPU has finished everything submitted so far.
	RBT_KILL(texture);
	rbtrend_instance_retire(texture->instance, texture, &rbtrend_release_handle<rbtrend_texture2d_t>);
}
RBT_RESULT rbtrend_texture2d_get_desc(
    rbtrend_texture2d texture,
    RBTREND_TEXTURE2D_DESC *out_desc) {
	if (!texture)
		return RBT_RESULT_INVALID_ARGUMENT;
	if (RBT_STALE(texture, "rbtrend_texture2d_get_desc", "texture"))
		return RBT_RESULT_INVALID_ARGUMENT;
	if (!out_desc)
		return RBT_RESULT_INVALID_ARGUMENT;
	*out_desc      = texture->desc;
	out_desc->base = rbt_struct_base(RBT_STRUCT_TYPE_REND_TEXTURE2D_DESC);
	return RBT_RESULT_OK;
}
crgpu_image rbtrend_texture2d_get_crgpu_image(rbtrend_texture2d texture) {
	if (!texture)
		return nullptr;
	if (RBT_STALE(texture, "rbtrend_texture2d_get_crgpu_image", "texture"))
		return nullptr;
	return texture->image;
}
RBT_RESULT rbtrend_texture2d_update(
    rbtrend_texture2d texture,
    crgpu_command_queue queue,
    uint32_t mip_level,
    uint32_t array_layer,
    uint32_t x_offset,
    uint32_t y_offset,
    uint32_t width,
    uint32_t height,
    const void *data,
    uint64_t data_size,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (!texture)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "texture is NULL");
	if (RBT_STALE(texture, "rbtrend_texture2d_update", "texture"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "texture is destroyed");
	RBTREND_ENTER(texture->instance, "rbtrend_texture2d_update");
	const uint32_t size[3]   = {texture->desc.width, texture->desc.height, 1};
	const uint32_t offset[3] = {x_offset, y_offset, 0};
	const uint32_t extent[3] = {width, height, 1};
	uint32_t texel_bytes     = 0;
	uint64_t region_bytes    = 0;
	if (const RBT_RESULT result = rbtrend_texture_region_valid(queue, texture->desc.mip_levels, texture->desc.array_layers, mip_level, array_layer, size, offset,
	                                                        extent, texture->desc.format, data, data_size, &texel_bytes, &region_bytes, out_error))
		return result;
	return rbtrend_texture_upload(texture, false, queue, mip_level, array_layer, offset, extent, texel_bytes, data, out_error);
}
RBT_RESULT rbtrend_texture2d_get_data(
    rbtrend_texture2d texture,
    crgpu_command_queue queue,
    uint32_t mip_level,
    uint32_t array_layer,
    uint32_t x_offset,
    uint32_t y_offset,
    uint32_t width,
    uint32_t height,
    void *out_data,
    uint64_t data_size,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (!texture)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "texture is NULL");
	if (RBT_STALE(texture, "rbtrend_texture2d_get_data", "texture"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "texture is destroyed");
	RBTREND_ENTER(texture->instance, "rbtrend_texture2d_get_data");
	const uint32_t size[3]   = {texture->desc.width, texture->desc.height, 1};
	const uint32_t offset[3] = {x_offset, y_offset, 0};
	const uint32_t extent[3] = {width, height, 1};
	uint32_t texel_bytes     = 0;
	uint64_t region_bytes    = 0;
	if (const RBT_RESULT result = rbtrend_texture_region_valid(queue, texture->desc.mip_levels, texture->desc.array_layers, mip_level, array_layer, size, offset,
	                                                        extent, texture->desc.format, out_data, data_size, &texel_bytes, &region_bytes, out_error))
		return result;
	return rbtrend_texture_readback(texture, false, queue, mip_level, array_layer, offset, extent, texel_bytes, out_data, out_error);
}

} // extern "C"

// Shared by rbtrend_texture2d_generate_mips and RBTREND_COMMAND_GENERATE_MIPS
// (rbtrendcommands.cpp), declared in rbtrendimages_internal.hpp with C++
// linkage, so outside the extern "C" block.
RBT_RESULT rbtrend_generate_mips_valid(rbtrend_texture2d texture, uint32_t base_mip, CRGPU_FILTER filter, RBTREND_ERROR *error) {
	if (base_mip >= texture->desc.mip_levels)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "base_mip %u, and the texture has %u levels", base_mip, texture->desc.mip_levels);
	if (filter != CRGPU_FILTER_NEAREST && filter != CRGPU_FILTER_LINEAR)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "%d is not a CRGPU_FILTER", (int)filter);
	if (!(texture->desc.usage & RBTREND_TEXTURE_USAGE_BIT_SHADER_RESOURCE))
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "the texture was created without RBTREND_TEXTURE_USAGE_BIT_SHADER_RESOURCE");
	if (!(texture->desc.usage & RBTREND_TEXTURE_USAGE_BIT_RENDER_TARGET))
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "the texture was created without RBTREND_TEXTURE_USAGE_BIT_RENDER_TARGET");
	if (!rbtrend_builtin_downsample_format(texture->desc.format))
		return rbtrend_fail(error, RBT_RESULT_UNSUPPORTED, "format %d is not one every backend renders to and filters", (int)texture->desc.format);
	return RBT_RESULT_OK;
}

RBT_RESULT rbtrend_texture2d_mip_chain(
    rbtrend_texture2d texture,
    uint32_t base_mip,
    CRGPU_FILTER filter,
    const rbtrend_mip_chain **out_chain,
    rbtrend_graphics_pipeline *out_pipeline,
    RBTREND_ERROR *error) {
	rbtrend_instance instance          = texture->instance;
	rbtrend_graphics_pipeline pipeline = rbtrend_builtin_downsample_pipeline(instance, texture->desc.format);
	if (!pipeline)
		return rbtrend_fail(error, RBT_RESULT_REND_GPU_FAILURE, "Rend's downsample pipeline for format %d can't be made", (int)texture->desc.format);
	for (const rbtrend_mip_chain &chain : texture->mip_chains) {
		if (chain.base_mip == base_mip && chain.filter == filter) {
			*out_chain    = &chain;
			*out_pipeline = pipeline;
			return RBT_RESULT_OK;
		}
	}
	rbtrend_binding_layout layout = rbtrend_builtin_downsample_layout(instance);
	rbtrend_sampler sampler       = rbtrend_builtin_downsample_sampler(instance, filter);
	if (!sampler)
		return rbtrend_fail(error, RBT_RESULT_REND_GPU_FAILURE, "Rend's downsample sampler can't be made");

	const uint32_t layer_count = texture->desc.array_layers;
	const uint32_t levels      = texture->desc.mip_levels - 1 - base_mip;
	rbtrend_mip_chain chain{};
	chain.base_mip = base_mip;
	chain.filter   = filter;
	try {
		chain.steps.resize((size_t)layer_count * levels);
	} catch (...) {
		return rbtrend_fail(error, RBT_RESULT_OUT_OF_MEMORY, "no memory for the mip chain's %u levels of %u layers", levels, layer_count);
	}
	for (uint32_t layer = 0; layer < layer_count; ++layer) {
		for (uint32_t i = 0; i < levels; ++i) {
			rbtrend_mip_step &step = chain.steps[(size_t)layer * levels + i];

			// The source is a 2D array view of its one layer, as the
			// downsample shader declares it (rbtrend_downsample_frag.slang says
			// why); the target a 2D view of its layer, which no shader reads.
			RBTREND_TEXTURE_VIEW_DESC view_desc{};
			view_desc.base        = rbt_struct_base(RBT_STRUCT_TYPE_REND_TEXTURE_VIEW_DESC);
			view_desc.shape       = RBTREND_TEXTURE_VIEW_SHAPE_2D_ARRAY;
			view_desc.aspect      = RBTREND_TEXTURE_VIEW_ASPECT_COLOR;
			view_desc.mip_count   = 1;
			view_desc.base_layer  = layer;
			view_desc.layer_count = 1;
			view_desc.type        = RBTREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE;
			view_desc.base_mip    = base_mip + i;
			RBT_RESULT result       = rbtrend_texture_view_create_2d(texture, &view_desc, &step.source, error);
			if (result == RBT_RESULT_OK) {
				view_desc.shape    = RBTREND_TEXTURE_VIEW_SHAPE_2D;
				view_desc.type     = RBTREND_TEXTURE_VIEW_TYPE_RENDER_TARGET;
				view_desc.base_mip = base_mip + i + 1;
				result             = rbtrend_texture_view_create_2d(texture, &view_desc, &step.target, error);
			}
			if (result != RBT_RESULT_OK) {
				// What the view's create said, with the level in front.
				if (error) {
					char said[RBTREND_ERROR_MESSAGE_SIZE];
					memcpy(said, error->message, sizeof(said));
					rbtrend_fail(error, result, "the views of level %u of layer %u: %s", base_mip + i + 1, layer, said);
				}
				rbtrend_mip_steps_release(chain.steps, false);
				return result;
			}

			RBTREND_BINDING_SET_ENTRY entries[2]{};
			entries[0]._binding                  = 0;
			entries[0]._type                     = CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			entries[0]._resource._image._view    = step.source;
			entries[1]._binding                  = 1;
			entries[1]._type                     = CRGPU_DESCRIPTOR_TYPE_SAMPLER;
			entries[1]._resource._image._sampler = sampler;
			(void)rbtrend_binding_set_create(layout, entries, 2, &step.set, nullptr);
			if (!step.set) {
				rbtrend_mip_steps_release(chain.steps, false);
				return rbtrend_fail(error, RBT_RESULT_REND_GPU_FAILURE, "the binding set of level %u of layer %u can't be created", base_mip + i + 1, layer);
			}
		}
	}
	try {
		texture->mip_chains.push_back(std::move(chain));
	} catch (...) {
		rbtrend_mip_steps_release(chain.steps, false);
		return rbtrend_fail(error, RBT_RESULT_OUT_OF_MEMORY, "no memory to keep the mip chain");
	}
	*out_chain    = &texture->mip_chains.back();
	*out_pipeline = pipeline;
	return RBT_RESULT_OK;
}

void rbtrend_record_mip_pass(
    crgpu_command_list cmd,
    rbtrend_graphics_pipeline pipeline,
    const rbtrend_mip_step &step,
    CRGPU_FILTER filter,
    uint32_t width,
    uint32_t height) {
	rbtrend_downsample_params params{};
	params.mode[0] = filter == CRGPU_FILTER_LINEAR ? 1u : 0u;
	// Every texel is drawn, so what the level held is not loaded.
	rbtrend_record_fullscreen_pass(cmd, pipeline, step.target->image_view, width, height, CRGPU_LOAD_OP_DONT_CARE, step.set, &params, sizeof(params));
}

extern "C" {
RBT_RESULT rbtrend_texture2d_generate_mips(
    rbtrend_texture2d texture,
    crgpu_command_queue queue,
    uint32_t base_mip,
    CRGPU_FILTER filter,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (!texture)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "texture is NULL");
	if (RBT_STALE(texture, "rbtrend_texture2d_generate_mips", "texture"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "texture is destroyed");
	RBTREND_ENTER(texture->instance, "rbtrend_texture2d_generate_mips");
	if (!queue)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "queue is NULL");
	if (const RBT_RESULT result = rbtrend_generate_mips_valid(texture, base_mip, filter, out_error))
		return result;
	const uint32_t layer_count = texture->desc.array_layers;
	for (uint32_t layer = 0; layer < layer_count; ++layer)
		if (*texture->tracking.at(base_mip, layer) == CRGPU_IMAGE_LAYOUT_UNDEFINED)
			return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "level %u of layer %u has never been written", base_mip, layer);
	const uint32_t last_mip = texture->desc.mip_levels - 1;
	if (base_mip == last_mip)
		return RBT_RESULT_OK;

	// The chain is made, or found, before anything is recorded, so a failure
	// records nothing.
	const rbtrend_mip_chain *chain     = nullptr;
	rbtrend_graphics_pipeline pipeline = nullptr;
	if (const RBT_RESULT result = rbtrend_texture2d_mip_chain(texture, base_mip, filter, &chain, &pipeline, out_error))
		return result;
	const uint32_t levels = last_mip - base_mip;

	// Each level of each layer: moved into the layout the pass reads or
	// renders it in, and every level then put back as an immediate
	// operation leaves it (rbtrend_immediate_final_layout).
	const RBT_RESULT result = rbtrend_record_immediate(texture->instance, queue, out_error, [&](crgpu_command_list cmd) {
		auto move = [&](uint32_t mip, uint32_t layer, CRGPU_IMAGE_LAYOUT from, CRGPU_IMAGE_LAYOUT to) {
			if (from == to)
				return;
			crgpu_image_view_desc_subresource_range range{};
			range.base_mip_level    = mip;
			range.mip_level_count   = 1;
			range.base_array_layer  = layer;
			range.array_layer_count = 1;
			crgpu_command_list_image_barrier_subresource(cmd, texture->image, &range, from, to);
		};
		for (uint32_t layer = 0; layer < layer_count; ++layer) {
			move(base_mip, layer, *texture->tracking.at(base_mip, layer), CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY);
			for (uint32_t i = 0; i < levels; ++i) {
				const uint32_t mip = base_mip + i + 1;
				move(mip, layer, *texture->tracking.at(mip, layer), CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
				rbtrend_record_mip_pass(cmd, pipeline, chain->steps[(size_t)layer * levels + i], chain->filter, rbtrend_mip_size(texture->desc.width, mip),
				                       rbtrend_mip_size(texture->desc.height, mip));
				move(mip, layer, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY);
			}
			for (uint32_t mip = base_mip; mip <= last_mip; ++mip) {
				CRGPU_IMAGE_LAYOUT *tracked = texture->tracking.at(mip, layer);
				const CRGPU_IMAGE_LAYOUT final_layout =
				    rbtrend_immediate_final_layout(*tracked, CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY);
				move(mip, layer, CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, final_layout);
			}
		}
	});
	if (result != RBT_RESULT_OK)
		return result;
	// The batch that holds the barriers goes ahead of every later
	// submission, as rbtrend_immediate_on_texture writes its layout.
	for (uint32_t layer = 0; layer < layer_count; ++layer) {
		for (uint32_t mip = base_mip; mip <= last_mip; ++mip) {
			CRGPU_IMAGE_LAYOUT *tracked = texture->tracking.at(mip, layer);
			*tracked                      = rbtrend_immediate_final_layout(*tracked, CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY);
		}
	}
	return RBT_RESULT_OK;
}

RBT_RESULT rbtrend_texture2d_resize(
    rbtrend_texture2d texture,
    uint32_t new_width,
    uint32_t new_height,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (!texture)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "texture is NULL");
	if (RBT_STALE(texture, "rbtrend_texture2d_resize", "texture"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "texture is destroyed");
	RBTREND_ENTER(texture->instance, "rbtrend_texture2d_resize");
	if (new_width == 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "new_width is 0");
	if (new_height == 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "new_height is 0");
#if _DEBUG
	// A view left alive would be released after the old image it was made
	// of. Its mip chains' views are the texture's own, released below.
	{
		uint32_t own_views = texture->present_view ? 1u : 0u;
		for (const rbtrend_mip_chain &chain : texture->mip_chains)
			for (const rbtrend_mip_step &step : chain.steps)
				own_views += (step.source ? 1u : 0u) + (step.target ? 1u : 0u);
		if (texture->live_views > own_views)
			return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "%u views of the texture are not destroyed; destroy them first",
			                   texture->live_views - own_views);
	}
#endif
	if (new_width == texture->desc.width && new_height == texture->desc.height)
		return RBT_RESULT_OK; // No resize needed
	if (texture->desc.cube_capable && new_width != new_height)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "a cube-capable texture stays square, and %ux%u is not", new_width, new_height);

	crgpu_image_desc image_desc{};
	image_desc.base.type = CR_STRUCT_TYPE_GPU_IMAGE_DESC;
	if (const RBT_RESULT result = rbtrend_resized_image_desc(texture->image, texture->requested_mip_levels, new_width, new_height, 1, &image_desc, out_error))
		return result;

	// The old image's retire, promised before anything changes.
	if (rbtrend_instance_reserve_release(texture->instance, 1) != RBT_RESULT_OK)
		return rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory to release the old image later");
	crgpu_image new_image = nullptr;
	CR_RESULT sr           = crgpu_image_create(texture->instance->desc.device, &image_desc, &new_image);
	if (CR_FAILED(sr)) {
		rbtrend_instance_unreserve_release(texture->instance, 1);
		return rbtrend_fail_cr(out_error, sr, "creating a %ux%u image failed", new_width, new_height);
	}
	// A new image, with no contents: every mip of every layer is back to no
	// layout. Done before the old image goes, so running out of memory for
	// a different mip count still leaves the texture as it was.
	if (!rbtrend_texture_layouts_reset(&texture->tracking, image_desc.mip_levels, image_desc.array_layers)) {
		crgpu_image_destroy(new_image);
		rbtrend_instance_unreserve_release(texture->instance, 1);
		return rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory for the texture's layouts at %ux%u", new_width, new_height);
	}

	// A list compiled against the old image would barrier it and commit
	// layouts for the new one: it is refused at submit.
	rbtrend_instance_forget(texture->instance, &texture->tracking);

	// The old image goes only now that its replacement exists, so a failed
	// resize leaves the texture exactly as it was. Submitted work may still
	// be using it, so it is released when that has finished. Its mip chains
	// are views of it, retired first so they go before it.
	rbtrend_mip_chains_release(texture, true);
	rbtrend_texture_view_destroy(texture->present_view);
	texture->present_view = nullptr;
	rbtrend_instance_retire(texture->instance, texture->image, &rbtrend_release_crgpu_image);
	texture->image           = new_image;
	texture->desc.width      = new_width;
	texture->desc.height     = new_height;
	texture->desc.mip_levels = image_desc.mip_levels;
	return RBT_RESULT_OK;
}
RBT_RESULT rbtrend_texture3d_create(
    rbtrend_instance instance,
    const RBTREND_TEXTURE3D_DESC *desc,
    rbtrend_texture3d *out_texture,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (out_texture)
		*out_texture = nullptr;
	if (!instance)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "instance is NULL");
	if (RBT_STALE(instance, "rbtrend_texture3d_create", "instance"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "instance is destroyed");
	RBTREND_ENTER(instance, "rbtrend_texture3d_create");
	if (!desc)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc is NULL");
	if (const RBT_RESULT base = rbtrend_base_check(out_error, "desc", &desc->base, RBT_STRUCT_TYPE_REND_TEXTURE3D_DESC))
		return base;
	if (desc->width == 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "width is 0");
	if (desc->height == 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "height is 0");
	if (desc->depth == 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "depth is 0");
	if (desc->format == CRGPU_FORMAT_UNKNOWN)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "format is CRGPU_FORMAT_UNKNOWN");
	if (!out_texture)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "out_texture is NULL");

	rbtrend_texture3d_t *result = (rbtrend_texture3d_t *)malloc(sizeof(rbtrend_texture3d_t));
	if (!result)
		return rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory for the texture");
	result = new (result) rbtrend_texture3d_t();

	result->instance             = instance;
	result->desc                 = *desc;
	result->requested_mip_levels = desc->mip_levels;
	crgpu_image_desc image_desc{};
	image_desc.base.type    = CR_STRUCT_TYPE_GPU_IMAGE_DESC;
	image_desc.usage        = (CRGPU_IMAGE_USAGE)desc->usage | CRGPU_IMAGE_USAGE_TRANSFER_DST | CRGPU_IMAGE_USAGE_TRANSFER_SRC;
	image_desc.type         = CRGPU_IMAGE_TYPE_3D;
	image_desc.format       = desc->format;
	image_desc.width        = desc->width;
	image_desc.height       = desc->height;
	image_desc.depth        = desc->depth;
	image_desc.array_layers = 1; // 3D textures do not support array layers
	image_desc.mip_levels   = desc->mip_levels ? desc->mip_levels : rbtrend_compute_mip_levels(desc->width, desc->height, desc->depth);
	image_desc.debug_name   = desc->debug_name;

	RBT_RESULT result_code = RBT_RESULT_OK;
	CR_RESULT sr        = CR_SUCCESS;
	if (!rbtrend_texture_layouts_reset(&result->tracking, image_desc.mip_levels, 1)) {
		result_code = rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory for the texture's layouts");
		goto failedattempt;
	}
	sr = crgpu_image_create(result->instance->desc.device, &image_desc, &result->image);
	if (CR_FAILED(sr)) {
		result->image = nullptr;
		result_code   = rbtrend_fail_cr(out_error, sr, "creating a %ux%ux%u image failed", desc->width, desc->height, desc->depth);
		goto failedattempt;
	}

	// desc->mip_levels may have been 0 (meaning "auto"); record what was
	// actually allocated so rbtrend_texture3d_get_desc and later view creation
	// see the real count instead of the sentinel.
	result->desc.mip_levels = image_desc.mip_levels;

	sr = rbtrend_texture3d_create_image_view(result->image, desc->format, image_desc.mip_levels, &result->image_view);
	if (CR_FAILED(sr)) {
		result->image_view = nullptr;
		result_code        = rbtrend_fail_cr(out_error, sr, "creating the texture's view failed");
		goto failedattempt;
	}
	// Its destroy's retire, promised now so the destroy can't fail.
	if (rbtrend_instance_reserve_release(instance, 1) != RBT_RESULT_OK) {
		result_code = rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory to release the texture later");
		goto failedattempt;
	}

	*out_texture = result;
	return RBT_RESULT_OK;
failedattempt:
	result->~rbtrend_texture3d_t();
	free(result);
	return result_code;
}
void rbtrend_texture3d_destroy(rbtrend_texture3d texture) {
	if (!texture)
		return;
	if (RBT_STALE(texture, "rbtrend_texture3d_destroy", "texture"))
		return;
	// As rbtrend_texture2d_destroy.
	rbtrend_instance_forget(texture->instance, &texture->tracking);
	// Released once the GPU has finished everything submitted so far.
	RBT_KILL(texture);
	rbtrend_instance_retire(texture->instance, texture, &rbtrend_release_handle<rbtrend_texture3d_t>);
}
RBT_RESULT rbtrend_texture3d_get_desc(
    rbtrend_texture3d texture,
    RBTREND_TEXTURE3D_DESC *out_desc) {
	if (!texture)
		return RBT_RESULT_INVALID_ARGUMENT;
	if (RBT_STALE(texture, "rbtrend_texture3d_get_desc", "texture"))
		return RBT_RESULT_INVALID_ARGUMENT;
	if (!out_desc)
		return RBT_RESULT_INVALID_ARGUMENT;
	*out_desc      = texture->desc;
	out_desc->base = rbt_struct_base(RBT_STRUCT_TYPE_REND_TEXTURE3D_DESC);
	return RBT_RESULT_OK;
}
crgpu_image_view rbtrend_texture3d_get_crgpu_image_view(rbtrend_texture3d texture) {
	if (!texture)
		return NULL;
	if (RBT_STALE(texture, "rbtrend_texture3d_get_crgpu_image_view", "texture"))
		return NULL;
	return texture->image_view;
}
crgpu_image rbtrend_texture3d_get_crgpu_image(rbtrend_texture3d texture) {
	if (!texture)
		return NULL;
	if (RBT_STALE(texture, "rbtrend_texture3d_get_crgpu_image", "texture"))
		return NULL;
	return texture->image;
}
RBT_RESULT rbtrend_texture3d_update(
    rbtrend_texture3d texture,
    crgpu_command_queue queue,
    uint32_t mip_level,
    uint32_t x_offset,
    uint32_t y_offset,
    uint32_t z_offset,
    uint32_t width,
    uint32_t height,
    uint32_t depth,
    const void *data,
    uint64_t data_size,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (!texture)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "texture is NULL");
	if (RBT_STALE(texture, "rbtrend_texture3d_update", "texture"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "texture is destroyed");
	RBTREND_ENTER(texture->instance, "rbtrend_texture3d_update");
	const uint32_t size[3]   = {texture->desc.width, texture->desc.height, texture->desc.depth};
	const uint32_t offset[3] = {x_offset, y_offset, z_offset};
	const uint32_t extent[3] = {width, height, depth};
	uint32_t texel_bytes     = 0;
	uint64_t region_bytes    = 0;
	if (const RBT_RESULT result = rbtrend_texture_region_valid(queue, texture->desc.mip_levels, 1, mip_level, 0, size, offset, extent, texture->desc.format, data,
	                                                        data_size, &texel_bytes, &region_bytes, out_error))
		return result;
	return rbtrend_texture_upload(texture, true, queue, mip_level, 0, offset, extent, texel_bytes, data, out_error);
}
RBT_RESULT rbtrend_texture3d_get_data(
    rbtrend_texture3d texture,
    crgpu_command_queue queue,
    uint32_t mip_level,
    uint32_t x_offset,
    uint32_t y_offset,
    uint32_t z_offset,
    uint32_t width,
    uint32_t height,
    uint32_t depth,
    void *out_data,
    uint64_t data_size,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (!texture)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "texture is NULL");
	if (RBT_STALE(texture, "rbtrend_texture3d_get_data", "texture"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "texture is destroyed");
	RBTREND_ENTER(texture->instance, "rbtrend_texture3d_get_data");
	const uint32_t size[3]   = {texture->desc.width, texture->desc.height, texture->desc.depth};
	const uint32_t offset[3] = {x_offset, y_offset, z_offset};
	const uint32_t extent[3] = {width, height, depth};
	uint32_t texel_bytes     = 0;
	uint64_t region_bytes    = 0;
	if (const RBT_RESULT result = rbtrend_texture_region_valid(queue, texture->desc.mip_levels, 1, mip_level, 0, size, offset, extent, texture->desc.format,
	                                                        out_data, data_size, &texel_bytes, &region_bytes, out_error))
		return result;
	return rbtrend_texture_readback(texture, true, queue, mip_level, 0, offset, extent, texel_bytes, out_data, out_error);
}
RBT_RESULT rbtrend_texture3d_resize(
    rbtrend_texture3d texture,
    uint32_t new_width,
    uint32_t new_height,
    uint32_t new_depth,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (!texture)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "texture is NULL");
	if (RBT_STALE(texture, "rbtrend_texture3d_resize", "texture"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "texture is destroyed");
	RBTREND_ENTER(texture->instance, "rbtrend_texture3d_resize");
	if (new_width == 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "new_width is 0");
	if (new_height == 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "new_height is 0");
	if (new_depth == 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "new_depth is 0");
#if _DEBUG
	// As rbtrend_texture2d_resize. The view a 3D texture keeps is Carrottus GPU's,
	// not an rbtrend_texture_view, so none here is its own.
	if (texture->live_views > 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "%u views of the texture are not destroyed; destroy them first", texture->live_views);
#endif
	if (new_width == texture->desc.width && new_height == texture->desc.height && new_depth == texture->desc.depth)
		return RBT_RESULT_OK; // No resize needed

	crgpu_image_desc image_desc{};
	image_desc.base.type = CR_STRUCT_TYPE_GPU_IMAGE_DESC;
	if (const RBT_RESULT result =
	        rbtrend_resized_image_desc(texture->image, texture->requested_mip_levels, new_width, new_height, new_depth, &image_desc, out_error))
		return result;

	// The old view's and image's retires, promised before anything changes.
	if (rbtrend_instance_reserve_release(texture->instance, 2) != RBT_RESULT_OK)
		return rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory to release the old image later");
	crgpu_image new_image = nullptr;
	CR_RESULT sr           = crgpu_image_create(texture->instance->desc.device, &image_desc, &new_image);
	if (CR_FAILED(sr)) {
		rbtrend_instance_unreserve_release(texture->instance, 2);
		return rbtrend_fail_cr(out_error, sr, "creating a %ux%ux%u image failed", new_width, new_height, new_depth);
	}
	crgpu_image_view new_view = nullptr;
	sr                          = rbtrend_texture3d_create_image_view(new_image, texture->desc.format, image_desc.mip_levels, &new_view);
	if (CR_FAILED(sr)) {
		crgpu_image_destroy(new_image);
		rbtrend_instance_unreserve_release(texture->instance, 2);
		return rbtrend_fail_cr(out_error, sr, "creating the view of a %ux%ux%u image failed", new_width, new_height, new_depth);
	}
	// As rbtrend_texture2d_resize: every mip is back to no layout.
	if (!rbtrend_texture_layouts_reset(&texture->tracking, image_desc.mip_levels, 1)) {
		crgpu_image_view_destroy(new_view);
		crgpu_image_destroy(new_image);
		rbtrend_instance_unreserve_release(texture->instance, 2);
		return rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory for the texture's layouts at %ux%ux%u", new_width, new_height, new_depth);
	}

	// As rbtrend_texture2d_resize.
	rbtrend_instance_forget(texture->instance, &texture->tracking);

	// The old image and its view go only now that their replacements exist,
	// so a failed resize leaves the texture exactly as it was. Submitted
	// work may still be using them, so they are released when that has
	// finished, the view before its image.
	rbtrend_instance_retire(texture->instance, texture->image_view, &rbtrend_release_crgpu_image_view);
	rbtrend_instance_retire(texture->instance, texture->image, &rbtrend_release_crgpu_image);
	texture->image           = new_image;
	texture->image_view      = new_view;
	texture->desc.width      = new_width;
	texture->desc.height     = new_height;
	texture->desc.depth      = new_depth;
	texture->desc.mip_levels = image_desc.mip_levels;
	return RBT_RESULT_OK;
}

// What is common to a view of a 2D and of a 3D texture: the use is a view
// type the texture's usage allows, the mip range is inside [mip_levels], and
// a use that takes one mip is given one. [desc]'s mip_count is resolved from
// RBTREND_TEXTURE_VIEW_REMAINING in [out_desc].
static RBT_RESULT rbtrend_texture_view_desc_valid(
    const RBTREND_TEXTURE_VIEW_DESC *desc,
    RBTREND_TEXTURE_USAGE_BITS texture_usage,
    CRGPU_FORMAT format,
    uint32_t mip_levels,
    RBTREND_TEXTURE_VIEW_DESC *out_desc,
    RBTREND_ERROR *error) {
	const RBTREND_TEXTURE_USAGE_BITS needed_usage = rbtrend_texture_view_type_usage(desc->type);
	if (needed_usage == RBTREND_TEXTURE_USAGE_BIT_NONE)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "type %u is not a view type", (unsigned)desc->type);
	if (!(texture_usage & needed_usage))
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "a view of type %u needs usage bit %u, and the texture's usage is %u", (unsigned)desc->type,
		                   (unsigned)needed_usage, (unsigned)texture_usage);
	if (desc->base_mip >= mip_levels)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "base_mip %u is not below the texture's %u mip levels", desc->base_mip, mip_levels);
	// The aspect is one the format has. A view rendered to covers all of
	// them; a view a shader reads covers one.
	const uint64_t format_aspects = rbtrend_texture2d_view_aspect_mask(format);
	if (desc->aspect == RBTREND_TEXTURE_VIEW_ASPECT_NONE)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "aspect is RBTREND_TEXTURE_VIEW_ASPECT_NONE");
	if (desc->aspect & ~format_aspects)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "aspect %u is not in the texture's format, whose aspects are %u", (unsigned)desc->aspect,
		                   (unsigned)format_aspects);
	if (desc->type == RBTREND_TEXTURE_VIEW_TYPE_DEPTH_STENCIL && desc->aspect != format_aspects)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "a view of type DEPTH_STENCIL covers every aspect of its format (%u), and aspect is %u",
		                   (unsigned)format_aspects, (unsigned)desc->aspect);
	if (desc->type == RBTREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE && desc->aspect == RBTREND_TEXTURE_VIEW_ASPECT_DEPTH_STENCIL)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "a view of type SHADER_RESOURCE reads one aspect: RBTREND_TEXTURE_VIEW_ASPECT_DEPTH or _STENCIL");
	*out_desc = *desc;
	if (desc->mip_count == RBTREND_TEXTURE_VIEW_REMAINING)
		out_desc->mip_count = mip_levels - desc->base_mip;
	if (out_desc->mip_count == 0)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "mip_count is 0");
	if (out_desc->mip_count > mip_levels - desc->base_mip)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "%u mips from mip %u run past the texture's %u mip levels", out_desc->mip_count, desc->base_mip,
		                   mip_levels);
	// A pass renders to one mip, and a storage image slot is one mip.
	if (desc->type != RBTREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE && out_desc->mip_count != 1)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "a view of type %u covers one mip, and mip_count is %u", (unsigned)desc->type,
		                   out_desc->mip_count);
	return RBT_RESULT_OK;
}

} // extern "C"

/* The view of [texture]'s [image] that [resolved] and [cr_view_type]
 * describe, as [dimension]: made, counted, and its destroy's retire
 * promised. */
template <typename Texture>
static RBT_RESULT rbtrend_texture_view_make(
    Texture *texture,
    RBTREND_TEXTURE_DIMENSION dimension,
    const RBTREND_TEXTURE_VIEW_DESC &resolved,
    uint32_t cr_view_type,
    rbtrend_texture_view *out_view,
    RBTREND_ERROR *error) {
	rbtrend_texture_view_t *result = (rbtrend_texture_view_t *)malloc(sizeof(rbtrend_texture_view_t));
	if (!result)
		return rbtrend_fail(error, RBT_RESULT_OUT_OF_MEMORY, "no memory for the view");
	result = new (result) rbtrend_texture_view_t();

	result->instance  = texture->instance;
	result->serial    = rbtrend_instance_next_serial(texture->instance);
	result->dimension = dimension;
	result->view_type = resolved.type;
	if (dimension == RBTREND_TEXTURE_DIMENSION_2D)
		result->texture._t2d = (rbtrend_texture2d)texture;
	else
		result->texture._t3d = (rbtrend_texture3d)texture;
	result->desc = resolved;

	crgpu_image_view_desc imgvd{};
	imgvd.base.type                           = CR_STRUCT_TYPE_GPU_IMAGE_VIEW_DESC;
	imgvd.parent_image                        = texture->image;
	imgvd.type                                = cr_view_type;
	imgvd.subresource_range.aspect_mask       = resolved.aspect;
	imgvd.subresource_range.base_mip_level    = resolved.base_mip;
	imgvd.subresource_range.mip_level_count   = resolved.mip_count;
	imgvd.subresource_range.base_array_layer  = resolved.base_layer;
	imgvd.subresource_range.array_layer_count = resolved.layer_count;
	const CR_RESULT sr                       = crgpu_image_view_create(texture->image, &imgvd, &result->image_view);
	if (CR_FAILED(sr)) {
		result->image_view = nullptr;
		RBTREND_DESTRUCT__T(result, rbtrend_texture_view_t);
		return rbtrend_fail_cr(error, sr, "creating the view failed");
	}
	// Its destroy's retire, promised now so the destroy can't fail.
	if (rbtrend_instance_reserve_release(texture->instance, 1) != RBT_RESULT_OK) {
		RBTREND_DESTRUCT__T(result, rbtrend_texture_view_t);
		return rbtrend_fail(error, RBT_RESULT_OUT_OF_MEMORY, "no memory to release the view later");
	}

#if _DEBUG
	++texture->live_views;
#endif
	*out_view = result;
	return RBT_RESULT_OK;
}

extern "C" {
RBT_RESULT rbtrend_texture_view_create_2d(
    rbtrend_texture2d texture,
    const RBTREND_TEXTURE_VIEW_DESC *desc,
    rbtrend_texture_view *out_view,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (out_view)
		*out_view = nullptr;
	if (!texture)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "texture is NULL");
	if (RBT_STALE(texture, "rbtrend_texture_view_create_2d", "texture"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "texture is destroyed");
	RBTREND_ENTER(texture->instance, "rbtrend_texture_view_create_2d");
	if (!desc)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc is NULL");
	if (const RBT_RESULT base = rbtrend_base_check(out_error, "desc", &desc->base, RBT_STRUCT_TYPE_REND_TEXTURE_VIEW_DESC))
		return base;
	RBTREND_TEXTURE_VIEW_DESC resolved{};
	resolved.base = rbt_struct_base(RBT_STRUCT_TYPE_REND_TEXTURE_VIEW_DESC);
	if (const RBT_RESULT result = rbtrend_texture_view_desc_valid(desc, texture->desc.usage, texture->desc.format, texture->desc.mip_levels, &resolved, out_error))
		return result;

	const uint32_t array_layers = texture->desc.array_layers;
	if (desc->base_layer >= array_layers)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "base_layer %u is not below the texture's %u array layers", desc->base_layer, array_layers);
	if (desc->layer_count == RBTREND_TEXTURE_VIEW_REMAINING)
		resolved.layer_count = array_layers - desc->base_layer;
	if (resolved.layer_count == 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "layer_count is 0");
	if (resolved.layer_count > array_layers - desc->base_layer)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "%u layers from layer %u run past the texture's %u array layers", resolved.layer_count,
		                   desc->base_layer, array_layers);

	// The shape decides the Carrottus GPU view type and how many layers it takes.
	uint32_t cr_view_type = CRGPU_IMAGE_VIEW_TYPE_NONE;
	switch (desc->shape) {
	case RBTREND_TEXTURE_VIEW_SHAPE_2D:
		if (resolved.layer_count != 1)
			return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "a 2D view is one layer, and layer_count is %u", resolved.layer_count);
		cr_view_type = CRGPU_IMAGE_VIEW_TYPE_2D;
		break;
	case RBTREND_TEXTURE_VIEW_SHAPE_2D_ARRAY:
		cr_view_type = CRGPU_IMAGE_VIEW_TYPE_2D_ARRAY;
		break;
	case RBTREND_TEXTURE_VIEW_SHAPE_CUBE:
		if (!texture->desc.cube_capable)
			return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "a cube view of a texture that was not created cube_capable");
		if (desc->base_layer != 0)
			return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "a cube view starts at layer 0, and base_layer is %u", desc->base_layer);
		if (resolved.layer_count != 6)
			return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "a cube view is six layers, and layer_count is %u", resolved.layer_count);
		cr_view_type = CRGPU_IMAGE_VIEW_TYPE_CUBE;
		break;
	case RBTREND_TEXTURE_VIEW_SHAPE_CUBE_ARRAY:
		if (!texture->desc.cube_capable)
			return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "a cube array view of a texture that was not created cube_capable");
		if (resolved.layer_count % 6 != 0)
			return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "a cube array view is layers in sixes, and layer_count is %u", resolved.layer_count);
		if (!(texture->instance->features & CRGPU_FEATURE_CUBE_ARRAY_VIEWS))
			return rbtrend_fail(out_error, RBT_RESULT_UNSUPPORTED, "a cube array view needs a device created with CRGPU_FEATURE_CUBE_ARRAY_VIEWS");
		cr_view_type = CRGPU_IMAGE_VIEW_TYPE_CUBE_ARRAY;
		break;
	case RBTREND_TEXTURE_VIEW_SHAPE_3D:
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "a 3D view of a 2D texture");
	default:
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "shape %u is not a view shape", (unsigned)desc->shape);
	}
	// A pass renders to one layer through a 2D view, or to every layer of a
	// 2D array view at once (a layered pass). A cube is rendered to as the 2D
	// array its layers are: no graphics API has a cube render target.
	const bool is_target = desc->type == RBTREND_TEXTURE_VIEW_TYPE_RENDER_TARGET || desc->type == RBTREND_TEXTURE_VIEW_TYPE_DEPTH_STENCIL;
	if (is_target && desc->shape != RBTREND_TEXTURE_VIEW_SHAPE_2D && desc->shape != RBTREND_TEXTURE_VIEW_SHAPE_2D_ARRAY)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "a view rendered to is RBTREND_TEXTURE_VIEW_SHAPE_2D or _2D_ARRAY, and shape is %u",
		                   (unsigned)desc->shape);
	if (is_target && desc->shape == RBTREND_TEXTURE_VIEW_SHAPE_2D_ARRAY && !(texture->instance->features & CRGPU_FEATURE_LAYERED_RENDERING))
		return rbtrend_fail(out_error, RBT_RESULT_UNSUPPORTED, "a 2D array view rendered to needs a device created with CRGPU_FEATURE_LAYERED_RENDERING");
	if (!out_view)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "out_view is NULL");

	return rbtrend_texture_view_make(texture, RBTREND_TEXTURE_DIMENSION_2D, resolved, cr_view_type, out_view, out_error);
}
RBT_RESULT rbtrend_texture_view_create_3d(
    rbtrend_texture3d texture,
    const RBTREND_TEXTURE_VIEW_DESC *desc,
    rbtrend_texture_view *out_view,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (out_view)
		*out_view = nullptr;
	if (!texture)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "texture is NULL");
	if (RBT_STALE(texture, "rbtrend_texture_view_create_3d", "texture"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "texture is destroyed");
	RBTREND_ENTER(texture->instance, "rbtrend_texture_view_create_3d");
	if (!desc)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc is NULL");
	if (const RBT_RESULT base = rbtrend_base_check(out_error, "desc", &desc->base, RBT_STRUCT_TYPE_REND_TEXTURE_VIEW_DESC))
		return base;
	RBTREND_TEXTURE_VIEW_DESC resolved{};
	resolved.base = rbt_struct_base(RBT_STRUCT_TYPE_REND_TEXTURE_VIEW_DESC);
	if (const RBT_RESULT result = rbtrend_texture_view_desc_valid(desc, texture->desc.usage, texture->desc.format, texture->desc.mip_levels, &resolved, out_error))
		return result;
	if (desc->type == RBTREND_TEXTURE_VIEW_TYPE_RENDER_TARGET)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "a 3D texture can't be rendered to through a view");
	if (desc->type == RBTREND_TEXTURE_VIEW_TYPE_DEPTH_STENCIL)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "a 3D texture can't be a depth target");
	if (desc->shape != RBTREND_TEXTURE_VIEW_SHAPE_3D)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "a view of a 3D texture is RBTREND_TEXTURE_VIEW_SHAPE_3D, and shape is %u", (unsigned)desc->shape);
	// A 3D texture has one layer.
	if (desc->base_layer != 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "base_layer is %u, and a 3D texture has only layer 0", desc->base_layer);
	if (desc->layer_count != 1 && desc->layer_count != RBTREND_TEXTURE_VIEW_REMAINING)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "layer_count is %u, and a 3D texture has one layer", desc->layer_count);
	if (!out_view)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "out_view is NULL");
	resolved.base_layer  = 0;
	resolved.layer_count = 1;

	return rbtrend_texture_view_make(texture, RBTREND_TEXTURE_DIMENSION_3D, resolved, CRGPU_IMAGE_VIEW_TYPE_3D, out_view, out_error);
}
void rbtrend_texture_view_destroy(rbtrend_texture_view view) {
	if (!view)
		return;
	if (RBT_STALE(view, "rbtrend_texture_view_destroy", "view"))
		return;
#if _DEBUG
	// Counted off at the caller's destroy, not at the release: the contract
	// is about the order of the calls.
	if (view->dimension == RBTREND_TEXTURE_DIMENSION_2D)
		--view->texture._t2d->live_views;
	else
		--view->texture._t3d->live_views;
#endif
	// A compiled list that names it is refused at submit.
	rbtrend_instance_forget(view->instance, view);
	// Released once the GPU has finished everything submitted so far.
	RBT_KILL(view);
	rbtrend_instance_retire(view->instance, view, &rbtrend_release_handle<rbtrend_texture_view_t>);
}
RBTREND_TEXTURE_VIEW_TYPE rbtrend_texture_view_get_type(rbtrend_texture_view view) {
	if (!view)
		return RBTREND_TEXTURE_VIEW_TYPE_NONE;
	if (RBT_STALE(view, "rbtrend_texture_view_get_type", "view"))
		return RBTREND_TEXTURE_VIEW_TYPE_NONE;
	return view->view_type;
}
RBT_RESULT rbtrend_texture_view_get_desc(
    rbtrend_texture_view view,
    RBTREND_TEXTURE_VIEW_DESC *out_desc) {
	if (!view)
		return RBT_RESULT_INVALID_ARGUMENT;
	if (RBT_STALE(view, "rbtrend_texture_view_get_desc", "view"))
		return RBT_RESULT_INVALID_ARGUMENT;
	if (!out_desc)
		return RBT_RESULT_INVALID_ARGUMENT;
	*out_desc      = view->desc;
	out_desc->base = rbt_struct_base(RBT_STRUCT_TYPE_REND_TEXTURE_VIEW_DESC);
	return RBT_RESULT_OK;
}
RBTREND_TEXTURE_DIMENSION rbtrend_texture_view_get_dimension(rbtrend_texture_view view) {
	if (!view)
		return RBTREND_TEXTURE_DIMENSION_1D;
	if (RBT_STALE(view, "rbtrend_texture_view_get_dimension", "view"))
		return RBTREND_TEXTURE_DIMENSION_1D;
	return view->dimension;
}
crgpu_image_view rbtrend_texture_view_get_crgpu_image_view(rbtrend_texture_view view) {
	if (!view)
		return nullptr;
	if (RBT_STALE(view, "rbtrend_texture_view_get_crgpu_image_view", "view"))
		return nullptr;
	return view->image_view;
}
rbtrend_texture2d rbtrend_texture_view_get_texture2d(rbtrend_texture_view view) {
	if (!view)
		return nullptr;
	if (RBT_STALE(view, "rbtrend_texture_view_get_texture2d", "view"))
		return nullptr;
	return view->dimension == RBTREND_TEXTURE_DIMENSION_2D ? view->texture._t2d : nullptr;
}

RBT_RESULT rbtrend_sampler_create(
    rbtrend_instance instance,
    const RBTREND_SAMPLER_DESC *desc,
    rbtrend_sampler *out_sampler,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (out_sampler)
		*out_sampler = nullptr;
	if (!instance)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "instance is NULL");
	if (RBT_STALE(instance, "rbtrend_sampler_create", "instance"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "instance is destroyed");
	RBTREND_ENTER(instance, "rbtrend_sampler_create");
	if (!desc)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc is NULL");
	if (const RBT_RESULT base = rbtrend_base_check(out_error, "desc", &desc->base, RBT_STRUCT_TYPE_REND_SAMPLER_DESC))
		return base;
	if (!out_sampler)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "out_sampler is NULL");

	RBTREND_MALLOC__T(result, rbtrend_sampler_t);
	if (!result)
		return rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory for the sampler");
	RBTREND_CONSTRUCT__T(result, rbtrend_sampler_t);
	result->instance = instance;
	result->serial   = rbtrend_instance_next_serial(instance);
	result->desc     = *desc;

	crgpu_sampler_desc sd{};
	sd.base.type      = CR_STRUCT_TYPE_GPU_SAMPLER_DESC;
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
	sd.compare_enable = desc->compare_enable;
	sd.compare_op     = desc->compare_op;
	sd.debug_name     = desc->debug_name;

	const CR_RESULT sr = crgpu_sampler_create(instance->desc.device, &sd, &result->sampler);
	if (CR_FAILED(sr)) {
		RBTREND_DESTRUCT__T(result, rbtrend_sampler_t);
		return rbtrend_fail_cr(out_error, sr, "creating the sampler failed");
	}
	// Its destroy's retire, promised now so the destroy can't fail.
	if (rbtrend_instance_reserve_release(instance, 1) != RBT_RESULT_OK) {
		RBTREND_DESTRUCT__T(result, rbtrend_sampler_t);
		return rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory to release the sampler later");
	}
	*out_sampler = result;
	return RBT_RESULT_OK;
}
void rbtrend_sampler_destroy(rbtrend_sampler sampler) {
	if (!sampler)
		return;
	if (RBT_STALE(sampler, "rbtrend_sampler_destroy", "sampler"))
		return;
	// A compiled list whose sets hold it is refused at submit.
	rbtrend_instance_forget(sampler->instance, sampler);
	// Released once the GPU has finished everything submitted so far.
	RBT_KILL(sampler);
	rbtrend_instance_retire(sampler->instance, sampler, &rbtrend_release_handle<rbtrend_sampler_t>);
}
RBT_RESULT rbtrend_sampler_get_desc(
    rbtrend_sampler sampler,
    RBTREND_SAMPLER_DESC *out_desc) {
	if (!sampler)
		return RBT_RESULT_INVALID_ARGUMENT;
	if (RBT_STALE(sampler, "rbtrend_sampler_get_desc", "sampler"))
		return RBT_RESULT_INVALID_ARGUMENT;
	if (!out_desc)
		return RBT_RESULT_INVALID_ARGUMENT;
	*out_desc      = sampler->desc;
	out_desc->base = rbt_struct_base(RBT_STRUCT_TYPE_REND_SAMPLER_DESC);
	return RBT_RESULT_OK;
}
crgpu_sampler rbtrend_sampler_get_crgpu_sampler(rbtrend_sampler sampler) {
	if (!sampler)
		return nullptr;
	if (RBT_STALE(sampler, "rbtrend_sampler_get_crgpu_sampler", "sampler"))
		return nullptr;
	return sampler->sampler;
}
}
