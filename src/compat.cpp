/* compat.cpp — Runtime stubs for symbols missing from the ER-301 firmware ABI.
 *
 * The ER-301 firmware exports sinf and cosf but NOT sincosf.
 * GCC with -ffast-math may fuse sinf+cosf calls into sincosf — this stub
 * prevents an unresolved symbol at load time.
 *
 * Compiled with -O1 and WITHOUT -ffast-math so GCC does not recursively
 * fuse the sinf/cosf calls inside this stub back into sincosf.
 *
 * fprintf / stderr: referenced by SWIG-generated Lua error paths.
 * The ER-301 has no libc FILE* I/O; these stubs silence the references.
 */

#include <cmath>
#include <cstdio>
#include <cstdarg>

extern "C" {

void sincosf(float x, float *s, float *c)
{
    *s = sinf(x);
    *c = cosf(x);
}

// Minimal fprintf stub — swallows all output silently on the bare-metal target.
int fprintf(FILE * /*stream*/, const char * /*fmt*/, ...)
{
    return 0;
}

} // extern "C"
