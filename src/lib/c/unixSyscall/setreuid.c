/* 
 * setreuid.c --
 *
 *	Source code for the setreuid library procedure.
 *
 * Copyright 1988 Regents of the University of California
 * Permission to use, copy, modify, and distribute this
 * software and its documentation for any purpose and without
 * fee is hereby granted, provided that the above copyright
 * notice appear in all copies.  The University of California
 * makes no representations about the suitability of this
 * software for any purpose.  It is provided "as is" without
 * express or implied warranty.
 */

#ifndef lint
static char rcsid[] = "$Header: /sprite/src/lib/c/unixSyscall/RCS/setreuid.c,v 1.1 88/11/17 13:30:52 ouster Exp $ SPRITE (Berkeley)";
#endif not lint

#include <sprite.h>
#include <proc.h>
#include "compatInt.h"

/*
 *----------------------------------------------------------------------
 *
 * setreuid --
 *
 *	Procedure to map from Unix setreuid system call to Sprite Proc_SetIDs.
 *
 * Results:
 *	UNIX_ERROR is returned upon error, with the actual error code
 *	stored in errno.  Upon success, UNIX_SUCCESS is returned.
 *
 * Side effects:
 *	The real and effective user ID's of the process are modified as
 *	specified, if the user is privileged to do so.
 *
 *----------------------------------------------------------------------
 */

int
setreuid(ruid, euid)
    int	ruid, euid;
{
    ReturnStatus status;	/* result returned by Proc_SetIDs */

    if (ruid == -1) {
	ruid = PROC_NO_ID;
    }
    if (euid == -1) {
	euid = PROC_NO_ID;
    }
    status = Proc_SetIDs(ruid, euid);
    if (status != SUCCESS) {
	errno = Compat_MapCode(status);
	return(UNIX_ERROR);
    } else {
	return(UNIX_SUCCESS);
    }
}
