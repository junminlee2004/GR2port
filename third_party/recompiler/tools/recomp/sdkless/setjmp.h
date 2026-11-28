/* Stand-in for the recompiled library's build without an SDK (build_library.py): jmp_buf and
 * _setjmp, for RcResume (src/recomp/rc.h) and the generated code's calls of setjmp. jmp_buf has
 * the size and alignment of the C library's: the runtime, built with the real header, longjmps to
 * it and reads RcResume's fields after it. */
#pragma once
#if defined(__APPLE__) && defined(__aarch64__)
typedef int jmp_buf[(14 + 8 + 2) * 2];
#elif defined(__APPLE__) && defined(__x86_64__)
typedef int jmp_buf[(9 * 2) + 3 + 16];
#else
#error "sdkless/setjmp.h: no jmp_buf for this target (an SDK build uses the C library's header)"
#endif
int _setjmp(jmp_buf env);
