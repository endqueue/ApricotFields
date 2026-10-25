#ifndef APRICOT_MATH_H
#define APRICOT_MATH_H

/**
 * @file aprimath.h
 * @brief Shared math types for all Apricot modules.
 *
 * Plain C structs, memory-layout compatible with GLM types in C++ backends.
 * Conversion in C++ is a zero-cost reinterpret_cast.
 */

/** @brief Two floats, laid out as `glm::vec2`. */
/* Frozen: a value, laid out as its GLM counterpart. It never changes; a new need gets a new type. */
typedef struct { float  /** X. */ x, /** Y. */ y;          } ApriVec2;
/** @brief Three floats, laid out as `glm::vec3`. */
/* Frozen: a value, laid out as its GLM counterpart. It never changes; a new need gets a new type. */
typedef struct { float  /** X. */ x, /** Y. */ y, /** Z. */ z;       } ApriVec3;
/** @brief Four floats, laid out as `glm::vec4`. */
/* Frozen: a value, laid out as its GLM counterpart. It never changes; a new need gets a new type. */
typedef struct { float  /** X. */ x, /** Y. */ y, /** Z. */ z, /** W. */ w;    } ApriVec4;
/** @brief Three doubles, laid out as `glm::dvec3`. */
/* Frozen: a value, laid out as its GLM counterpart. It never changes; a new need gets a new type. */
typedef struct { double /** X. */ x, /** Y. */ y, /** Z. */ z;       } ApriDVec3;
/** @brief Four doubles, laid out as `glm::dvec4`. */
/* Frozen: a value, laid out as its GLM counterpart. It never changes; a new need gets a new type. */
typedef struct { double /** X. */ x, /** Y. */ y, /** Z. */ z, /** W. */ w;    } ApriDVec4;
/** @brief A quaternion of floats, laid out as `glm::quat`: #x, #y and #z are the vector part, #w the scalar part. */
/* Frozen: a value, laid out as its GLM counterpart. It never changes; a new need gets a new type. */
typedef struct { float  /** Vector part, x. */ x, /** Vector part, y. */ y, /** Vector part, z. */ z, /** Scalar part. */ w;    } ApriQuat;
/** @brief A 4x4 matrix of floats, laid out as `glm::mat4`. */
/* Frozen: a value, laid out as its GLM counterpart. It never changes; a new need gets a new type. */
typedef struct { float  /** Column-major: column c, row r is m[c * 4 + r]. */ m[16];         } ApriMat4;
/** @brief A 4x4 matrix of doubles, laid out as `glm::dmat4`. */
/* Frozen: a value, laid out as its GLM counterpart. It never changes; a new need gets a new type. */
typedef struct { double /** Column-major: column c, row r is m[c * 4 + r]. */ m[16];         } ApriDMat4;

#endif /* APRICOT_MATH_H */
