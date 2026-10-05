/* ========================================================================
 *
 * n64.ld -- Libdragon linker script
 *
 * Originally based on n64ld.x, developed by Frank Somers <frank@g0dev.com>,
 * with modifications by hcs (halleyscometsoftware@hotmail.com),
 * released under the Unlicense.
 * ========================================================================
 */

/* Director64: libdragon's n64.ld (third_party/libdragon/n64.ld, the pinned
 * revision the toolchain image installs) with the read-only data, the
 * initialised data and the heap start each placed at a fixed footing in the
 * 8 KiB period of the console's direct-mapped data cache. Where a section
 * starts modulo that period decides which of its objects share cache lines
 * with the main stack's hottest frames and with the runtime's structures,
 * and without the alignment every change to the code's size moved every
 * static: the same runtime measured 16% slower per Lingo step on Mucklas's
 * train scene after a 768-byte code change put the interrupt and timing
 * statics on the stack's lines (2026-09-27, docs/roadmap.md, Track B). The
 * structures the runtime allocates itself are placed the same way
 * (platforms/n64/director_main.c, NATIVE_LAYOUT). Diff against the
 * libdragon copy when the submodule moves; the Director64 additions are
 * the comments and lines marked below and the assertion at the end.
 */

OUTPUT_FORMAT ("elf32-bigmips", "elf32-bigmips", "elf32-littlemips")
OUTPUT_ARCH (mips)
EXTERN (_start)
ENTRY (_start)

PHDRS
{
    irq PT_LOAD AT ( 0x80000000 );
    main PT_LOAD AT ( 0x80000400 );
}

