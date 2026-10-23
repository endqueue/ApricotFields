#ifndef APRIPLAINDATA_HPP
#define APRIPLAINDATA_HPP

// Data is plain structs, checked by the compiler. The element of an array
// that holds per-command, per-draw or per-element data is trivially
// copyable and standard layout: the array is bytes, copied, moved and
// walked as bytes, with no constructor, destructor or hidden pointer per
// element. APRI_PLAIN_DATA(T) goes beside each such array's element type;
// the workspace's ./check.sh plain-data finds the arrays and fails on one
// whose element has no assert and no check.baseline entry.
//
// APRI_HOT_RECORD(T, bytes) is a size budget on a record a per-frame loop
// reads, in cache lines or a fraction of one, so a field added to it is a
// decision made by changing the budget, not a side effect.

#include <type_traits>

#define APRI_CACHE_LINE 64

#define APRI_PLAIN_DATA(T)                                                        \
    static_assert(std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>, \
                  #T " is an array's element: data is plain structs (trivially copyable, standard layout)")

#define APRI_HOT_RECORD(T, BYTES)                                                  \
    static_assert(sizeof(T) <= (BYTES), #T " is a hot record over its size budget of " #BYTES \
                                        " bytes: growing it is a decision, made by changing the budget")

#endif // APRIPLAINDATA_HPP
