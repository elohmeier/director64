#ifndef DIRECTOR64_N64_MEMORY_BUDGET_H
#define DIRECTOR64_N64_MEMORY_BUDGET_H

/* The permanent binary has a separate 2 MiB validator cap. Reserve the two
 * actual 640x480 RGBA16 framebuffers, 512 KiB for SDK queues/audio/stack, and
 * at least 1 MiB for loaded overlays/assets. This is a necessary boot bound,
 * not a claim that every scene fits that final dynamic allowance. */
#define N64_RDRAM_BYTES (8u * 1024u * 1024u)
#define N64_STATIC_BUDGET (2u * 1024u * 1024u)
#define N64_FRAMEBUFFER_BYTES (2u * 640u * 480u * 2u)
#define N64_SERVICE_RESERVE (512u * 1024u)
#define N64_MEDIA_RESERVE (1024u * 1024u)
#define N64_RUNTIME_BYTES (sizeof(lv_runtime_t) + sizeof(dg_runtime_t) + 16384u)
#define N64_FIXED_RESERVE                                                      \
  (N64_FRAMEBUFFER_BYTES + N64_SERVICE_RESERVE + N64_MEDIA_RESERVE)
_Static_assert(N64_STATIC_BUDGET + N64_RUNTIME_BYTES + N64_FIXED_RESERVE <=
                   N64_RDRAM_BYTES,
               "N64 runtime pools leave insufficient RDRAM for engine, "
               "display, audio and media");

#if defined(__mips__)
/* Absolute ELF symbols let the ROM validator report the linked profile's
 * actual structure sizes, independently of host compiler alignment. */
static inline void n64_export_memory_budget(void) {
  __asm__ volatile(".globl director64_runtime_bytes\n"
                   ".equ director64_runtime_bytes, %0\n"
                   ".globl director64_fixed_reserve\n"
                   ".equ director64_fixed_reserve, %1\n"
                   :
                   : "i"(N64_RUNTIME_BYTES), "i"(N64_FIXED_RESERVE));
}
#endif
#endif
