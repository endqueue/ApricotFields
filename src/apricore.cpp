
#include "apricore.h"

#if __cplusplus
extern "C" {
#endif

uint32_t apri_get_version(void) {
	/* Compiled into the library, so this is the library's version, not
	 * the version of the header the caller has. */
	return APRICOTFIELDS_VERSION;
}

void apri_debug_name_set(void *object, const char *name) {
	if (object) {
		/* Store the debug name in the first member of the object struct. */
		char **name_ptr = (char **)object;
		*name_ptr       = (char *)name;
	}
}

const char *apri_debug_name_get(void *object) {
	if (object) {
		/* Get the debug name from the first member of the object struct. */
		char **name_ptr = (char **)object;
		return *name_ptr;
	} else
		return nullptr;
}

#if __cplusplus
}
#endif
