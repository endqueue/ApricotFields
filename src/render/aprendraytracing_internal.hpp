#ifndef APREND_RAYTRACING_INTERNAL_HPP
#define APREND_RAYTRACING_INTERNAL_HPP

/* Internal header: never included outside ApricotFields/src/render/. The
 * structs behind aprend_blas and aprend_tlas (aprendraytracing.h). */

#include "aprend_internal.hpp"
#include "render/aprendraytracing.h"

/* What every ray tracing call and command checks first: the device was
 * created with SPUDGPU_FEATURE_RAY_QUERY. */
static inline bool aprend_rt_supported(aprend_instance instance) {
	return (instance->features & SPUDGPU_FEATURE_RAY_QUERY) != 0;
}

/* The message of every refusal for a device without ray queries, after the
 * call's name. */
#define APREND_RT_UNSUPPORTED_MESSAGE "the device was not created with SPUDGPU_FEATURE_RAY_QUERY"

/* The name of one of ray tracing's four commands, for a message. */
static inline const char *aprend_rt_command_name(APREND_COMMAND_TYPE type) {
	switch (type) {
	case APREND_COMMAND_BUILD_BLAS:
		return "BUILD_BLAS";
	case APREND_COMMAND_BUILD_TLAS:
		return "BUILD_TLAS";
	case APREND_COMMAND_LIT_SHADOW_MASK:
		return "LIT_SHADOW_MASK";
	case APREND_COMMAND_LIT_PATH_TRACE:
		return "LIT_PATH_TRACE";
	default:
		return "a command";
	}
}

typedef struct aprend_blas_t {
	char *debug_name{nullptr};
	APRI_STALE_TAG
	aprend_blas_t() = default;
	~aprend_blas_t() = default;
	aprend_instance instance{nullptr};
	/* Debug builds' check of the rule in aprendcontext.h ("Threads"), one
	 * caller to the BLAS (APREND_ENTER_BLAS). In every build, so the struct
	 * is the same in each. */
	apri_overlap entered{};
} aprend_blas_t;

typedef struct aprend_tlas_t {
	char *debug_name{nullptr};
	APRI_STALE_TAG
	aprend_tlas_t() = default;
	~aprend_tlas_t() = default;
	aprend_instance instance{nullptr};
	/* As aprend_blas_t's. */
	apri_overlap entered{};
} aprend_tlas_t;

#define APREND_ENTER_BLAS(blas, who) APRI_ENTER((blas)->entered, "aprend", who, "blas")
#define APREND_ENTER_TLAS(tlas, who) APRI_ENTER((tlas)->entered, "aprend", who, "tlas")

#endif // APREND_RAYTRACING_INTERNAL_HPP
