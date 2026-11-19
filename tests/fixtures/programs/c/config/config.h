/* Build options decided by the preprocessor. With no -D flags: HAVE_FAST_COPY is on, so
   buf_copy() is fast_copy(); TRACE_ON is off, so TRACE() logs instead of tracing. */
#ifndef CONFIG_H
#define CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__STDC__) && !defined(NO_FAST_COPY)
#  define HAVE_FAST_COPY
#endif

void fast_copy(char *dst, const char *src, int n);
void log_line(const char *msg);
int checksum(const char *s, int mode);

#ifdef HAVE_FAST_COPY
#  define buf_copy fast_copy
#else
void buf_copy(char *dst, const char *src, int n);
#endif

#ifdef TRACE_ON
void trace(const char *msg);
#  define TRACE(msg) trace(msg)
#else
#  define TRACE(msg) log_line(msg)
#endif

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_H */
