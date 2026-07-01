// Layer 1 of the mimalloc integration: routes C++ global operator new/delete
// through mimalloc. Global operators must be defined once per binary, so this
// file is compiled into each executable (see recreation_enable_mimalloc).
//
// Only needed where the platform's malloc interposition does not already cover
// new/delete across module boundaries, i.e. Windows. On POSIX the statically
// linked mimalloc provides the operator override directly, so this file is a
// no-op there.
#if defined(RECREATION_MIMALLOC) && defined(_WIN32)
#include <mimalloc-new-delete.h>
#endif
