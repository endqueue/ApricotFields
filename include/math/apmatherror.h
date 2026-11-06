/**
 * @file apmatherror.h
 * @brief ApricotMath's per-call error message, shared by its headers.
 *
 * A call that can fail for a reason worth a sentence takes an APMATH_ERROR
 * pointer (NULL to skip it): on failure it holds a description in developer
 * English, naming the part that failed; on success its message is empty.
 * It is per call, never kept on an object, so calls that may overlap each
 * other each write only their own.
 *
 * apmathpoly.h and apmathbvh.h include it; a caller includes either.
 */

#ifndef APMATHERROR_H
#define APMATHERROR_H

#if __cplusplus
extern "C" {
#endif

/** @brief The size of APMATH_ERROR's message buffer, its NUL included. */
#define APMATH_ERROR_MESSAGE_SIZE 256

/**
 * @brief Why a call failed, filled by calls that take one.
 */
/* Frozen: filled per call. It never changes; a new need gets a new type. */
typedef struct APMATH_ERROR {
	/** NUL-terminated, truncated to fit. Empty on success. */
	char message[APMATH_ERROR_MESSAGE_SIZE];
} APMATH_ERROR;

#if __cplusplus
}
#endif

#endif // APMATHERROR_H
