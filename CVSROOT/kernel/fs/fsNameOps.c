/* 
 * fsNameOps.c --
 *
 *	This has procedures for the operations done on file names
 *	like open and remove.  The name lookups are done via
 *	FsLookupOperation which uses the prefix table to choose the server.
 *
 * Copyright (C) 1987 Regents of the University of California
 * All rights reserved.
 * Permission to use, copy, modify, and distribute this
 * software and its documentation for any purpose and without
 * fee is hereby granted, provided that the above copyright
 * notice appear in all copies.  The University of California
 * makes no representations about the suitability of this
 * software for any purpose.  It is provided "as is" without
 * express or implied warranty.
 */

#ifndef lint
static char rcsid[] = "$Header$ SPRITE (Berkeley)";
#endif not lint


#include "sprite.h"
#include "fs.h"
#include "fsInt.h"
#include "fsOpTable.h"
#include "fsNameOps.h"
#include "fsTrace.h"
#include "fsStream.h"
#include "proc.h"
#include "rpc.h"
#include "dbg.h"


/*
 *----------------------------------------------------------------------
 *
 * Fs_Open --
 *
 *      This routine sets up a Fs_Stream object for the named file.  The
 *      parameters specify the manner in which the stream will be used and
 *      a set of permission bits in case a file needs to be created for
 *      the stream.
 *
 * Results:
 *      An error code, and a pointer to a Fs_Stream object for the file.
 *      This is used in further operations on the file and is released
 *      with Fs_Close
 *
 * Side effects:
 *	The stream is opened.
 *
 *----------------------------------------------------------------------
 */
