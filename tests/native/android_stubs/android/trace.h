#ifndef TEST_ANDROID_TRACE_H
#define TEST_ANDROID_TRACE_H
static inline void ATrace_beginSection(const char *name) { (void)name; }
static inline void ATrace_endSection(void) {}
#endif
