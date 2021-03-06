/*
 * specs.h --
 *
 *	This file is included by gcc.c, and defines the compilation
 *	and linking specs for each of the target machines for which
 *	gcc can currently generate code.
 *
 * Copyright 1988 Regents of the University of California
 * Permission to use, copy, modify, and distribute this
 * software and its documentation for any purpose and without
 * fee is hereby granted, provided that the above copyright
 * notice appear in all copies.  The University of California
 * makes no representations about the suitability of this
 * software for any purpose.  It is provided "as is" without
 * express or implied warranty.
 *
 * $Header: /sprite/src/cmds/cc/sprite/RCS/specs.h,v 1.18 91/09/12 15:40:39 rab Exp Locker: rab $ SPRITE (Berkeley)
 */

#ifndef _SPECS
#define _SPECS

/* This structure says how to run one compiler, and when to do so.  */

struct compiler
{
  char *suffix;			/* Use this compiler for input files
				   whose names end in this suffix.  */
  char *spec;			/* To use this compiler, pass this spec
				   to do_spec.  */
};

/*
 * One of the following structures exists for each machine for
 * which gcc can generate code.  It gives all the spec-related
 * information for compiling for tha target.
 */

typedef struct
{
    char *name;				/* Official name of this target
					 * machine (many -m switches may
					 * map to the same entry). */
    struct compiler *base_specs;	/* List of specs to use when compiling
					 * for this target machine.  Last
					 * compiler must be all zeros to
					 * indicate end of list. */
    struct compiler *cplusplus_specs;   /* List of specs to use when invoked					           as a c++ compiler */
    char *link_base_spec;		/* Basic spec to use to link for this
					 * target. */
    char *cpp_predefines;		/* Spec to substitute for %p. */
    char *cpp_spec;			/* Spec to substitute for %C. */
    char *cc1_spec;                     /* Spec to substitute for %1. */
    char *cplus1_spec;                  /* Spec to substitute for %+  */
    char *asm_spec;			/* Spec to substitute for %a. */
    char *link_spec;			/* Spec to substitute for %l. */
    char *lib_spec;			/* Spec to substitute for %L. */
    char *startfile_spec;		/* Spec to substitute for %S. */
    char *signed_char_spec;             /* Spec to substitute for %c. */
} target_specs;

static char default_assembler_string[] = 
     "%{!E:%{!S:%a %{R} %{j} %{J} %{h} %{d2}\
	%i %{c:%{o*}%{!o*:-o %w%b.o}}%{!c:-o %d%w%b.o}\n }}\
      %{E:%C %{nostdinc} %{C} %{v} %{D*} %{U*} %{I*} -m%m %{M*} %{T} \
        -undef -D__GNUC__ %{ansi: -$ -D__STRICT_ANSI__}\
	%{!ansi:%p -Dunix -Dsprite}\
        %{O:-D__OPTIMIZE__} %{traditional} %{pedantic}\
	%{Wcomment} %{Wtrigraphs} %{Wall}\
        %i %{o*} %{M*:%{o*}}}\n";

static char default_c_string[] =
     "%C %{nostdinc} %{C} %{v} %{D*} %{U*} %{I*} -m%m %{M*} %{T} \
        -undef -D__GNUC__ %{ansi: -$ -D__STRICT_ANSI__}\
	%{!ansi:%p -Dunix -Dsprite}\
	%{msoft-float:-D__SOFT_FLOAT__}\
        %{O:-D__OPTIMIZE__} %{traditional} %{pedantic}\
	%{Wcomment} %{Wtrigraphs} %{Wall}\
        %i %{!M*:%{!E:%g.cpp}}%{E:%{o*}}%{M*:%{o*}}\n\
     %{!M*:%{!E:%1 %g.cpp %{!Q:-quiet} -dumpbase %i %{Y*} %{d*} %{m*} %{f*}\
	%{a} %{g} %{O} %{W*} %{w} %{pedantic} %{ansi} %{traditional}\
	%{v:-version} %{gg:-symout %g.sym} %{pg:-p} %{p}\
        %{S:%{o*}%{!o*:-o %b.s}}%{!S:-o %g.s}\n\
     %{!S:%a %{R} %{j} %{J} %{h} %{d2} %{gg:-G %g.sym}\
	%g.s %{c:%{o*}%{!o*:-o %w%b.o}}%{!c:-o %d%w%b.o}\n }}}";

