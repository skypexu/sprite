/*
 * Copyright (c) 1980 Regents of the University of California.
 * All rights reserved.  The Berkeley software License Agreement
 * specifies the terms and conditions for redistribution.
 *
 *	@(#)a.out.h	5.2 (Berkeley) 4/7/87
 */

/*
 * Definitions of the a.out header
 * and magic numbers are shared with
 * the kernel.
 */
#ifndef _AOUT
#define _AOUT

#include <sun4.md/sys/exec.h>

extern int Aout_PageSize[];

/*
 * Macro to tell whether or not the magic number in an a.out file
 * is an illegal one.
 */

#define	N_BADMAG(x) \
    (((x).a_magic)!=OMAGIC && \
     ((x).a_magic)!=NMAGIC && \
     ((x).a_magic)!=ZMAGIC && \
     ((x).a_magic)!=SPRITE_ZMAGIC && \
     ((x).a_magic)!=UNIX_ZMAGIC)

/*
 * Macros to tell where various pieces of information start in the
 * a.out file.
 */

#define N_PAGSIZ(x) (Aout_PageSize[(x).a_machtype])

#define N_TXTOFF(x) \
	((x).a_magic==ZMAGIC ? 0 : sizeof (struct exec))
#define N_SYMOFF(x) \
	(N_TXTOFF(x) + (x).a_text+(x).a_data + (x).a_trsize + (x).a_drsize)
#define	N_STROFF(x) \
	(N_SYMOFF(x) + (x).a_syms)

/*
 * Macros to tell where the various segments start in virtual memory,
 * when the process is loaded.
 */
#include <sun4.md/kernel/procMach.h>

#define N_TXTADDR(x) PROC_CODE_LOAD_ADDR(*((ProcExecHeader *) &(x)))
#define N_DATADDR(x) PROC_DATA_LOAD_ADDR(*((ProcExecHeader *) &(x)))
#define N_BSSADDR(x) PROC_BSS_LOAD_ADDR(*((ProcExecHeader *) &(x)))

/*
 * Sparc relocation types
 */

enum reloc_type
{
        RELOC_8,        RELOC_16,       RELOC_32,       /* simplest relocs    */
        RELOC_DISP8,    RELOC_DISP16,   RELOC_DISP32,   /* Disp's (pc-rel)    */
        RELOC_WDISP30,  RELOC_WDISP22,                  /* SR word disp's     */
        RELOC_HI22,     RELOC_22,                       /* SR 22-bit relocs   */
        RELOC_13,       RELOC_LO10,                     /* SR 13&10-bit relocs*/
        RELOC_SFA_BASE, RELOC_SFA_OFF13,                /* SR S.F.A. relocs   */
        RELOC_BASE10,   RELOC_BASE13,   RELOC_BASE22,   /* base_relative pic */
        RELOC_PC10,     RELOC_PC22,                     /* special pc-rel pic*/
        RELOC_JMP_TBL,                                  /* jmp_tbl_rel in pic */
        RELOC_SEGOFF16,                                 /* ShLib offset-in-seg*/
        RELOC_GLOB_DAT, RELOC_JMP_SLOT, RELOC_RELATIVE, /* rtld relocs        */
};

/*
 * Format of a relocation datum.
 */

struct reloc_info_sparc /* used when header.a_machtype == M_SPARC */
{
        unsigned long int r_address;    /* address which is relocated         */
        unsigned int    r_index   :24;  /* segment index or symbol index      */
        unsigned int    r_extern  : 1;  /* if F, r_index==SEG#; if T, SYM idx */
        int                       : 2;  /* <unused>                           */
        enum reloc_type r_type    : 5;  /* type of relocation to perform      */
        long int        r_addend;       /* addend for relocation value        */
}; 


#define	relocation_info reloc_info_sparc
#if 0
struct relocation_info {
	int	r_address;	/* address which is relocated */
unsigned int	r_symbolnum:24,	/* local symbol ordinal */
		r_pcrel:1, 	/* was relocated pc relative already */
		r_length:2,	/* 0=byte, 1=word, 2=long */
		r_extern:1,	/* does not include value of sym referenced */
		:4;		/* nothing, yet */
};
#endif

/*
 * Format of a symbol table entry; this file is included by <a.out.h>
 * and should be used if you aren't interested the a.out header
 * or relocation information.
 */
struct	nlist {
	union {
		char	*n_name;	/* for use when in-core */
		long	n_strx;		/* index into file string table */
	} n_un;
unsigned char	n_type;		/* type flag, i.e. N_TEXT etc; see below */
	char	n_other;	/* unused */
	short	n_desc;		/* see <stab.h> */
unsigned long	n_value;	/* value of this symbol (or sdb offset) */
};
#define	n_hash	n_desc		/* used internally by ld */

/*
 * Simple values for n_type.
 */
#define	N_UNDF	0x0		/* undefined */
#define	N_ABS	0x2		/* absolute */
#define	N_TEXT	0x4		/* text */
#define	N_DATA	0x6		/* data */
#define	N_BSS	0x8		/* bss */
#define	N_COMM	0x12		/* common (internal to ld) */
#define	N_FN	0x1e		/* file name symbol */

#define	N_EXT	01		/* external bit, or'ed in */
#define	N_TYPE	0x1e		/* mask for all the type bits */

/*
 * Sdb entries have some of the N_STAB bits set.
 * These are given in <stab.h>
 */
#define	N_STAB	0xe0		/* if any of these bits set, a SDB entry */

/*
 * Format for namelist values.
 */
#define	N_FORMAT	"%08x"

#endif /* _AOUT */