SECTIONS {
    .intvectors 0x80000000 : {
        . = ALIGN(32);
        KEEP(*(.intvectors))
        __intvectors_end = .;
    } :irq

    /* The text section carries the app code and its relocation addr is
    * the first byte of the cart domain in cached, unmapped memory
    */
    .text 0x80000400 : {
        __text_start = .;
        *(.boot)
        . = ALIGN(16);
        *(.text)
        *(.text.*)
        KEEP(*(keep.text.*))
        *(.init)
        *(.fini)
        *(.gnu.linkonce.t.*)
        . = ALIGN(16);
        __text_end  = .;
    } :main

   .eh_frame_hdr : { *(.eh_frame_hdr) }
   .eh_frame : { 
        __EH_FRAME_BEGIN__ = .; /* Define symbol for accessing eh_frame section */
        KEEP (*(.eh_frame)) 
    }
   .gcc_except_table : { *(.gcc_except_table*) } 
   .jcr : { KEEP (*(.jcr)) } 

    /* Director64: the read-only data keeps a fixed footing in the cache
       period, so the runtime's tables land on the same sets whatever the
       code size. */
    . = ALIGN(8192);

    .rodata : {
        *(.rdata)
        *(.rodata)
        *(.rodata.*)
        KEEP(*(keep.rodata.*))
        *(.gnu.linkonce.r.*)
        . = ALIGN(4);
        __tdata_align = .;
        LONG (ALIGNOF(.tdata));
        . = ALIGN(8);
    }

    /* This is important to keep __CTOR_END__ consistent */
    . = ALIGN(4);

    .ctors : {
        __CTOR_LIST__ = .;
        /* Actually we should place the count here like;

        LONG((__CTOR_END__ - __CTOR_LIST__) / 4 - 2)

        but g++ seem to already do this (though incorrectly placing an 0xFFFFFFFF)
        and that breaks things if we also add it. Ideally this should be parsed
        by the code but we already know the end as well.

        The new build system links everything via g++ by default and should use
        __wrap___do_global_ctors and enables it via the --wrap linker option.
        When linking with ld, we should use __do_global_ctors, which is the default
        if you don't provide the --wrap option. */
        KEEP(*(EXCLUDE_FILE (*crtend.o) .ctors))
        KEEP(*(SORT(.ctors.*)))
        KEEP(*crtend.o(.ctors))
         /* Similarly we should have a;

        LONG(0)

        here, and g++ seem to insert it at __CTOR_END__ */
        __CTOR_END__ = .;
    }

    . = ALIGN(8);

    /* Data section has relocation address at start of RAM in cached,
    * unmapped memory, but is loaded just at the end of the text segment,
    * and must be copied to the correct location at startup
    * Gather all initialised data together.  The memory layout
    * will place the global initialised data at the lowest addrs.
    * The lit8, lit4, sdata and sbss sections have to be placed
    * together in that order from low to high addrs with the _gp symbol
    * positioned (aligned) at the start of the sdata section.
    * We then finish off with the standard bss section
    */
    /* Director64: the initialised data starts 0x1900 into a period. The
       small scalars of every object cluster around the .data/.bss boundary
       (the interrupt depth, tick base and frame counters every interrupt
       touches, and the runtime's cost counters), and this footing keeps
       that cluster across the period boundary, 0x1c00..0x2200, where the
       emulator's miss profile shows the fewest other hot lines: the main
       stack's hottest frames own 0x1300..0x1900, the interpreter's header
       0x800 and the score's 0x1000 (director_main.c). The assertion at the
       end of this script holds the cluster clear of the stack. */
    . = ALIGN(8192) + 0x1900;

    .data : {
        __data_start = .;
        *(.data)
        *(.data.*)
        KEEP(*(keep.data.*))
        *(.gnu.linkonce.d.*)
        . = ALIGN(8);
    }
    
    .tdata : {
        __tls_base = .;
        __tdata_start = .;
        *(.tdata)
        *(.tdata.*)
        *(.gnu.linkonce.td.*)
        __tdata_end = .;
    }
    
    .tbss : {
        __tbss_start = .;
        *(.tbss)
        *(.tbss.*)
        *(.gnu.linkonce.tb.*)
        . = ALIGN(8);
        __tbss_end = .;
        __tls_end = .;
    }
    
    /* Fix dot for TBSS Sections */
    . = .+(__tbss_end-__tbss_start);
    
    /* Small data START */

    .sdata : {
        _gp = . + 0x8000;
        *(.sdata)
        *(.sdata.*)
        *(.gnu.linkonce.s.*)
        . = ALIGN(8);
    }

    .lit8 : {
        *(.lit8)
        . = ALIGN(8);
    }
    .lit4 : {
        *(.lit4)
        . = ALIGN(8);
    }

    . = ALIGN(8);
    __data_end = .;

    /* Here the ROM is finished. The rest is just in RAM */
    . = ALIGN(8);
    __rom_end = .;

    .sbss : {
         __bss_start = .;
        *(.sbss)
        *(.sbss.*)
        *(.gnu.linkonce.sb.*)
        *(.scommon)
        *(.scommon.*)
    }

    /* Small data END */

    . = ALIGN(8);
    .bss : {
        *(.bss)
        *(.bss*)
        *(.gnu.linkonce.b.*)
        *(COMMON)
        . = ALIGN(8);
        __th_tdata_copy = .;
        . = .+(__tdata_end-__tdata_start);
        /* Director64: the heap starts on a period boundary, so the buffers
           the console allocates at boot land on the same sets in every
           build. */
        . = ALIGN(8192);
         __bss_end = .;
    }

    . = ALIGN(8);

    /* Deprecated */
    end = .;
}

/* Give an assert for a too huge binary. The maximum allowed size here is 8 MiB,
   minus the space for the stack and the initial low memory section. 
   Notice the exact size doesn't matter much: we want to detect mistakes that
   would be much harder to find at runtime when no heap is available... */
ASSERT(end - __text_start <= 8*1024*1024 - 64*1024 - 1024, "ERROR: the generated ELF is too big for N64 memory")

/* Director64: the hot static cluster spans about 0x100 bytes before the
   .data/.bss boundary to 0x500 after it, and must not share the period's
   0x1300..0x1900 with the main stack's hottest lines. When this fails the
   statics grew or shrank past the margin: choose a new footing after the
   ALIGN before .data above, and re-measure (tools/cache-profile). */
ASSERT(((__bss_start - 0x100) & 0x1fff) + 0x600 <= 0x1300 || ((__bss_start - 0x100) & 0x1fff) >= 0x1900,
       "ERROR: Director64 hot statics share data-cache sets with the main stack; see platforms/n64/n64.ld")