static char default_cplusplus_string[] = 
    "%C -+ %{nostdinc} %{C} %{v} %{D*} %{U*} %{I*} \
       -I/sprite/lib/include/g++ -m%m %{M*} %{T} \
       -undef -D__GNUC__ -D__GNUG__ -D__cplusplus %p -Dsprite %P\
       %c %{O:-D__OPTIMIZE__} %{traditional} %{pedantic}\
       %{Wcomment*} %{Wtrigraphs} %{Wall}\
       %i %{!M*:%{!E:%g.cpp}}%{E:%{o*}}%{M*:%{o*}}\n\
     %{!M*:%{!E:%+ %g.cpp %{!Q:-quiet} -dumpbase %i %{Y*} %{d*} %{m*} %{f*}\
	%{+e*} %{a} %{g} %{g0} %{O} %{W*} %{w} %{pedantic} %{traditional}\
	%{v:-version} %{gg:-symout %g.sym} %{pg:-p} %{p}\
        %{S:%{o*}%{!o*:-o %b.s}}%{!S:-o %g.s}\n\
     %{!S:%a %{R} %{j} %{J} %{h} %{d2} %{gg:-G %g.sym}\
	%g.s %{c:%{o*}%{!o*:-o %w%b.o}}%{!c:-o %d%w%b.o}\n }}}";

/*
 * Spec information for individual machines:
 */

static struct compiler default_compilers[] =
{
    {".c", default_c_string },
    {".cc", default_cplusplus_string },
    {".s", default_assembler_string },
    /* Mark end of table. */
    {0, 0}
};

static struct compiler default_cplusplus[] =
{
    {".c", default_cplusplus_string },
    {".cc", default_cplusplus_string },
    {".s", default_assembler_string },
    /* Mark end of table. */
    {0, 0},
};

#if 0
static struct compiler sun3_compilers[] =
{
    {".c",
     "%C %{nostdinc} %{C} %{v} %{D*} %{U*} %{I*} -m%m %{M*} %{T} \
        -undef -D__GNUC__ %{ansi: -$ -D__STRICT_ANSI__}\
	%{!ansi:%p -Dunix -Dsprite}\
	%{!m68881:-D__SOFT_FLOAT__}\
        %{O:-D__OPTIMIZE__} %{traditional} %{pedantic}\
	%{Wcomment} %{Wtrigraphs} %{Wall}\
        %i %{!M*:%{!E:%g.cpp}}%{E:%{o*}}%{M*:%{o*}}\n\
     %{!M*:%{!E:%1 %g.cpp %{!Q:-quiet} -dumpbase %i %{Y*} %{d*} %{m*} %{f*}\
	%{a} %{g} %{O} %{W*} %{w} %{pedantic} %{ansi} %{traditional}\
	%{v:-version} %{gg:-symout %g.sym} %{pg:-p} %{p}\
	%{!m68881:-msoft-float}\
        %{S:%{o*}%{!o*:-o %b.s}}%{!S:-o %g.s}\n\
     %{!S:%a %{R} %{j} %{J} %{h} %{d2} %{gg:-G %g.sym}\
	%g.s %{c:%{o*}%{!o*:-o %w%b.o}}%{!c:-o %d%w%b.o}\n }}}"},
    {".s",
     "%{!E:%{!S:%a %{R} %{j} %{J} %{h} %{d2}\
	%i %{c:%{o*}%{!o*:-o %w%b.o}}%{!c:-o %d%w%b.o}\n }}\
      %{E:%C %{nostdinc} %{C} %{v} %{D*} %{U*} %{I*} -m%m %{M*} %{T} \
        -undef -D__GNUC__ %{ansi: -$ -D__STRICT_ANSI__}\
	%{!ansi:%p -Dunix -Dsprite}\
        %{O:-D__OPTIMIZE__} %{traditional} %{pedantic}\
	%{Wcomment} %{Wtrigraphs} %{Wall}\
        %i %{o*} %{M*:%{o*}}}\n"},
    /* Mark end of table. */
    {0, 0}
};
#endif

