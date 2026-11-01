#include "render/aprendraytracing.h"
#include "aprendraytracing_internal.hpp"

extern "C" {

APRESULT aprend_blas_create(aprend_instance instance, const APREND_BLAS_DESC *desc, aprend_blas *out_blas,
                            APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (out_blas)
		*out_blas = nullptr;
	if (!instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is NULL");
	if (APRI_STALE(instance, "aprend_blas_create", "instance"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is destroyed");
	APREND_ENTER(instance, "aprend_blas_create");
	if (!desc)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is NULL");
	if (const APRESULT base = aprend_base_check(out_error, "desc", &desc->base, APRI_STRUCT_TYPE_APREND_BLAS_DESC))
		return base;
	if (!out_blas)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_blas is NULL");
	if (!aprend_rt_supported(instance))
		return aprend_fail(out_error, APRESULT_UNSUPPORTED, "aprend_blas_create: " APREND_RT_UNSUPPORTED_MESSAGE);
	return aprend_fail(out_error, APRESULT_NOT_IMPLEMENTED, "aprend_blas_create is not implemented yet");
}

void aprend_blas_destroy(aprend_blas blas) {
	if (!blas)
		return;
	if (APRI_STALE(blas, "aprend_blas_destroy", "blas"))
		return;
}

uint32_t aprend_blas_get_slot(aprend_blas blas) {
	if (!blas)
		return 0;
	if (APRI_STALE(blas, "aprend_blas_get_slot", "blas"))
		return 0;
	APREND_ENTER_BLAS(blas, "aprend_blas_get_slot");
	return 0;
}

APRESULT aprend_blas_get_info(aprend_blas blas, APREND_BLAS_INFO *out_info, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!blas)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "blas is NULL");
	if (APRI_STALE(blas, "aprend_blas_get_info", "blas"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "blas is destroyed");
	APREND_ENTER_BLAS(blas, "aprend_blas_get_info");
	if (!out_info)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_info is NULL");
	if (const APRESULT base = aprend_base_out_check(out_error, "out_info", &out_info->base, APRI_STRUCT_TYPE_APREND_BLAS_INFO))
		return base;
	return aprend_fail(out_error, APRESULT_NOT_IMPLEMENTED, "aprend_blas_get_info is not implemented yet");
}

APRESULT aprend_tlas_create(aprend_instance instance, const APREND_TLAS_DESC *desc, aprend_tlas *out_tlas,
                            APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (out_tlas)
		*out_tlas = nullptr;
	if (!instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is NULL");
	if (APRI_STALE(instance, "aprend_tlas_create", "instance"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is destroyed");
	APREND_ENTER(instance, "aprend_tlas_create");
	if (!desc)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is NULL");
	if (const APRESULT base = aprend_base_check(out_error, "desc", &desc->base, APRI_STRUCT_TYPE_APREND_TLAS_DESC))
		return base;
	if (!out_tlas)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_tlas is NULL");
	if (!aprend_rt_supported(instance))
		return aprend_fail(out_error, APRESULT_UNSUPPORTED, "aprend_tlas_create: " APREND_RT_UNSUPPORTED_MESSAGE);
	return aprend_fail(out_error, APRESULT_NOT_IMPLEMENTED, "aprend_tlas_create is not implemented yet");
}

void aprend_tlas_destroy(aprend_tlas tlas) {
	if (!tlas)
		return;
	if (APRI_STALE(tlas, "aprend_tlas_destroy", "tlas"))
		return;
}

APRESULT aprend_tlas_write_instances(aprend_tlas tlas, uint32_t first, const APREND_RAY_INSTANCE *instances,
                                     uint32_t count, APREND_ERROR *out_error) {
	(void)first;
	(void)instances;
	(void)count;
	aprend_error_clear(out_error);
	if (!tlas)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "tlas is NULL");
	if (APRI_STALE(tlas, "aprend_tlas_write_instances", "tlas"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "tlas is destroyed");
	APREND_ENTER_TLAS(tlas, "aprend_tlas_write_instances");
	APREND_ENTER(tlas->instance, "aprend_tlas_write_instances");
	return aprend_fail(out_error, APRESULT_NOT_IMPLEMENTED, "aprend_tlas_write_instances is not implemented yet");
}

APRESULT aprend_tlas_set_instance_count(aprend_tlas tlas, uint32_t count, APREND_ERROR *out_error) {
	(void)count;
	aprend_error_clear(out_error);
	if (!tlas)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "tlas is NULL");
	if (APRI_STALE(tlas, "aprend_tlas_set_instance_count", "tlas"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "tlas is destroyed");
	APREND_ENTER_TLAS(tlas, "aprend_tlas_set_instance_count");
	return aprend_fail(out_error, APRESULT_NOT_IMPLEMENTED, "aprend_tlas_set_instance_count is not implemented yet");
}

APRESULT aprend_tlas_set_materials(aprend_tlas tlas, uint32_t first, const aprend_material *materials, uint32_t count,
                                   APREND_ERROR *out_error) {
	(void)first;
	(void)materials;
	(void)count;
	aprend_error_clear(out_error);
	if (!tlas)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "tlas is NULL");
	if (APRI_STALE(tlas, "aprend_tlas_set_materials", "tlas"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "tlas is destroyed");
	APREND_ENTER_TLAS(tlas, "aprend_tlas_set_materials");
	APREND_ENTER(tlas->instance, "aprend_tlas_set_materials");
	return aprend_fail(out_error, APRESULT_NOT_IMPLEMENTED, "aprend_tlas_set_materials is not implemented yet");
}

APRESULT aprend_tlas_get_info(aprend_tlas tlas, APREND_TLAS_INFO *out_info, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!tlas)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "tlas is NULL");
	if (APRI_STALE(tlas, "aprend_tlas_get_info", "tlas"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "tlas is destroyed");
	APREND_ENTER_TLAS(tlas, "aprend_tlas_get_info");
	if (!out_info)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_info is NULL");
	if (const APRESULT base = aprend_base_out_check(out_error, "out_info", &out_info->base, APRI_STRUCT_TYPE_APREND_TLAS_INFO))
		return base;
	return aprend_fail(out_error, APRESULT_NOT_IMPLEMENTED, "aprend_tlas_get_info is not implemented yet");
}

} // extern "C"
