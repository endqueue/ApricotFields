/**
 * @file rbtmatherror.h
 * @brief Rabbittus Math's per-call error message, shared by its headers.
 *
 * A call that can fail for a reason worth a sentence takes an RBTMATH_ERROR
 * pointer (NULL to skip it): on failure it holds a description in developer
 * English, naming the part that failed; on success its message is empty.
 * It is per call, never kept on an object, so calls that may overlap each
 * other each write only their own.
 *
 * rbtmathpoly.h and rbtmathbvh.h include it; a caller includes either.
 */

#ifndef RBTMATHERROR_H
#define RBTMATHERROR_H

#if __cplusplus
extern "C" {
#endif

/** @brief The size of RBTMATH_ERROR's message buffer, its NUL included. */
#define RBTMATH_ERROR_MESSAGE_SIZE 256

/**
 * @brief Why a call failed, filled by calls that take one.
 */
/* Frozen: filled per call. It never changes; a new need gets a new type. */
typedef struct RBTMATH_ERROR {
	/** NUL-terminated, truncated to fit. Empty on success. */
	char message[RBTMATH_ERROR_MESSAGE_SIZE];
} RBTMATH_ERROR;

#if __cplusplus
}
#endif

#endif // RBTMATHERROR_H