static char default_link_spec[] =
    "%{!c:%{!M*:%{!E:%{!S:%l %{o*}\
    %{A} %{d} %{e*} %{N} %{n} %{r} %{s} %{S} %{T*} %{t} %{u*} %{X} %{x} %{z}\
    %{y*} %{!nostdlib:%S} %{L*} -L/sprite/lib/%m.md \
    %o %{!nostdlib:%L}\n }}}}";


static target_specs m68000_target =
{
    "sun2",					/* name */
    default_compilers,				/* base_specs */
    default_cplusplus,                          /* c++ specs */
    default_link_spec,				/* link_base_spec */
    "-Dmc68000 -Dsun2",				/* cpp_predefines */
    "cpp",                                      /* cpp_spec */
    "cc1.68k -msoft-float -m68000",		/* cc1_spec */
    "g++1.68k -m68000",                   /* cplus1_spec */
    "as.sun3 -m68010",				/* asm_spec */
    "ld.sun3 -X -m68010 %{!e:-e start}",	/* link_spec */
    "%{!p:%{!pg:-lc}}%{p:-lc_p}%{pg:-lc_p}",	/* lib_spec */
    "",						/* start_file_spec */
    "%{funsigned-char: -D__CHAR_UNSIGNED__}",   /* signed_char_spec */
};

static target_specs m68020_target =
{
    "sun3",					/* name */
    default_compilers,				/* base_specs */
    default_cplusplus,                          /* c++ specs */
    default_link_spec,				/* link_base_spec */
    "-Dmc68000 -Dsun3 -Dsun",			/* cpp_predefines */
    "cpp",                                      /* cpp_spec */
    "cc1.68k -m68020",                          /* cc1_spec */
    "g++1.68k -m68020",                          /* cplus1_spec */
    "as.sun3 -m68020",				/* asm_spec */
    "ld.sun3 -X %{!e:%{!pg:-e start}%{pg:-e gstart}}",
						/* link_spec */	
    "%{!p:%{!pg:-lc}}%{p:-lc_p}%{pg:-lc_p}",	/* lib_spec */
    "",						/* start_file_spec */
    "%{funsigned-char: -D__CHAR_UNSIGNED__}",   /* signed_char spec */
};

static target_specs spur_target =
{
    "spur",					/* name */
    default_compilers,				/* base_specs */
    default_cplusplus,                          /* c++ specs */
    default_link_spec,				/* link_base_spec */
    "-Dspur",					/* cpp_predefines */
    "cpp",                                      /* cpp_spec */
    "cc1.spur -msoft-float",			/* cc1_spec */
    "g++1.spur -msoft-float",                    /* cplus1_spec */
    "sas",					/* asm_spec */
    "sld -X -p %{!e:-e start}",			/* link_spec */
    "%{!p:%{!pg:-lc}}%{p:-lc_p}%{pg:-lc_p}",	/* lib_spec */
    "",						/* start_file_spec */
    "%{funsigned-char: -D__CHAR_UNSIGNED__}",   /* signed_char spec */
};

static target_specs sparc_target =
{
    "sun4",					/* name */
    default_compilers,				/* base_specs */
    default_cplusplus,                          /* c++ specs */
    default_link_spec,				/* link_base_spec */
    "-Dsparc -Dsun4 -Dsun",			/* cpp_predefines */
    "cpp",                                      /* cpp_spec */
    "cc1.sparc",                		/* cc1_spec */
    "g++1.sparc",                                /* cplus1_spec */
    "as.sun4",		         		/* asm_spec */
    "ld.sun4 -X %{!e:%{!pg:-e start}%{pg:-e gstart}}",
						/* link_spec */
    "%{!p:%{!pg:-lc}}%{p:-lc_p}%{pg:-lc_p}",	/* lib_spec */
    "",						/* start_file_spec */
    "%{funsigned-char: -D__CHAR_UNSIGNED__}",   /* signed_char spec */
};