ReturnStatus
Fs_Open(name, useFlags, type, permissions, streamPtrPtr)
    char 	*name;		/* The name of the file to open */
    register int useFlags;	/* Indicates read/write etc. the valid bits 
				 * to include are defined in fs.h */
    int 	type;		/* If FS_FILE then any type of file can be 
				 * opened, except if useFlags includes 
				 * FS_CREATE then a regular file will be 
				 * created.  Otherwise only the specified 
				 * type can be opened (or created). */
    int 	permissions;	/* A mask indicating the permission bits to 
				 * turn on if the file gets created */
    Fs_Stream 	**streamPtrPtr;	/* Contents set to point to a valid stream
			 	 * or NIL if there was an error. */
{
    register Fs_Stream 	*streamPtr;	/* Local copy of stream pointer */
    ReturnStatus 	status;		/* Return error code from RPC */
    FsOpenArgs 		openArgs;	/* Packaged up parameters */
    FsOpenResults 	openResults;	/* Packaged up results */
    Proc_ControlBlock	*procPtr;	/* Used to get process IDs */
    FsNameInfo		*nameInfoPtr;	/* Used to track name and prefix */

    *streamPtrPtr = (Fs_Stream *)NIL;

    if ((useFlags & FS_EXECUTE) && (useFlags & (FS_WRITE | FS_CREATE))) {
	return(FS_INVALID_ARG);
    }
	
    /*
     * Override the type if user flags indicate special files.
     */
    if (useFlags & FS_NAMED_PIPE_OPEN) {
	type = FS_NAMED_PIPE;
    }
    if (useFlags & FS_MASTER) {
	Sys_Panic(SYS_WARNING, "Fs_Open: FS_MASTER flag not supported\n");
	type = FS_PSEUDO_DEV;
    }
    if (useFlags & FS_NEW_MASTER) {
	type = FS_PSEUDO_DEV;
    }
    /*
     * Make sure we check for write permission before truncating.
     */
    if (useFlags & FS_TRUNC) {
	useFlags |= FS_WRITE;
    }

    /*
     * Call the Domain specific open routine.  The parameters to this are
     * packaged up and then it is called via the routine that deals
     * with the prefix table to choose domain type and server.  The domain
     * open routine returns streamData used to set up the I/O handle.
     * The stream's nameInfo is also set up as a side effect of going
     * through the prefix table.
     */
    procPtr = Proc_GetEffectiveProc();
    openArgs.useFlags		= useFlags;
    openArgs.permissions	= permissions & procPtr->fsPtr->filePermissions;
    openArgs.type		= type;
    openArgs.clientID		= rpc_SpriteID;
    FsSetIDs(procPtr, &openArgs.id);
    openResults.streamData	= (ClientData) NIL;

    nameInfoPtr = Mem_New(FsNameInfo);

    FS_TRACE_NAME(FS_TRACE_OPEN_START, name);
    status = FsLookupOperation(name, FS_DOMAIN_OPEN, (Address)&openArgs,
				(Address)&openResults, nameInfoPtr);
    FS_TRACE_NAME(FS_TRACE_OPEN_DONE, name);
    /*
     * Call the stream type open routine to set up the I/O handle.
     */
    if (status == SUCCESS) {
	/*
	 * Install the stream and then call the client open procedure
	 * to complete the setup of the stream's I/O handle
	 */
	streamPtr = FsStreamFind(&openResults.streamID, (FsHandleHeader *)NIL,
				 useFlags, (Boolean *)NIL);
	streamPtr->nameInfoPtr = nameInfoPtr;
	FsHandleUnlock(streamPtr);
	status = (*fsStreamOpTable[openResults.ioFileID.type].cltOpen)
		    (&openResults.ioFileID, &streamPtr->flags, rpc_SpriteID,
		     openResults.streamData, &streamPtr->ioHandlePtr);
	if (status == SUCCESS) {
	    if (streamPtr->flags & FS_TRUNC) {
		int length = 0;
		(void)Fs_IOControl(streamPtr, IOC_TRUNCATE, sizeof(int),
				    (Address)&length, 0, (Address)NIL);
	    }
	    *streamPtrPtr = streamPtr;
	} else {
	    FsHandleLock(streamPtr);
	    FsStreamDispose(streamPtr);
	}
    } else {
	Mem_Free((Address)nameInfoPtr);
    }
    return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * FsSetIDs --
 *
 *	Get user ID and group IDs from the proc table.  The FsUserID
 *	struct includes storage for the list of groups.  Alternatively,
 *	it could contain a pointer to the groups in the proc table.
 *
 *	TODO: Byte the bullet and figure out a nice way to pass a
 *	variable length list of groups around.  Then this procedure
 *	would not be needed, and the Fs_ProcessState could be
 *	referenced directly.  The ugliest place to do all this is
 *	in the RPC stubs.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Instantiate *idPtr to hold the user and group IDs of the current proc.
 *
 *----------------------------------------------------------------------
 */
void
FsSetIDs(procPtr, idPtr)
    Proc_ControlBlock 		*procPtr;
    FsUserIDs			*idPtr;
{
    register	Fs_ProcessState *fsPtr;
    register	int	*procGroupIDs;
    register	int	*groupPtr;
    register 	int 	i;

    if (procPtr == (Proc_ControlBlock *)NIL) {
	procPtr = Proc_GetEffectiveProc();
    }
    idPtr->user = procPtr->effectiveUserID;

    fsPtr = procPtr->fsPtr;
    if (fsPtr = (Fs_ProcessState *)NIL) {
	/*
	 * Exiting processes remove swap files after clearing fs state.
	 */
	idPtr->numGroupIDs = 0;
	procGroupIDs = (int *)NIL;
    } else {
	idPtr->numGroupIDs = fsPtr->numGroupIDs;
	procGroupIDs = fsPtr->groupIDs;
    }
    for (i = 0, groupPtr = idPtr->group; i < FS_NUM_GROUPS;  i++, groupPtr++) {
	/*
	 * The file system state record supports a variable length array of
	 * group IDs but here we truncate it...
	 */
	if (i < idPtr->numGroupIDs) {
	    *groupPtr = *procGroupIDs;
	    procGroupIDs++;
	} else {
	    *groupPtr = -1;
	}
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Fs_Remove --
 *
 *	Remove the named file.  Because each entry in a directory is only
 *	one of many posible references, the disk space for the file may or
 *	may not be freed after this call.   Only when the last link to
 *	the file is removed is the disk space freed up.  This does not
 *	follow links so that links are removed instead of the file
 *	they reference.
 *
 * Results:
 *	An error code.
 *
 * Side effects:
 *	The file is removed
 *
 *----------------------------------------------------------------------
 */
ReturnStatus
Fs_Remove(name)
    char *name;		/* The name of the file to remove */
{
    ReturnStatus status;
    FsLookupArgs lookupArgs;

    lookupArgs.useFlags = FS_DELETE;
    FsSetIDs((Proc_ControlBlock *)NIL, &lookupArgs.id);
    lookupArgs.clientID = rpc_SpriteID;
    status = FsLookupOperation(name, FS_DOMAIN_REMOVE,
		     (Address)&lookupArgs, (Address)NIL, (FsNameInfo *)NIL);
    return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * Fs_RemoveDir --
 *
 *	Remove the named directory.  It must be empty, that is it should
 *	only contain entries for itself (.) and its parent (..).
 *
 * Results:
 *	An error code.
 *
 * Side effects:
 *	The directory is removed if possible
 *
 *----------------------------------------------------------------------
 */
ReturnStatus
Fs_RemoveDir(name)
    char *name;		/* The name of the directory to remove */
{
    ReturnStatus status;
    FsLookupArgs lookupArgs;

    lookupArgs.useFlags = FS_DELETE;
    FsSetIDs((Proc_ControlBlock *)NIL, &lookupArgs.id);
    lookupArgs.clientID = rpc_SpriteID;
    status = FsLookupOperation(name, FS_DOMAIN_REMOVE_DIR,
			 (Address)&lookupArgs, (Address)NIL, (FsNameInfo *)NIL);
    return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * Fs_MakeDevice --
 *
 *	Make the named device file.  This is the procedure which
 *	does the actual work for the Fs_MakeDevice system call.
 *
 * Results:
 *	An error code.
 *
 * Side effects:
 *	The device file is made
 *
 *----------------------------------------------------------------------
 */
ReturnStatus
Fs_MakeDevice(name, devicePtr, permissions)
    char *name;			/* The name of the directory to make */
    Fs_Device *devicePtr;	/* Specifies device info */
    int permissions;		/* The permissions for the new directory */
{
    ReturnStatus status;	/* Return error code */
    FsMakeDeviceArgs makeDevArgs;/* Packaged up parameters */
    Proc_ControlBlock *procPtr;	/* Used to get process IDs */

    procPtr = Proc_GetEffectiveProc();
    makeDevArgs.device 		= *devicePtr;
    makeDevArgs.permissions	= permissions & procPtr->fsPtr->filePermissions;
    FsSetIDs(procPtr, &makeDevArgs.id);
    makeDevArgs.clientID	= rpc_SpriteID;

    status = FsLookupOperation(name, FS_DOMAIN_MAKE_DEVICE,
		     (Address)&makeDevArgs, (Address)NIL, (FsNameInfo *)NIL);
    return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * Fs_MakeDir --
 *
 *	Make the named directory.  This is the procedure which
 *	does the actual work for the Fs_MakeDir system call.
 *
 * Results:
 *	An error code.
 *
 * Side effects:
 *	The directory is made
 *
 *----------------------------------------------------------------------
 */
ReturnStatus
Fs_MakeDir(name, permissions)
    char *name;		/* The name of the directory to make */
    int permissions;	/* The permissions for the new directory */
{
    ReturnStatus status;	/* Return error code from RPC */
    FsOpenArgs openArgs;	/* Packaged up parameters */
    Proc_ControlBlock *procPtr;	/* Used to get process IDs */

    procPtr = Proc_GetEffectiveProc();
    openArgs.useFlags = FS_CREATE | FS_EXCLUSIVE | FS_FOLLOW ;
    openArgs.permissions = permissions & procPtr->fsPtr->filePermissions;
    openArgs.type = FS_DIRECTORY;
    FsSetIDs(procPtr, &openArgs.id);
    openArgs.clientID = rpc_SpriteID;

    status = FsLookupOperation(name, FS_DOMAIN_MAKE_DIR, (Address)&openArgs,
				    (Address)NIL, (FsNameInfo *)NIL);
    return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * Fs_ChangeDir --
 *
 *	Change the process's current directory.  This opens the
 *	new current directory, and if that's successful the
 *	old one is closed.
 *
 * Results:
 *	An error code.
 *
 * Side effects:
 *	Change the current directory.
 *
 *----------------------------------------------------------------------
 */
ReturnStatus
Fs_ChangeDir(pathName)
    char *pathName;
{
    register	Fs_ProcessState *fsPtr;
    Fs_Stream		*newCwdPtr;	/* A stream is used because it has
					 * a reference count and can be
					 * closed with existing routines. */
    ReturnStatus	status;

    fsPtr = (Proc_GetEffectiveProc())->fsPtr;

    /*
     * FS_EXECUTE permission needed to change to a directory.
     */
    newCwdPtr = (Fs_Stream *)NIL;
    status = Fs_Open(pathName, FS_EXECUTE | FS_FOLLOW, FS_DIRECTORY, 0,
				  &newCwdPtr);
    if (status) {
	return(status);
    }
    (void)Fs_Close(fsPtr->cwdPtr);
    fsPtr->cwdPtr = newCwdPtr;
    return(SUCCESS);
}

/*
 *----------------------------------------------------------------------
 *
 * Fs_Trunc --
 *
 *	Truncate a file to a given length given its pathname.
 *
 * Results:
 *	An error code
 *
 * Side effects:
 *	The files length gets set and any data beyond that is deleted.
 *
 *----------------------------------------------------------------------
 */
ReturnStatus
Fs_Trunc(pathName, length)
    char *pathName;
    int length;
{
    Fs_Stream *streamPtr;
    ReturnStatus status;

    streamPtr = (Fs_Stream *)NIL;
    status = Fs_Open(pathName, FS_WRITE | FS_FOLLOW, FS_FILE, 0, &streamPtr);
    if (status != SUCCESS) {
	return(status);
    }
    status = Fs_IOControl(streamPtr, IOC_TRUNCATE, sizeof(int),
			    (Address)&length, 0, (Address)NIL);
    (void)Fs_Close(streamPtr);
    return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * Fs_GetAttributes --
 *
 *	Get the attributes of a file given its name.  First the name server
 *	is contacted to get the initial version of the attributes.  Then
 *	the I/O server is contacted to update attributes like the
 *	access and modify times.
 *
 * Results:
 *	An error code and the attributes.
 *
 * Side effects:
 *	None here.
 *
 *----------------------------------------------------------------------
 */
ReturnStatus
Fs_GetAttributes(pathName, fileOrLink, attrPtr)
    char *pathName;
    int fileOrLink;		/* FS_ATTRIB_FILE or FS_ATTRIB_LINK */
    Fs_Attributes *attrPtr;
{
    ReturnStatus status;
    FsOpenArgs openArgs;
    FsGetAttrResults getAttrResults;	/* References attrPtr and ioFileID */
    FsFileID ioFileID;			/* Returned from name server, indicates
					 * who the I/O server is. */


    openArgs.useFlags = (fileOrLink == FS_ATTRIB_LINK) ? 0 : FS_FOLLOW;
    openArgs.permissions = 0;
    openArgs.type = FS_FILE;
    openArgs.clientID = rpc_SpriteID;
    FsSetIDs((Proc_ControlBlock *)NIL, &openArgs.id);

    getAttrResults.attrPtr = attrPtr;
    getAttrResults.fileIDPtr = &ioFileID;

    /*
     * Get an initial version of the attributes from the name server.
     */
    status = FsLookupOperation(pathName, FS_DOMAIN_GET_ATTR,
		 (Address)&openArgs, (Address)&getAttrResults,
		 (FsNameInfo *)NIL);
    if (status == SUCCESS) {
	/*
	 * Update those with attributes cached at the I/O server.
	 */
	status = (*fsStreamOpTable[ioFileID.type].getIOAttr)
			(&ioFileID, rpc_SpriteID, attrPtr);
    }
    return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * Fs_SetAttributes --
 *
 *	Set some attributes of a file given its name.  The input contains
 *	the complete set of attributes for the file but not all of these
 *	are affected by this call.  Ie.  the user can't change everything.
 *	The name server is contacted first to set attributes file descriptor,
 *	then the I/O server is contacted to update cached attributes.
 *
 * Results:
 *	An error code
 *
 * Side effects:
 *	See FsLocalSetAttrID for which attributes get updated at the
 *	file server.
 *
 *----------------------------------------------------------------------
 */
ReturnStatus
Fs_SetAttributes(pathName, fileOrLink, attrPtr)
    char *pathName;
    int fileOrLink;		/* FS_ATTRIB_FILE or FS_ATTRIB_LINK */
    Fs_Attributes *attrPtr;
{
    register ReturnStatus status;
    FsSetAttrArgs setAttrArgs;		/* Bundled openArgs and attributes */
    FsOpenArgs *openArgsPtr;		/* Pointer into setAttrArgs */
    FsFileID ioFileID;			/* Used to get to I/O server */

    openArgsPtr = &setAttrArgs.openArgs;
    openArgsPtr->useFlags = FS_OWNERSHIP;
    if (fileOrLink == FS_ATTRIB_LINK) {
	openArgsPtr->useFlags |= FS_FOLLOW;
    }
    openArgsPtr->permissions = 0;
    openArgsPtr->type = FS_FILE;
    openArgsPtr->clientID = rpc_SpriteID;
    FsSetIDs((Proc_ControlBlock *)NIL, &openArgsPtr->id);

    /*
     * This copy is done here to avoid doing it in the client RPC stub.
     */
    setAttrArgs.attr = *attrPtr;

    /*
     * Set the attributes at the name server.  We get in return a fileID
     * for the actual device which specifies a serverID.
     */
    status = FsLookupOperation(pathName, FS_DOMAIN_SET_ATTR,
		     (Address)&setAttrArgs, (Address)&ioFileID,
		     (FsNameInfo *)NIL);
    if (status == SUCCESS) {
	/*
	 * Set the attributes at the I/O server.
	 */
	status = (*fsStreamOpTable[ioFileID.type].setIOAttr)
			(&ioFileID, attrPtr);
    }
    return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * Fs_HardLink --
 *
 *      Make two pathnames refer to the same file.  The pathnames are
 *      restricted to be in the same domain, ie. stored on the same
 *      disk pack, so the file server can make the link.
 *
 * Results:
 *      SUCCESS if the link was made.  FS_CANT_LINK if the pathnames
 *      are not in the same domain.
 *
 * Side effects:
 *      Cause the two pathnames to refer to the same file.
 *
 *----------------------------------------------------------------------
 */
ReturnStatus
Fs_HardLink(pathName, linkName)
    char *pathName;	/* Name of the existing file */
    char *linkName;	/* Name of the file to create which is a link to
			 * the existing file */
{
    ReturnStatus status;
    FsLookupArgs lookupArgs;

    lookupArgs.useFlags = FS_LINK;
    FsSetIDs((Proc_ControlBlock *)NIL, &lookupArgs.id);
    lookupArgs.clientID = rpc_SpriteID;
    status = FsTwoNameOperation(FS_DOMAIN_HARD_LINK, pathName, linkName,
						     &lookupArgs);
    return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * Fs_Rename --
 *
 *	Change the name of a file.  This is complicated because the files
 *	can only be in the same domain.  This is not directly evident.
 *
 * Results:
 *      SUCCESS if the name was changed.
 *
 * Side effects:
 *	Change the name of a file.
 *
 *----------------------------------------------------------------------
 */
ReturnStatus
Fs_Rename(pathName, newName)
    char *pathName;	/* Name of the existing file */
    char *newName;	/* The new pathname for the file */
{
    ReturnStatus status;
    FsLookupArgs lookupArgs;

    lookupArgs.useFlags = FS_LINK | FS_RENAME;
    FsSetIDs((Proc_ControlBlock *)NIL, &lookupArgs.id);
    lookupArgs.clientID = rpc_SpriteID;
    status = FsTwoNameOperation(FS_DOMAIN_RENAME, pathName, newName,
						  &lookupArgs);
    return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * Fs_SymLink --
 *
 *	Create a symbolic link file by writing the name of the target
 *	file (including the NULL) into the link file.  This only
 *	succeeds if the link file does not already exist.
 *
 * Results:
 *      SUCCESS if the link was created.
 *
 * Side effects:
 *	Create the file and write the link name into it.
 *
 *----------------------------------------------------------------------
 */
ReturnStatus
Fs_SymLink(targetName, linkName, remoteFlag)
    char *targetName;	/* Name of the file to link to */
    char *linkName;	/* The name of the new link file that's created */
    Boolean remoteFlag;	/* TRUE => link will be a REMOTE_LINK */
{
    ReturnStatus status;
    Fs_Stream *streamPtr;
    int length;

    if (remoteFlag) {
	/*
	 * Could check for super-user only here.
	 */
	if (targetName[0] != '/') {
	    /*
	     * Remote links must be absolute.  They should also be circular,
	     * but that is harder to check.
	     */
	    return(FS_INVALID_ARG);
	}
    }
    status = Fs_Open(linkName, FS_CREATE | FS_WRITE | FS_EXCLUSIVE,
		        (remoteFlag ? FS_REMOTE_LINK : FS_SYMBOLIC_LINK),
		        0777, &streamPtr);
    if (status == SUCCESS) {
	length = String_Length(targetName) + 1;
	status = Fs_Write(streamPtr, targetName, 0, &length);
	(void)Fs_Close(streamPtr);
    }
    return(status);
}
