#
# Prototype Makefile for machine-dependent directories.
#
# A file of this form resides in each ".md" subdirectory of a
# command.  Its name is typically "md.mk".  During makes in the
# parent directory, this file (or a similar file in a sibling
# subdirectory) is included to define machine-specific things
# such as additional source and object files.
#
# This Makefile is automatically generated.
# DO NOT EDIT IT OR YOU MAY LOSE YOUR CHANGES.
#
# Generated from /sprite/lib/mkmf/Makefile.md
# Sun Nov 17 20:48:20 PST 1991
#
# For more information, refer to the mkmf manual page.
#
# $Header: /sprite/lib/mkmf/RCS/Makefile.md,v 1.6 90/03/12 23:28:42 jhh Exp $
#
# Allow mkmf

SRCS		= fsconsistCache.c fsconsistIOClient.c
HDRS		= fsconsist.h
MDPUBHDRS	= 
OBJS		= ds3100.md/fsconsistCache.o ds3100.md/fsconsistIOClient.o
CLEANOBJS	= ds3100.md/fsconsistCache.o ds3100.md/fsconsistIOClient.o
INSTFILES	= Makefile local.mk
SACREDOBJS	= 
