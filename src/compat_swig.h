/* compat_swig.h — fprintf no-op for SWIG-generated wrapper compilation.
 *
 * IMPORTANT: This file is NOT passed as -include to the compiler.
 * Doing so would define fprintf as a macro, conflicting with
 * `using ::fprintf` inside <cstdio> and causing a compile error.
 *
 * Instead, the actual fprintf stub lives in compat.cpp and is resolved
 * at partial-link (-r) time.  This file is retained for documentation
 * only and is not referenced by the Makefile build rules.
 */

// #define fprintf(f, ...) ((void)0)   ← DO NOT ENABLE — see note above