static target_specs mips_target =
{
    "ds3100",					/* name */
    default_compilers,				/* base_specs */
    default_cplusplus,                          /* c++ specs */
    default_link_spec,				/* link_base_spec */
    "-Dmips -Dds3100",				/* cpp_predefines */
    "cpp",                                      /* cpp_spec */
    "cc1.mips",                 		/* cc1_spec */
    "g++1.mips",                                 /* cplus1_spec */
#if 0
    "as.ds3100",				/* asm_spec */
    "ld.ds3100 -X %{!e:%{!pg:-e start}%{pg:-e gstart}}",
						/* link_spec */
#else
    /* 
     *    For now we have to use the Ultrix assembler and linker.
     *	These will fail on a sun.
     */
#ifdef ds3100
    "/sprite/cmds.ds3100/as",
    "/sprite/cmds.ds3100/ld -B1.31 /usr/lib/crt0.o1.31",
#else
    "echo YOU CANT ASSEMBLE FOR THE DS3100 ON THIS MACHINE",
    "echo YOU CANT LINK FOR THE DS3100 ON THIS MACHINE",
#endif
#endif
    "%{!p:%{!pg:-lc}}%{p:-lc_p}%{pg:-lc_p}",	/* lib_spec */
    "",						/* start_file_spec */
    "%{funsigned-char: -D__CHAR_UNSIGNED__}",   /* signed_char spec */
};


static target_specs symm_target =
{
    "symm",					/* name */
    default_compilers,				/* base_specs */
    default_cplusplus,                          /* c++ specs */
    default_link_spec,				/* link_base_spec */
    "-Di386 -Dsymm -Dsequent",			/* cpp_predefines */
    "cpp",                                      /* cpp_spec */
    "cc1.symm",                    		/* cc1_spec */
    "g++1.symm",                                /* cplus1_spec */
    "as.symm",		         		/* asm_spec */
    "ld.symm -Tdata 400000 %{!e:%{!pg:-e __start}%{pg:-e __gstart}}",
						/* link_spec */
    "%{!p:%{!pg:-lc}}%{p:-lc_p}%{pg:-lc_p}",	/* lib_spec */
    "",						/* start_file_spec */
    "%{funsigned-char: -D__CHAR_UNSIGNED__}",   /* signed_char spec */
};


/*
 * Top-level table used to look up information for a particular target
 * machine.  The first entry will be used as the default target if
 * a target isn't specified in a command-line switch or environment
 * variable.
 */

typedef struct
{
    char *name;			/* Name of target machine (as it
				 * appears in "-m" switch. */
    target_specs *info;		/* Information about target machine. */
} target_machine;

static target_machine target_machines[] = {
    {"sun3",	&m68020_target},
    {"68000",	&m68000_target},
    {"68010",	&m68000_target},
    {"sun2",	&m68000_target},
    {"68020",	&m68020_target},
    {"spur",	&spur_target},
    {"sun4",    &sparc_target},
    {"sun4c",   &sparc_target},
    {"sun4c2",  &sparc_target},
    {"sparc",   &sparc_target},
    {"ds3100",  &mips_target},
    {"mips",    &mips_target},
    {"sym",     &symm_target},
    {"symm",    &symm_target},
    {"sequent", &symm_target},
    {"i386",    &symm_target},
    {0, 0}				/* Zeroes mark end of list. */
};

static char *target_name;		/* Name of selected target. */
static target_specs *target;		/* Info for selected target. */

/*
 *  Put temporary files in /tmp
 */
#define P_tmpdir    "/tmp"

#endif /* _SPECS */

