#ifndef APRICOT_MATH_H
#define APRICOT_MATH_H

/* Shared math types for all Apricot modules.
 * Plain C structs — memory-layout compatible with GLM types in C++ backends.
 * Conversion in C++ is a zero-cost reinterpret_cast. */

/* Frozen: a value, laid out as its GLM counterpart. It never changes; a new need gets a new type. */
typedef struct { float  x, y;          } ApriVec2;
/* Frozen: a value, laid out as its GLM counterpart. It never changes; a new need gets a new type. */
typedef struct { float  x, y, z;       } ApriVec3;
/* Frozen: a value, laid out as its GLM counterpart. It never changes; a new need gets a new type. */
typedef struct { float  x, y, z, w;    } ApriVec4;
/* Frozen: a value, laid out as its GLM counterpart. It never changes; a new need gets a new type. */
typedef struct { double x, y, z;       } ApriDVec3;
/* Frozen: a value, laid out as its GLM counterpart. It never changes; a new need gets a new type. */
typedef struct { double x, y, z, w;    } ApriDVec4;
/* Frozen: a value, laid out as its GLM counterpart. It never changes; a new need gets a new type. */
typedef struct { float  x, y, z, w;    } ApriQuat;
/* Frozen: a value, laid out as its GLM counterpart. It never changes; a new need gets a new type. */
typedef struct { float  m[16];         } ApriMat4;   /* column-major */
/* Frozen: a value, laid out as its GLM counterpart. It never changes; a new need gets a new type. */
typedef struct { double m[16];         } ApriDMat4;  /* column-major */

#endif /* APRICOT_MATH_H */
