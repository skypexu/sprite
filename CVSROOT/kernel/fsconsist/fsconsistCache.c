/* 
 * fsCacheConsist.c --
 *
 *	Routines used to keep the file system caches consistent.  The
 *	server maintains a list of client machines that have the file
 *	open.  This list indicates how many opens per client, if the file
 *	is being cached, and if the file is open for writing on a client.
 *	The client list is updated when files are opened, closed, and removed.
 *
 *	SYNCHRONIZATION:  There are two classes of procedures here: those
 *	that make call-backs to clients, and those that just examine the
 *	client list.  All access to a client list is synchronized using
 *	a monitor lock embedded in the consist structure.  Furthermore,
 *	consistency actions are serialized by setting a flag during
 *	call-backs (CONSIST_IN_PROGRESS).  There is a possible deadlock
 *	if the monitor lock is held during a call-back because this
 *	prevents a close operation from completing. (At close time the
 *	client list is adjusted but no call-backs are made.  Also, the client
 *	has its handle locked which blocks our call-back.)  Accordingly,
 *	both the handle lock and the monitor lock are released during
 *	a call-back.  The CONSIST_IN_PROGRESS flag is left on to prevent
 *	other consistency actions during the call-back.
 *
 * Copyright 1986 Regents of the University of California.
 * All rights reserved.
 */

#ifndef lint
static char rcsid[] = "$Header$ SPRITE (Berkeley)";
#endif not lint

#include	"sprite.h"
#include	"fs.h"
#include	"fsInt.h"
#include	"fsClient.h"
#include	"fsConsist.h"
#include	"fsBlockCache.h"
#include	"fsCacheOps.h"
#include	"fsOpTable.h"
#include	"fsLocalDomain.h"
#include	"fsDebug.h"
#include	"hash.h"
#include	"vm.h"
#include	"proc.h"
#include	"sys.h"
#include	"rpc.h"
#include	"recov.h"
#include	"dbg.h"

#define	LOCKPTR	(&consistPtr->lock)

Boolean	fsCacheDebug = FALSE;
Boolean	fsClientCaching = TRUE;
#ifdef CONSIST_DEBUG
int	fsTraceConsistMinor = 2249;
#endif /* CONSIST_DEBUG */

/*
 * Flags for the FsConsistInfo struct that's defined in fsInt.h
 *
 *	FS_CONSIST_IN_PROGRESS	Cache consistency is being performed on this
 *				file.
 *	FS_CONSIST_ERROR	There was an error during the cache 
 *				consistency.
 */
#define	FS_CONSIST_IN_PROGRESS	0x1
#define	FS_CONSIST_ERROR	0x2

/*
 * Rpc to send when forcing a client to invalidate or write back a file.
 */

typedef struct ConsistMsg {
    Fs_FileID	fileID;		/* Which file to invalidate. */
    int		flags;		/* One of the flags defined below. */
    int		openTimeStamp;	/* Open that this rpc pertains to. */
    int		version;	/* Version number of the file */
} ConsistMsg;

/*
 * Message sent when the client has completed the work requested by the server.
 * This is actually the request part of the rpc transaction.
 */

typedef struct ConsistReply {
    Fs_FileID 		fileID;
    FsCachedAttributes	cachedAttr;
    ReturnStatus	status;
} ConsistReply;


/*
 * Structure used to keep track of outstanding cache consistency requests.
 */
typedef struct {
    List_Links	links;
    int		clientID;
    int		flags;
} ConsistMsgInfo;

/*
 * Global time stamp.  A time stamp is returned to clients on each open
 * and on cache consistency messages.  This way clients can detect races
 * between open replies and consistency actions.
 */
static	int	openTimeStamp = 0;

/*
 * Cache conistency statistics.
 */

FsCacheConsistStats fsConsistStats;

/*
 * Forward declarations.
 */
void		StartConsistency();
void 		UpdateList();
ReturnStatus	EndConsistency();
void	 	ClientCommand();
void		ProcessConsist();
void		ProcessConsistReply();
char *		ConsistType();


/*
 * ----------------------------------------------------------------------------
 *
 * FsConsistInit --
 *
 *      Initialize the client use information for a file.  This is done
 *	before adding any clients so it just resets all the fields.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Reset the client use state..
 *
 * ----------------------------------------------------------------------------
 *
 */
void
FsConsistInit(consistPtr, hdrPtr)
    register FsConsistInfo *consistPtr;	/* State to initialize */
    FsHandleHeader *hdrPtr;		/* Back pointer to handle */
{
    consistPtr->flags = 0;
    consistPtr->lastWriter = -1;
    consistPtr->openTimeStamp = 0;
    consistPtr->hdrPtr = hdrPtr;
    SYNC_LOCK_INIT_DYNAMIC(&consistPtr->lock);
    consistPtr->consistDone.waiting = 0;
    consistPtr->repliesIn.waiting = 0;
    List_Init(&consistPtr->clientList);
    List_Init(&consistPtr->msgList);
    List_Init(&consistPtr->migList);
}

/*
 * ----------------------------------------------------------------------------
 *
 * FsFileConsistency --
 *
 *	Take action to ensure that the caches are consistency for this
 *	file.  This checks against use conflicts and will return an
 *	non-SUCCESS status if the open should fail.  Otherwise this
 *	makes call-backs to other clients to keep caches consistent.
 *
 * Results:
 *	SUCCESS or FS_FILE_BUSY.  Also, *cacheablePtr set to TRUE if the
 *	file is cacheable on the client.  *openTimeStampPtr is set to the
 *	next open time stamp on the file.  Clients use this timeStamp
 *	to catch races between open replies, which have the next timeStamp,
 *	and consistency messages from other opens happening
 *	at about the same time, which have the current timeStamp.
 *
 * Side effects:
 *	Issues cache consistency messages and adds the client to the
 *	lis of clients of the file.
 *	THIS UNLOCKS THE HANDLE.
 *
 * ----------------------------------------------------------------------------
 *
 */
ENTRY ReturnStatus
FsFileConsistency(handlePtr, clientID, useFlags, cacheablePtr,
    openTimeStampPtr)
    FsLocalFileIOHandle *handlePtr;	/* File to check consistency of. */
    int 		clientID;	/* ID of the host doing the open */
    register int 	useFlags;	/* useFlags from the open call */
    Boolean		*cacheablePtr;	/* TRUE if file is cacheable. */
    int			*openTimeStampPtr;/* Timestamp of open.  Used by clients
					 * to catch races between open replies
					 * and cache consistency messages */
{
    register FsConsistInfo *consistPtr = &handlePtr->consist;
    ReturnStatus status;

    LOCK_MONITOR;

    /*
     * Go through the list of other clients using the file checking
     * for conflicts and issuing cache consistency messages.
     */
    StartConsistency(consistPtr, clientID, useFlags, cacheablePtr);
    /*
     * Add ourselves to the list of clients using the file.
     */
    UpdateList(consistPtr, clientID, useFlags, *cacheablePtr,
		openTimeStampPtr);
    /*
     * Now that we are all set up, and have told all the other clients
     * using the file what they have to do, we wait for them to finish.
     */
    status = EndConsistency(consistPtr);
    UNLOCK_MONITOR;
    return(status);
}

/*
 * ----------------------------------------------------------------------------
 *
 * StartConsistency --
 *
 *      Initiate cache consistency action on a file.  This decides if the
 *	client can cache the file, and then makes call-backs to other
 *	clients so that caches stay consistent.  EndConsistency
 *	should be called later to wait for the client replies.
 *
 * Results:
 *	*cacheablePtr set to TRUE if the file is cacheable.   *fileBusyPtr
 *	is set to FS_FILE_BUSY if the file is either being opened for execution
 *	and it is already open for writing or vice versa.
 *
 * Side effects:
 *	Sets the FS_CONSIST_IN_PROGRESS flag and makes call-backs to
 *	clients.  The flag is cleared if the call-backs can't be made.
 *
 * ----------------------------------------------------------------------------
 *
 */

INTERNAL void
StartConsistency(consistPtr, clientID, useFlags, cacheablePtr)
    FsConsistInfo	*consistPtr;	/* File's consistency state. */
    int			clientID;	/* ID of host opening the file */
    int			useFlags;	/* Indicates how they are using it */
    Boolean		*cacheablePtr;	/* Return, TRUE if client can cache */
{
    register FsClientInfo *clientPtr;
    register FsClientInfo *nextClientPtr;
    register FsCacheConsistStats *statPtr = &fsConsistStats;
    register int openForWriting = useFlags & FS_WRITE;
    register Boolean cacheable;

    /*
     * Make sure that noone else is in the middle of performing cache
     * consistency on this file.
     */
    while (consistPtr->flags & FS_CONSIST_IN_PROGRESS) {
	(void) Sync_Wait(&consistPtr->consistDone, FALSE);
    }
    consistPtr->flags = FS_CONSIST_IN_PROGRESS;

    /*
     * Determine cacheability of the file.  Note the system-wide switch
     * to disable client caching.  There are other special cases that
     * are not cached:
     *  1. Swap files are not cached on clients.
     *	2. Non-files, (dirs, links) are not cacheable so we don't have to
     *	   worry about keeping them consistent.  (This could be done.)
     *  3. Files that are being concurrently write-shared are not cached.
     *     This includes one writer and one or more readers on other hosts,
     *	   and writers on multiple hosts.
     */
    cacheable = fsClientCaching;
    if ((useFlags & FS_SWAP) && (clientID != rpc_SpriteID)) {
	cacheable = FALSE;
    } else if (((FsLocalFileIOHandle *)consistPtr->hdrPtr)->descPtr->fileType
		    != FS_FILE) {
	cacheable = FALSE;
    }
    if (cacheable) {
	LIST_FORALL(&consistPtr->clientList, (List_Links *)clientPtr) {
	    if (clientPtr->clientID != clientID) {
		if ((clientPtr->use.write > 0) ||
		    ((clientPtr->use.ref > 0) && openForWriting)) {
		    cacheable = FALSE;
		    goto done;
		}
	    }
	}
    }
done:
#ifdef CONSIST_DEBUG
    if (fsTraceConsistMinor == consistPtr->hdrPtr->fileID.minor) {
	printf("File <%d,%d> version %d start consist w/ use 0x%x, %s\n",
		consistPtr->hdrPtr->fileID.major,
		consistPtr->hdrPtr->fileID.minor,
		((FsLocalFileIOHandle *)consistPtr->hdrPtr)->cacheInfo.version,
		useFlags, (cacheable ? "cacheable" : "not cacheable"));
    }
#endif /* CONSIST_DEBUG */
    /*
     * Now that we know the cacheable state of the file, check the use
     * by other clients, perhaps sending them cache consistency
     * messages.  For each message we send out (the client replies
     * right-away without actually doing anything yet) ClientCommand
     * adds an entry to the consistInfo's message list.  Note also that
     * the current client list entry can get removed as side effects of
     * a call-back so we can't use a simple LIST_FOR_ALL here.
     */
    statPtr->numConsistChecks++;
    nextClientPtr = (FsClientInfo *)List_First(&consistPtr->clientList);
    while (!List_IsAtEnd(&consistPtr->clientList, (List_Links *)nextClientPtr)){
	clientPtr = nextClientPtr;
	clientPtr->locked = FALSE;
	nextClientPtr = (FsClientInfo *)List_Next((List_Links *)clientPtr);
	/*
	 * Hang onto the next client element across calls to ClientCommand,
	 * which releases the consistency lock and may allow client list
	 * deletions due to garbage collection.
	 */
	if (!List_IsAtEnd(&consistPtr->clientList,(List_Links *)nextClientPtr)){
	    nextClientPtr->locked = TRUE;
	}
	if (clientPtr->clientID == clientID) {
	    /*
	     * Don't call back to the client doing the open.  That can
	     * cause deadlock.  Instead, that client takes care of its
	     * own cache via the FsCacheUpdate procedure.
	     */
	    continue;
	}
#ifdef CONSIST_DEBUG
	if (fsTraceConsistMinor == consistPtr->hdrPtr->fileID.minor) {
	    printf("Client %d, %s, use %d write %d\n",
		    clientPtr->clientID,
		    (clientPtr->cached ? "caching" : "not caching"),
		    clientPtr->use.ref, clientPtr->use.write);
	}
#endif /* CONSIST_DEBUG */
	statPtr->numClients++;
	if (!clientPtr->cached) {
	    /*
	     * Case 1, the other client isn't caching the file. Do nothing.
	     */
	    statPtr->notCaching++;
	} else if (cacheable) {
	    if (consistPtr->lastWriter != clientPtr->clientID) {
		/*
		 * Case 2, the other client is caching and it's ok.
		 */
		statPtr->readCaching++;
	    } else if (consistPtr->lastWriter == clientID) {
		/*
		 * Case 3, the last writer is now opening for reading.
		 * Its dirty cache is ok.
		 */
		statPtr->writeCaching++;
	    } else {
		/*
		 * Case 4, the last writer needs to give us back the
		 * dirty blocks so the opening client will get good data.
		 */
		ClientCommand(consistPtr, clientPtr,
					FS_WRITE_BACK_BLOCKS);
		statPtr->writeBack++;
	    }
	} else {
	    if ((clientPtr->use.write == 0) && (clientPtr->use.ref > 0)) {
		/*
		 * Case 5, another reader needs to stop caching.
		 */
		ClientCommand(consistPtr, clientPtr,
					FS_INVALIDATE_BLOCKS);
		statPtr->readInvalidate++;
	    } else if (clientPtr->use.write > 0) {
		/*
		 * Case 6, the writer needs to stop caching and give
		 * us back its dirty blocks.
		 */
		ClientCommand(consistPtr, clientPtr,
			    FS_WRITE_BACK_BLOCKS | FS_INVALIDATE_BLOCKS);
		statPtr->writeInvalidate++;
	    }
	}
    }
    *cacheablePtr = cacheable;
}

/*
 * ----------------------------------------------------------------------------
 *
 * UpdateList --
 *
 *	Update the state of the client that is using one of our files.
 *	A timestamp is generated for return to the client so it can
 *	catch races between the open return message and cache consistency
 *	messages associated with this file.
 *
 * Results:
 *	The timestamp.
 *
 * Side effects:
 *	The version number is incremented when open for writing.  The
 *	lastWriter is remembered when opening for writing.  Use counts
 *	are kept to reflect the clients use of the file.  Finally,
 *	client list entries for hosts no longer using or caching the
 *	file are deleted.
 *
 * ----------------------------------------------------------------------------
 */

INTERNAL void
UpdateList(consistPtr, clientID, useFlags, cacheable,
	   openTimeStampPtr)
    register FsConsistInfo *consistPtr;	/* Consistency state for the file. */
    int			clientID;	/* ID of client using the file */
    int			useFlags;	/* FS_READ|FS_WRITE|FS_EXECUTE */
    Boolean		cacheable;	/* TRUE if client is caching the file */
    int			*openTimeStampPtr;/* Generated for the client so it can
					 * catch races between the return from
					 * this open and other cache messages */
{
    register	FsClientInfo	*clientPtr;	/* State for other clients */

    /*
     * Add the client to the I/O handle client list.
     */
    clientPtr = FsIOClientOpen(&consistPtr->clientList, clientID, useFlags,
				cacheable);

    if (cacheable && (useFlags & FS_WRITE)) {
	consistPtr->lastWriter = clientID;
    }
#ifdef CONSIST_DEBUG
    if (fsTraceConsistMinor == consistPtr->hdrPtr->fileID.minor) {
	printf("UpdateList: client %d %s, last writer %d\n",
	    clientID, (clientPtr->cached ? "caching" : "not caching"),
	    consistPtr->lastWriter);
    }
#endif /* CONSIST_DEBUG */
    /*
     * Return a time stamp for the open.  This timestamp is used by clients
     * to catch races between the reply message for an open, and a cache
     * consistency message generated from a different open happening at
     * about the same time.
     */
    if (openTimeStampPtr != (int *)NIL) {
	openTimeStamp++;
	*openTimeStampPtr =
	    consistPtr->openTimeStamp =
		clientPtr->openTimeStamp = openTimeStamp;
    } else {
	consistPtr->openTimeStamp = clientPtr->openTimeStamp = 0;
    }
}


/*
 * ----------------------------------------------------------------------------
 *
 * EndConsistency --
 *	Wait for cache consistency actions to complete.
 *
 * Results:
 *	SUCCESS or FS_NO_DISK_SPACE.  We have to abort an open if a client
 *	cannot write back its cache in response to a consistency message.
 *	This only happens if the disk is full.  If the client is down we
 *	won't wait for it.  If it is not really down we treat it as
 *	down and will abort its attempt to re-open later.
 *
 * Side effects:
 *	Notifies the consistDone condition to allow someone else to
 *	open the file.  Clears the FS_CONSIST_IN_PROGRES flag.
 *
 * ----------------------------------------------------------------------------
 *
 */

INTERNAL ReturnStatus
EndConsistency(consistPtr)
    FsConsistInfo	*consistPtr;
{
    register ReturnStatus status;

    while (!List_IsEmpty(&consistPtr->msgList)) {
	(void) Sync_Wait(&consistPtr->repliesIn, FALSE);
    }
    if (consistPtr->flags & FS_CONSIST_ERROR) {
	/*
	 * The only reason consistency actions fail altogether is when
	 * a client can't do a writeback because there isn't enough
	 * disk space here.  Crashed clients don't matter.
	 */
	status = FS_NO_DISK_SPACE;
    } else {
	status = SUCCESS;
    }
    consistPtr->flags = 0;
    Sync_Broadcast(&consistPtr->consistDone);
    return(status);
}

/*
 * ----------------------------------------------------------------------------
 *
 * FsReopenClient --
 *
 *	Conflict checking has already been done for the re-open, and this
 *	routine updates global use counts and the client list so that
 *	regular opens know the file is being used.  This is called with
 *	the handle locked and it also grabs the consistency monitor lock
 *	to update the client list.
 *	Consistency call-backs are made	later by FsReopenConsistency.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Updates the global use counts of the file, the last writer of
 *	the file, and the client list entry for this client.  Note that
 *	by updating the lastWriter here we may cause a call-back to
 *	the re-opening client during FsReopenConsistency.  This is the
 *	best we can do if another client has slipped in and opened
 *	for reading already.
 *
 * ----------------------------------------------------------------------------
 */
ENTRY void
FsReopenClient(handlePtr, clientID, use, haveDirtyBlocks)
    FsLocalFileIOHandle	*handlePtr;	/* Should be LOCKED. */
    int			clientID;	/* The client who is opening the file.*/
    FsUseCounts		use;		/* Clients usage of the file */
    Boolean 		haveDirtyBlocks;/* TRUE if client expects it to be
					 * cacheable because it has
					 * outstanding dirty blocks. */
{
    register FsConsistInfo	*consistPtr = &handlePtr->consist;
    register FsClientInfo	*clientPtr;
    Boolean			found;

    LOCK_MONITOR;

    found = FALSE;
    LIST_FORALL(&(consistPtr->clientList), (List_Links *)clientPtr) {
	if (clientPtr->clientID == clientID) {
	    found = TRUE;
	    handlePtr->use.ref   += use.ref   - clientPtr->use.ref;
	    handlePtr->use.write += use.write - clientPtr->use.write;
	    handlePtr->use.exec  += use.exec  - clientPtr->use.exec;
	    clientPtr->use = use;
	    clientPtr->cached = haveDirtyBlocks;
	} else if ((clientPtr->use.ref > 0) && haveDirtyBlocks) {
	    /*
	     * Oops, another client is reading this file but the re-opening
	     * client has dirty blocks for it.  We've already checked the
	     * version number so we know another client isn't writing.
	     * We allow this conflict to happen - the regular cache
	     * consistency routines will tell the reading client to
	     * stop caching, and the writing client will write-back
	     * its blocks as soon as possible.  This leaves a window
	     * for inconsistent reads, but the outstanding dirty blocks
	     * are not lost.
	     */
	    printf(
	"FsReopenHandle: file \"%s\" <%d,%d>: client %d reading stale data\n",
		FsHandleName(handlePtr),
		handlePtr->hdr.fileID.major, handlePtr->hdr.fileID.minor,
		clientPtr->clientID);
	}
    }
#ifdef notdef
    if (fsTraceConsistMinor == handlePtr->hdr.fileID.minor) {
	printf("FsReopenClient %d, use %d write %d, %s, last writer %d\n",
		clientID, use.ref, use.write, (found ? "found" : "not found"),
		consistPtr->lastWriter);
    }
#endif /* notdef */

    if (!found) {
	clientPtr = mnew(FsClientInfo);
	clientPtr->clientID = clientID;
	clientPtr->use = use;
	clientPtr->cached = haveDirtyBlocks;
	clientPtr->openTimeStamp = 0;
	List_InitElement((List_Links *) clientPtr);
	List_Insert((List_Links *) clientPtr,
		LIST_ATFRONT(&consistPtr->clientList));

	handlePtr->use.ref   += use.ref;
	handlePtr->use.write += use.write;
	handlePtr->use.exec  += use.exec;
    }
    if (haveDirtyBlocks) {
	if (consistPtr->lastWriter != -1 &&
	    consistPtr->lastWriter != clientID) {
	    /*
	     * Version number checking should have prevented this.
	     */
	    panic(
		    "Client %d with dirty blocks not last writer %d\n",
		    clientID, consistPtr->lastWriter);
	} else {
	    consistPtr->lastWriter = clientID;
	}
    } else if (consistPtr->lastWriter == clientID) {
	consistPtr->lastWriter = -1;
    }
    UNLOCK_MONITOR;
}

/*
 * ----------------------------------------------------------------------------
 *
 * FsReopenConsistency --
 *
 *	Perform cache consistency actions for a file that the client had 
 *	open before we crashed.  This is similar to regular cache consistency,
 *	except that the client list has already been updated by FsReopenClient.
 *	This can result in a call-back to the re-opening client to get
 *	its version of the file.
 *
 * Results:
 *	TRUE if could provide the cacheability expected, FALSE otherwise.
 *
 * Side effects:
 *	*cacheablePtr is TRUE if the file is to be cached by the client.
 *	*openTimeStampPtr is set for use by clients.  They keep this timestamp
 *	in order to catch races between the return from their open request
 *	(which we are part of) and cache consistency messages resulting from
 *	opens occuring at about the same time.
 *
 * ----------------------------------------------------------------------------
 */
ENTRY ReturnStatus
FsReopenConsistency(handlePtr, clientID, use, swap, cacheablePtr, openTimeStampPtr)
    FsLocalFileIOHandle	*handlePtr;	/* Should be UNLOCKED. */
    int			clientID;	/* The client who is opening the file.*/
    FsUseCounts		use;		/* Clients usage of the file */
    int			swap;		/* 0 or FS_SWAP to indicate swap file.*/
    Boolean 		*cacheablePtr;	/* IN: TRUE if client expects it to be
					 * cacheable, i.e. has dirty blocks.
					 * OUT: Cacheability of the file. */
    int			*openTimeStampPtr; /* Place to return a timestamp for 
					 * this open.*/
{
    int				useFlags;
    Boolean			cacheable;
    ReturnStatus		status;
    register FsConsistInfo	*consistPtr = &handlePtr->consist;
    register FsClientInfo	*clientPtr;

    LOCK_MONITOR;

    useFlags = swap;
    if (use.ref > use.write + use.exec) {
	useFlags |= FS_READ;
    }
    if (use.write > 0 || *cacheablePtr) {
	useFlags |= FS_WRITE;
    }
    if (use.exec > 0) {
	useFlags |= FS_EXECUTE;
    }

    if (useFlags == 0) {
	/*
	 * The client isn't using the file anymore so clean up state.
	 */
	LIST_FORALL(&(consistPtr->clientList), (List_Links *)clientPtr) {
	    if (clientPtr->clientID == clientID) {
		List_Remove((List_Links *)clientPtr);
		free((Address)clientPtr);
		break;
	    }
	}
	*openTimeStampPtr = consistPtr->openTimeStamp;
	*cacheablePtr = FALSE;
	status = SUCCESS;
    } else {
	StartConsistency(consistPtr, clientID, useFlags, &cacheable);

	status = FAILURE;
	LIST_FORALL(&consistPtr->clientList, (List_Links *)clientPtr) {
	    if (clientPtr->clientID == clientID) {
		status = SUCCESS;
		break;
	    }
	}
	if (status != SUCCESS) {
	    panic( "FsReopenConsistency, no client entry\n");
	}
	/*
	 * Get a new openTimeStamp so the client can detect races between
	 * the reopen return and consistency messages generated by other
	 * opens happening right now (or real soon).
	 */
	openTimeStamp++;
	*openTimeStampPtr =
	    consistPtr->openTimeStamp =
		clientPtr->openTimeStamp = openTimeStamp;
	/*
	 * Mark the client as caching or not.
	 */
	*cacheablePtr = clientPtr->cached = cacheable;
	if ((useFlags & FS_WRITE) && cacheable) {
	    consistPtr->lastWriter = clientID;
	}
	/*
	 * Wait for cache consistency call-backs to complete.
	 */
	status = EndConsistency(consistPtr);
    }
    UNLOCK_MONITOR;
    return(status);
}

/*
 * ----------------------------------------------------------------------------
 *
 * FsMigrateConsistency --
 *
 *	Shift the references on a file from one client to another.  If
 *	the stream to this handle is not shared accross network this
 *	is done by first removing the useCounts due to the srcClient,
 *	and them performing the regular cache consistency algorithm as
 *	if the dstClient is opening the file.  If the stream is shared
 *	things are more complicated.  We must not close the original
 *	client until all stream references to its I/O handle have migrated,
 *	and we must be careful to not add too many refererences to the
 *	new client.
 *
 * Results:
 *	SUCCESS, unless there is a cache consistency conflict detected.
 *
 * Side effects:
 *	The HANDLE RETURNS UNLOCKED.  Also, full-fledged cache consistency
 *	actions are taken.
 *
 * ----------------------------------------------------------------------------
 */

ENTRY ReturnStatus
FsMigrateConsistency(handlePtr, srcClientID, dstClientID, useFlags, closeSrc,
	cacheablePtr, openTimeStampPtr)
    FsLocalFileIOHandle	*handlePtr;	/* Needs to be UNLOCKED  */
    int			srcClientID;	/* ID of client using the file */
    int			dstClientID;	/* ID of client using the file */
    int			useFlags;	/* FS_READ|FS_WRITE|FS_EXECUTE
					 * FS_RMT_SHARED if shared now.
					 * FS_NEW_STREAM if dstClientID is
					 * getting stream for first time. */
    Boolean		closeSrc;	/* TRUE if should close source client.
					 * This is set by FsStreamMigClient */
    Boolean		*cacheablePtr;	/* Return - Cachability of file */
    int			*openTimeStampPtr;/* Generated for the client so it can
					 * catch races between the return from
					 * this open and other cache messages */
{
    register FsConsistInfo *consistPtr = &handlePtr->consist;
    Boolean			cache;
    register	ReturnStatus	status;

    LOCK_MONITOR;

    cache = (srcClientID == consistPtr->lastWriter);
    if (closeSrc) {
	/*
	 * Remove references due to the original client so it doesn't confuse
	 * the regular cache consistency algorithm.  Note that this call doesn't
	 * disturb the last writer state so any dirty blocks on the old client
	 * will get handled properly.
	 */
	if (!FsIOClientClose(&consistPtr->clientList, srcClientID, useFlags,
		&cache)) {
	    printf(
	"FsMigrateConsistency, srcClient %d unknown for %s %s <%d,%d>\n",
		srcClientID,
		FsFileTypeToString(handlePtr->hdr.fileID.type),
		FsHandleName(handlePtr),
		handlePtr->hdr.fileID.major, handlePtr->hdr.fileID.minor);
	}
    }
    /*
     * The rest of this is like regular cache consistency.
     */

    StartConsistency(consistPtr, dstClientID, useFlags, cacheablePtr);
    if (useFlags & FS_NEW_STREAM) {
	/*
	 * The client is getting the stream to this I/O handle for
	 * the first time so we should add it as a client.  We are
	 * careful about this because there is only one reference to
	 * the I/O handle per client per stream. 
	 */
	UpdateList(consistPtr, dstClientID, useFlags, *cacheablePtr,
		    openTimeStampPtr);
    }
    status = EndConsistency(consistPtr);

    UNLOCK_MONITOR;
    return(status);
}


/*
 * ----------------------------------------------------------------------------
 *
 * FsGetClientAttrs --
 *
 *	This does call-backs to clients to flush back cached attributes.
 *	Files that are being executed are treated as a special case.  They
 *	are not being written so we, the server, have the right size and
 *	modify time.  Instead of getting access times from clients (there
 *	could be lots and lots) we just return a flag that says the
 *	file is being executed.  Our caller will presumably use the
 *	the current time for the access time in this case.
 *
 * Results:
 *	*isExecedPtr set to TRUE if the file is being executed.
 *
 * Side effects:
 *      RPC's may be done to clients using the file to tell them to write
 *	back their cached attributes.
 *	This unlocks the handle while waiting for clients, but
 *	re-locks it before returning.
 *
 * ----------------------------------------------------------------------------
 */
ENTRY void
FsGetClientAttrs(handlePtr, clientID, isExecedPtr)
    register FsLocalFileIOHandle *handlePtr;
    int			clientID;	/* The client who is doing the stat.*/
    Boolean		*isExecedPtr;	/* TRUE if file is being executed.
					 * Our caller will use the current
					 * time for the access time in this
					 * case */
{
    register Boolean		isExeced = FALSE;
    register FsConsistInfo	*consistPtr = &handlePtr->consist;
    register FsClientInfo	*clientPtr;
    register FsClientInfo	*nextClientPtr;

    LOCK_MONITOR;

    /*
     * Unlock the handle so other operations on the file can proceed while
     * we probe around the network for the most up-to-date attributes.
     * Otherwise the following deadlock is possible:
     * Client 1 does a get attributes about the same time client 2 does
     * a close.  Client 2 has its handle locked during the close, but we
     * will be calling back to it to get its attributes.  Our callback can't
     * start until client 2 unlocks its handle, but it can't unlock it's
     * handle until the close finishes.  The close can't finish because
     * it is blocked here on the locked handle.
     */
    FsHandleUnlock(handlePtr);

    /*
     * Make sure that noone else is in the middle of performing cache
     * consistency on this handle.  If so wait until they are done.
     */
    while (consistPtr->flags & FS_CONSIST_IN_PROGRESS) {
	(void) Sync_Wait(&consistPtr->consistDone, FALSE);
    }
    consistPtr->flags = FS_CONSIST_IN_PROGRESS;

    /*
     * Go through the set of clients using the file and see if they
     * are caching attributes.
     */
    nextClientPtr = (FsClientInfo *)List_First(&consistPtr->clientList);
    while (!List_IsAtEnd(&consistPtr->clientList, (List_Links *)nextClientPtr)){
	clientPtr = nextClientPtr;
	clientPtr->locked = FALSE;;
	nextClientPtr = (FsClientInfo *)List_Next((List_Links *)clientPtr);
	/*
	 * Hang onto the next client list element across calls to ClientCommand,
	 * which releases the consistency lock and allows list deletetions.
	 */
	if (!List_IsAtEnd(&consistPtr->clientList,(List_Links *)nextClientPtr)){
	    nextClientPtr->locked = TRUE;
	}
	if (clientPtr->use.exec > 0) {
	    isExeced = TRUE;
	}
	if ((clientPtr->cached) && (clientPtr->use.ref > 0)) {
	    if ((clientPtr->clientID != clientID) &&
		(clientPtr->use.exec == 0)) {
		/*
		 * Don't call back to our caller because it will check
		 * its own attributes later.  Also, don't send rpcs to
		 * clients executing files.  For these clients we just
		 * set the access time to the current time.  This
		 * is an optimization to not make stating of binaries
		 * abysmally slow.
		 */
		ClientCommand(consistPtr, clientPtr, FS_WRITE_BACK_ATTRS);
	    }
	}
    }
    /*
     * Now that we are all set up, and have told all the other clients using
     * the file what they have to do, we wait for them to finish.
     */
    (void)EndConsistency(consistPtr);

    FsHandleLock(handlePtr);
    *isExecedPtr = isExeced;
    UNLOCK_MONITOR;
}

/*
 * ----------------------------------------------------------------------------
 *
 * FsConsistClose --
 *
 *	A thin layer on top of FsIOClientClose that also cleans up
 *	the last writer of a file.
 *
 * Results:
 *	TRUE if there was a record that the client was using the file.
 *	This is used to trap out invalid closes.  Also, *wasCachedPtr
 *	is set to TRUE if the file (in particular, its attributes) was
 *	cached on the client.
 *
 * Side effects:
 *	The client list entry is removed.  If the client was
 *	the last writer but has no dirty blocks (as indicated by the flags)
 *	then the last writer field of the consist info is cleared.
 *
 * ----------------------------------------------------------------------------
 *
 */
ENTRY Boolean
FsConsistClose(consistPtr, clientID, flags, wasCachedPtr)
    register FsConsistInfo *consistPtr;	/* Handle of file being closed */
    int			clientID;	/* Host ID of client that had it open */
    register int	flags;		/* Flags from the stream. */
    Boolean		*wasCachedPtr;	/* TRUE upon return if the client was
					 * caching (attributes) of the file. */
{
    LOCK_MONITOR;

    *wasCachedPtr = (consistPtr->lastWriter == clientID);
#ifdef CONSIST_DEBUG
    if (consistPtr->hdrPtr->fileID.minor == fsTraceConsistMinor) {
	printf("ConsistClose: closing client %d, lastwriter %d\n",
		    clientID, consistPtr->lastWriter);
    }
#endif CONSIST_DEBUG
    if (!FsIOClientClose(&consistPtr->clientList, clientID, flags,
			 wasCachedPtr)) {
	UNLOCK_MONITOR;
	return(FALSE);
    }

    if ((consistPtr->lastWriter != -1) && (flags & FS_LAST_DIRTY_BLOCK)) {
	if (clientID != consistPtr->lastWriter) {
	    printf(
	    "FsConsistClose, \"%s\" <%d,%d>: client %d not last writer %d, %s\n",
		    FsHandleName(consistPtr->hdrPtr),
		    consistPtr->hdrPtr->fileID.major,
		    consistPtr->hdrPtr->fileID.minor,
		    clientID, consistPtr->lastWriter,
		    (*wasCachedPtr) ? "was cached" : "wasn't cached");
	} else {
#ifdef CONSIST_DEBUG
	    if (consistPtr->hdrPtr->fileID.minor == fsTraceConsistMinor) {
		printf("ConsistClose: erasing %d lastwriter\n", clientID);
	    }
#endif CONSIST_DEBUG
	    consistPtr->lastWriter = -1;
	}
    }
    UNLOCK_MONITOR;
    return(TRUE);
}


/*
 * ----------------------------------------------------------------------------
 *
 * FsConsistClients --
 *
 *	Returns the number of clients in the client list for a file.
 *	Called to see if it's ok to scavenge the file handle.
 *
 * Results:
 *	The number of clients in the client list.
 *
 * Side effects:
 *	Unused client list entries are cleaned up.  We only need to remember
 *	clients that are actively using the file or who have dirty blocks
 *	because they are the last writer.
 *
 * ----------------------------------------------------------------------------
 *
 */
ENTRY int
FsConsistClients(consistPtr)
    register FsConsistInfo *consistPtr;	/* Handle of file being closed */
{
    register int numClients = 0;
    register FsClientInfo *clientPtr;
    register FsClientInfo *nextClientPtr;

    LOCK_MONITOR;

    if (consistPtr->flags & FS_CONSIST_IN_PROGRESS) {
	/*
	 * Not safe to mess with list during consistency.
	 */
	UNLOCK_MONITOR;
	return(1);
    }
    nextClientPtr = (FsClientInfo *)List_First(&consistPtr->clientList);
    while (!List_IsAtEnd(&consistPtr->clientList, (List_Links *)nextClientPtr)){
	clientPtr = nextClientPtr;
	nextClientPtr = (FsClientInfo *)List_Next((List_Links *)clientPtr);

	if (clientPtr->use.ref == 0 &&
	    clientPtr->clientID != consistPtr->lastWriter) {
	    if (clientPtr->locked) {
		printf("FsConsistClients hit locked client %d for <%d,%d>\n",
		    clientPtr->clientID, consistPtr->hdrPtr->fileID.major,
		    consistPtr->hdrPtr->fileID.minor);
	    } else {
		List_Remove((List_Links *) clientPtr);
		free((Address) clientPtr);
	    }
	} else {
	    numClients++;
	}
    }
    UNLOCK_MONITOR;
    return(numClients);
}


/*
 * ----------------------------------------------------------------------------
 *
 * FsDeleteLastWriter --
 *
 *	Remove the last writer from the consistency list.  This is called
 *	from the write rpc stub when the last block of a file comes
 *	in from a remote client.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Removes the client list entry for the last writer if the last
 *	writer is no longer using the file.
 *
 * ----------------------------------------------------------------------------
 *
 */
ENTRY void
FsDeleteLastWriter(consistPtr, clientID)
    FsConsistInfo *consistPtr;
    int		clientID;
{
    register	FsClientInfo	*clientPtr;

    LOCK_MONITOR;

    if (consistPtr->flags & FS_CONSIST_IN_PROGRESS) {
	/*
	 * Not safe to mess with list during consistency.  We are called
	 * from Fs_RpcWrite on the client's last block, but we will
	 * delete the last writer in ProcessConsistReply if the write-back
	 * is forced as part of cache consistency.
	 */
	UNLOCK_MONITOR;
	return;
    }
    LIST_FORALL(&consistPtr->clientList, (List_Links *) clientPtr) {
	if (clientPtr->clientID == clientID) {
	    if (clientPtr->use.ref == 0 &&
		consistPtr->lastWriter == clientID) {
#ifdef CONSIST_DEBUG
		if (consistPtr->hdrPtr->fileID.minor == fsTraceConsistMinor) {
		    printf("FsDeleteLastWriter <%d,%d> host %d\n",
			consistPtr->hdrPtr->fileID.major,
			consistPtr->hdrPtr->fileID.minor, clientID);
		}
#endif CONSIST_DEBUG
		if (clientPtr->locked) {
		    printf("FsDeleteLastWriter: locked client %d of <%d,%d>\n",
			clientPtr->clientID, consistPtr->hdrPtr->fileID.major,
			consistPtr->hdrPtr->fileID.minor);
		} else {
		    List_Remove((List_Links  *) clientPtr);
		    free((Address) clientPtr);
		}
		consistPtr->lastWriter = -1;
		break;
	    }
	}
    }
    UNLOCK_MONITOR;
}

/*
 * ----------------------------------------------------------------------------
 *
 * FsClientRemoveCallback --
 *
 *      Called when a file is deleted.  Send an rpc to everyone who has
 *      dirty blocks cached telling them to delete them from their cache.
 *	We may have forgotten about clients with clean blocks so we can't
 *	call them back.  Instead we depend on the version number being
 *	incremented (by 1 when opened for writing, by 2 when creating)
 *	to catch old blocks left in caches.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Any remotely cached dirty data is invalidated.
 *	All the entries in the client list are deleted.
 *
 * ----------------------------------------------------------------------------
 *
 */
ENTRY void
FsClientRemoveCallback(consistPtr, clientID)
    FsConsistInfo *consistPtr;	/* File to check */
    int		clientID;	/* Client who is removing the file.  This
				 * host is not contacted via call-back.
				 * Instead, the current RPC (close) should
				 * return FS_FILE_REMOVED.  Note, a callback
				 * is made during a remove-by-name. */
{
    register	FsClientInfo	*clientPtr;
    int				curClientID;

    LOCK_MONITOR;

    FsHandleUnlock(consistPtr->hdrPtr);

    while (consistPtr->flags & FS_CONSIST_IN_PROGRESS) {
	(void) Sync_Wait(&consistPtr->consistDone, FALSE);
    }
    consistPtr->flags = FS_CONSIST_IN_PROGRESS;

    /*
     * Loop through the list notifying clients and deleting client elements.
     */
    while (!List_IsEmpty((List_Links *)&consistPtr->clientList)) {
	clientPtr = (FsClientInfo *)
		    List_First((List_Links *) &consistPtr->clientList);
	curClientID = clientPtr->clientID;
	if (clientPtr->use.ref > 0) {
	    panic(
		"FsClientRemoveCallback: Client using removed file\n");
	} else if (consistPtr->lastWriter != -1) {
	    if (clientPtr->clientID != consistPtr->lastWriter) {
		printf(
    "FsClientRemoveCallback: \"%s\" <%d,%d> client %d not last writer (%d).\n",
			FsHandleName(consistPtr->hdrPtr),
			consistPtr->hdrPtr->fileID.major,
			consistPtr->hdrPtr->fileID.minor,
			clientPtr->clientID, consistPtr->lastWriter);
	    } else if (clientPtr->clientID != clientID) {
		/*
		 * Tell the client caching the file to remove it from its cache.
		 * This should only be the last writer as we are called only
		 * when it is truely time to remove the file.
		 */
		ClientCommand(consistPtr, clientPtr, FS_DELETE_FILE);
		while (!List_IsEmpty(&consistPtr->msgList)) {
		    (void) Sync_Wait(&consistPtr->repliesIn, FALSE);
		}
	    }
	}
	/*
	 * We have to check carefully that the client element is still
	 * here because it may already be removed.
	 */
	LIST_FORALL((List_Links *)&consistPtr->clientList,
		    (List_Links *)clientPtr) {
	    if (clientPtr->clientID == curClientID) {
		List_Remove((List_Links *) clientPtr);
		free((Address) clientPtr);
		break;
	    }
	}
    }
    consistPtr->flags = 0;
    consistPtr->lastWriter = -1;
    FsHandleLock(consistPtr->hdrPtr);
    UNLOCK_MONITOR;
}

/*
 * ----------------------------------------------------------------------------
 *
 * FsConsistKill --
 *
 *	Find and remove the given client in the list for the handle.  The
 *	number of client references, writers, and executers is returned
 *	so our caller can clean up the reference counts in the handle.
 *
 * Results:
 *	*inUsePtr set to TRUE if the client has the file open, *writingPtr
 *	set to TRUE if the client has the file open for writing, and 
 *	*executingPtr set to TRUE if the client has the file open for
 *	execution.
 *	
 * Side effects:
 *	If this client was the last writer for the file then the last
 *	writer field in the handle is set to -1.
 *
 * ----------------------------------------------------------------------------
 *
 */
ENTRY void
FsConsistKill(consistPtr, clientID, refPtr, writePtr, execPtr)
    FsConsistInfo *consistPtr;	/* Consistency state from which to remove
				 * the client. */
    int		clientID;	/* Client to delete. */
    int		*refPtr;	/* Number of times client has file open. */
    int		*writePtr;	/* Number of times client is writing file. */
    int		*execPtr;	/* Number of times clients is executing file.*/
{
    register ConsistMsgInfo 	*msgPtr;

    LOCK_MONITOR;

    FsIOClientKill(&consistPtr->clientList, clientID, refPtr, writePtr,
		    execPtr);

    if (consistPtr->lastWriter == clientID) {
	consistPtr->lastWriter = -1;
    }
    /*
     * Remove the client from the list of clients involved in a cache
     * consistency action so we don't hang on the dead client.
     */
    LIST_FORALL(&(consistPtr->msgList), (List_Links *) msgPtr) {
	if (msgPtr->clientID == clientID) {
	    List_Remove((List_Links *) msgPtr);
	    free((Address) msgPtr);
	    Sync_Broadcast(&consistPtr->consistDone);
	    break;
	}
    }

    UNLOCK_MONITOR;
}

/*
 *----------------------------------------------------------------------------
 *
 * FsGetAllDirtyBlocks --
 *
 *	Retrieve dirty blocks from all clients that have files open on the
 *	given domain.  This is called when a disk is being detached.  We
 *	have to get any outstanding data before taking the disk off-line.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------------
 */
void
FsGetAllDirtyBlocks(domain, invalidate)
    int		domain;		/* Domain to get dirty blocks for. */
    Boolean	invalidate;	/* Remove file from cache after getting blocks
				 * back. */
{
    Hash_Search				hashSearch;
    register	FsHandleHeader		*hdrPtr;

    Hash_StartSearch(&hashSearch);
    for (hdrPtr = FsGetNextHandle(&hashSearch);
	 hdrPtr != (FsHandleHeader *) NIL;
         hdrPtr = FsGetNextHandle(&hashSearch)) {
	if (hdrPtr->fileID.type == FS_LCL_FILE_STREAM &&
	    hdrPtr->fileID.major == domain) {
	    register FsLocalFileIOHandle *handlePtr =
		    (FsLocalFileIOHandle *) hdrPtr;
	    FsFetchDirtyBlocks(&handlePtr->consist, invalidate);
	}
	FsHandleUnlock(hdrPtr);
    }
}

/*
 * ----------------------------------------------------------------------------
 *
 * FsFetchDirtyBlocks --
 *
 *      Fetch dirty blocks back from the last writer of the file.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	If the last writer doesn't have the file actively open, then it is
 *	no longer the last writer and it is deleted from the client list.
 *
 * ----------------------------------------------------------------------------
 *
 */
ENTRY void
FsFetchDirtyBlocks(consistPtr, invalidate)
    FsConsistInfo *consistPtr;	/* Consistency state for file. */
    Boolean	invalidate;	/* If TRUE the client is told to invalidate
				 * after writing back the blocks */
{
    register	FsClientInfo	*clientPtr;
    register	FsClientInfo	*nextClientPtr;

    LOCK_MONITOR;
    FsHandleUnlock(consistPtr->hdrPtr);

    /*
     * Make sure that noone else is in the middle of performing cache
     * consistency on this handle.  If so wait until they are done.
     */
    while (consistPtr->flags & FS_CONSIST_IN_PROGRESS) {
	(void) Sync_Wait(&consistPtr->consistDone, FALSE);
    }
    /*
     * See if there are any dirty blocks to fetch.
     */
    if (consistPtr->lastWriter == -1 ||
	consistPtr->lastWriter == rpc_SpriteID) {
	FsHandleLock(consistPtr->hdrPtr);
        UNLOCK_MONITOR;
	return;
    }
    consistPtr->flags = FS_CONSIST_IN_PROGRESS;

    /*
     * There is a last writer.  In this case the only thing on the list is
     * the client that is the last writer because we know the domain for
     * the file is in-active.
     */
    nextClientPtr = (FsClientInfo *)List_First(&consistPtr->clientList);
    while (!List_IsAtEnd(&consistPtr->clientList, (List_Links *)nextClientPtr)){
	clientPtr = nextClientPtr;
	clientPtr->locked = FALSE;
	nextClientPtr = (FsClientInfo *)List_Next((List_Links *)clientPtr);
	if (!List_IsAtEnd(&consistPtr->clientList,(List_Links *)nextClientPtr)){
	    nextClientPtr->locked = TRUE;
	}
	if (clientPtr->clientID != consistPtr->lastWriter) {
	    FsHandleLock(consistPtr->hdrPtr);
	    consistPtr->flags = 0;
	    UNLOCK_MONITOR;
	    panic("FsFetchDirtyBlocks: Non last writer in list.\n");
	    return;
	} else if (clientPtr->use.write > 0) {
	    /*
	     * The client has it actively open for writing.  In this case don't
	     * do anything because he can have more dirty blocks anyway.
	     */
	} else {
	    register int flags = FS_WRITE_BACK_BLOCKS;
	    if (invalidate) {
		flags |= FS_INVALIDATE_BLOCKS;
	    }
	    ClientCommand(consistPtr, clientPtr, flags);
	    (void)EndConsistency(consistPtr);
	}
    }
    consistPtr->flags = 0;
    FsHandleLock(consistPtr->hdrPtr);
    UNLOCK_MONITOR;
    return;
}

/*
 * The consistency messages are issued in two parts.  In the first phase
 * the server issues all the clients commands, but the clients return
 * an RPC reply immediately and schedule ProcessConsist in the background
 * to actually effect the cache consistency.  The second phase consists
 * of the server waiting around for a return RPC by the client that
 * indicates that it is done.
 */
void	ProcessConsist();
void	ProcessConsistReply();
/*
 * Consist message information.
 */
typedef struct ConsistItem {
    int		serverID;
    ConsistMsg args;
} ConsistItem;

/*
 * ----------------------------------------------------------------------------
 *
 * ClientCommand --
 *
 *	Send an rpc to a client telling him to perform some cache 
 *	consistency operation.  Also put the client onto a list of 
 *	outstanding cache consistency messages.
 *
 * Results:
 *	The status from the RPC.
 *
 * Side effects:
 *      Client added to list of outstanding cache consistency messages.
 *
 * ----------------------------------------------------------------------------
 *
 */

INTERNAL void
ClientCommand(consistPtr, clientPtr, flags)
    register FsConsistInfo *consistPtr;	/* Consistency state of file */
    FsClientInfo	*clientPtr;	/* State of other client's cache */
    int			flags;		/* Command for the other client */
{
    Rpc_Storage		storage;
    ConsistMsg		consistRpc;
    ReturnStatus	status;
    ConsistMsgInfo	*msgPtr;
    int			numRefusals;

    if (clientPtr->clientID == rpc_SpriteID) {
	/*
	 * Don't issue commands to ourselves (the server) because the commands
	 * issued to the other clients (write-back, invalidate, etc.) will
	 * all result in a consistent server cache anyway.
	 */
	if (flags & FS_INVALIDATE_BLOCKS) {
	    /*
	     * If we told ourselves to invalidate the file then mark us
	     * as not caching the file.
	     */
	    clientPtr->cached = FALSE;
	}
	if (flags & FS_WRITE_BACK_BLOCKS) {
	    /*
	     * We already have the most recent blocks.
	     */
	    consistPtr->lastWriter = -1;
	}
	if (clientPtr->use.ref == 0 && 
	    consistPtr->lastWriter != clientPtr->clientID) {
	    FS_CACHE_DEBUG_PRINT1("ClientCommand: Removing %d ",
					clientPtr->clientID);
	    List_Remove((List_Links *) clientPtr);
	    free((Address) clientPtr);
	}
	return;
    }
    /*
     * Map to the client's view of the file (i.e. remote).
     */
    consistRpc.fileID = consistPtr->hdrPtr->fileID;
    if (consistRpc.fileID.type != FS_LCL_FILE_STREAM) {
	    panic( "ClientCommand, bad stream type <%d>\n",
		consistRpc.fileID.type);
    } else {
	consistRpc.fileID.type = FS_RMT_FILE_STREAM;
    }
    /*
     * The openTimeStamp lets the client catch races between this message
     * and the reply to an open it may be making at the same time.
     */
    consistRpc.flags = flags;
    consistRpc.openTimeStamp = clientPtr->openTimeStamp;
    consistRpc.version =
	((FsLocalFileIOHandle *)consistPtr->hdrPtr)->cacheInfo.version;

    storage.requestParamPtr = (Address) &consistRpc;
    storage.requestParamSize = sizeof(ConsistMsg);
    storage.requestDataPtr = (Address) NIL;
    storage.requestDataSize = 0;

    storage.replyParamPtr = (Address) NIL;
    storage.replyParamSize = 0;
    storage.replyDataPtr = (Address) NIL;
    storage.replyDataSize = 0;

    /*
     * Put the client onto the list of outstanding cache consistency
     * messages.
     */

    msgPtr = (ConsistMsgInfo *) malloc(sizeof(ConsistMsgInfo));
    msgPtr->clientID = clientPtr->clientID;
    msgPtr->flags = consistRpc.flags;
    List_Insert((List_Links *) msgPtr, LIST_ATREAR(&consistPtr->msgList));

    /*
     * Have to release this monitor during the call-back so that
     * an unrelated close can complete its call to FsConsistClose.
     * Alternatives are to fix the consistency call-back RPC stubs
     * so they don't lock the handle.
     */
    if ((consistPtr->flags & FS_CONSIST_IN_PROGRESS) == 0) {
	panic( "Client CallBack - consist flag not set\n");
    }
    UNLOCK_MONITOR;
    numRefusals = 0;
    while ( TRUE ) {
	/*
	 * Send the rpc to the client.  The client will return FAILURE if the
	 * open that we are talking about hasn't returned to the client yet.
	 */
	status = Rpc_Call(clientPtr->clientID, RPC_FS_CONSIST, &storage);
	if (status != FAILURE) {
	    break;
	} else {
	    numRefusals++;
	    if (numRefusals > 30) {
		printf(
		    "Client %d dropped 30 %s requests for \"%s\" <%d,%d>\n",
			    clientPtr->clientID, ConsistType(flags),
			    FsHandleName(consistPtr->hdrPtr),
			    consistRpc.fileID.major, consistRpc.fileID.minor);
		consistRpc.flags |= FS_DEBUG_CONSIST;
		numRefusals = 0;
	    } else {
		consistRpc.flags &= ~FS_DEBUG_CONSIST;
	    }
	}
    }
    if (status != SUCCESS) {
	/*
	 * Couldn't post call-back to the client.  Nuke it from the
	 * list of clients using the file.
	 */
	int ref, write, exec;
	int clientID = clientPtr->clientID;
	FsConsistKill(consistPtr, clientPtr->clientID, &ref, &write, &exec);
	printf("ClientCommand, %s msg to client %d file \"%s\" <%d,%d> failed %x\n",
	    ConsistType(flags), clientID, FsHandleName(consistPtr->hdrPtr),
	    consistRpc.fileID.major, consistRpc.fileID.minor, status);
	printf("\tClient had %d refs %d write %d exec\n", ref, write, exec);
    }
    LOCK_MONITOR;
}

/*
 *----------------------------------------------------------------------
 *
 * Fs_RpcConsist --
 *
 *	Service stub for RPC_FS_CONSIST.  This is executed on a filesystem
 *	client in response to a cache consistency command.  This schedules
 *	a call to ProcessConsist and returns a reply to the server.
 *	If, by remote chance, the message concerns an in-progress open
 *	that hasn't yet completed, we just return an error code to the
 *	server so that it will re-try.
 *
 * Results:
 *	SUCCESS.
 *
 * Side effects:
 *	Add work to the queue for the client consistency process.
 *
 *----------------------------------------------------------------------
 */
/*ARGSUSED*/
ReturnStatus
Fs_RpcConsist(srvToken, clientID, command, storagePtr)
    ClientData srvToken;	/* Handle on server process passed to
				 * Rpc_Reply */
    int clientID;		/* ID of server controlling the file */
    int command;		/* Command identifier */
    Rpc_Storage *storagePtr;	/* The request fields refer to the request
				 * buffers and also indicate the exact amount
				 * of data in the request buffers.  The reply
				 * fields are initialized to NIL for the
				 * pointers and 0 for the lengths.  This can
				 * be passed to Rpc_Reply */
{
    register ConsistMsg	*consistArgPtr;
    register ConsistItem	*consistPtr;
    register FsRmtFileIOHandle	*rmtHandlePtr;
    register ReturnStatus	status;

    consistArgPtr = (ConsistMsg *)storagePtr->requestParamPtr;
    if (consistArgPtr->fileID.type != FS_RMT_FILE_STREAM) {
	panic( "Fs_RpcConsist, bad stream type <%d>\n",
		    consistArgPtr->fileID.type);
    }
    /*
     * This fetch locks the handle.  In earlier versions of the kernel
     * this could cause deadlock.  This may still be true.  3/9/88.
     */
    rmtHandlePtr = FsHandleFetchType(FsRmtFileIOHandle, &consistArgPtr->fileID);
    if (rmtHandlePtr == (FsRmtFileIOHandle *)NIL) {
	if (FsPrefixOpenInProgress(&consistArgPtr->fileID) == 0) {
	    status = FS_STALE_HANDLE;
	    printf(
		    "Fs_RpcConsist: <%d,%d> %s msg from %d dropped: %s\n",
		    consistArgPtr->fileID.major,
		    consistArgPtr->fileID.minor,
		    ConsistType(consistArgPtr->flags),
		    consistArgPtr->fileID.serverID,
		    "no handle");
	} else {
	    /*
	     * A consistency message has arrived from an open from which
	     * we haven't received the reply.  Return FAILURE to force
	     * the server to resend and give the open reply a chance
	     * of getting to us.  This is the "open/consistency race".
	     */
	    status = FAILURE;
#ifdef notdef
	    printf(
		    "Fs_RpcConsist: <%d,%d> %s msg from %d dropped: %s\n",
		    consistArgPtr->fileID.major,
		    consistArgPtr->fileID.minor,
		    ConsistType(consistArgPtr->flags),
		    consistArgPtr->fileID.serverID,
		    "open in progress");
#endif
	}
    } else if (rmtHandlePtr->openTimeStamp != consistArgPtr->openTimeStamp) {
	if (FsPrefixOpenInProgress(&consistArgPtr->fileID) == 0) {
	    status = FS_STALE_HANDLE;
	    printf(
		"Fs_RpcConsist: <%d,%d> %s msg from %d timestamp %d not %d, %s\n",
		    consistArgPtr->fileID.major,
		    consistArgPtr->fileID.minor,
		    ConsistType(consistArgPtr->flags),
		    consistArgPtr->fileID.serverID,
		    consistArgPtr->openTimeStamp, rmtHandlePtr->openTimeStamp,
		    "stale handle");
	} else {
	    /*
	     * Possible open/consistency race.
	     */
	    status = FAILURE;
#ifdef notdef
	    printf(
		"Fs_RpcConsist: <%d,%d> %s msg from %d timestamp %d not %d, %s\n",
		    consistArgPtr->fileID.major,
		    consistArgPtr->fileID.minor,
		    ConsistType(consistArgPtr->flags),
		    consistArgPtr->fileID.serverID,
		    consistArgPtr->openTimeStamp, rmtHandlePtr->openTimeStamp,
		    "open in progress");
#endif
	}
    } else {
	status = SUCCESS;
    }

    if (rmtHandlePtr != (FsRmtFileIOHandle *)NIL) {
	FsHandleRelease(rmtHandlePtr, TRUE);
    }
    if (status == SUCCESS) {
	/*
	 * This is a message corresponding to our current notion of the
	 * file.  Pass the message to a consistency handler process.
	 */
	consistPtr = (ConsistItem *) malloc(sizeof(ConsistItem));
	consistPtr->serverID = clientID;
	consistPtr->args = *consistArgPtr;
	Proc_CallFunc(ProcessConsist, (ClientData) consistPtr, 0);
    } else if (consistArgPtr->flags & FS_DEBUG_CONSIST) {
	printf(
	    "Fs_RpcConsist: <%d,%d> Lots of %s msgs dropped: %s\n",
		    consistArgPtr->fileID.major,
		    consistArgPtr->fileID.minor,
		    ConsistType(consistArgPtr->flags),
		    (status == FS_STALE_HANDLE) ? "stale handle" :
						  "open in progress");
    }
    Rpc_Reply(srvToken, status, storagePtr, (int(*)())NIL, (ClientData)NIL);
    return(SUCCESS);
}


/*
 * ----------------------------------------------------------------------------
 *
 * ProcessConsist --
 *
 *	Process a client's cache consistency request.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The cache may be flushed or invalidated.
 *
 * ----------------------------------------------------------------------------
 *
 */
void
ProcessConsist(data, callInfoPtr)
    ClientData		data;
    Proc_CallInfo	*callInfoPtr;
{
    register FsRmtFileIOHandle 	*handlePtr;
    ReturnStatus		status;
    Rpc_Storage			storage;
    ConsistReply		reply;
    register	ConsistItem	*consistPtr;

    consistPtr = (ConsistItem *) data;
    callInfoPtr->interval = 0;

    if (consistPtr->args.fileID.type != FS_RMT_FILE_STREAM) {
	panic( "ProcessConsist, unexpected file type %d\n",
	    consistPtr->args.fileID.type);
	return;
    }
    handlePtr = FsHandleFetchType(FsRmtFileIOHandle, &consistPtr->args.fileID);
    if (handlePtr == (FsRmtFileIOHandle *)NIL) {
	printf("<%d, %d, %d, %d>:", consistPtr->args.fileID.type,
		     consistPtr->args.fileID.serverID,
		     consistPtr->args.fileID.major,
		     consistPtr->args.fileID.minor);
	printf( "ProcessConsist: lost the handle\n");
	return;
    }
    FsHandleUnlock(handlePtr);

    FS_CACHE_DEBUG_PRINT2("ProcessConsist: Got %s request for file %d\n", 
	ConsistType(consistPtr->args.flags), handlePtr->rmt.hdr.fileID.minor);

    /*
     * Process the request under the per file cache lock.
     */
    reply.status = FsCacheConsist(&handlePtr->cacheInfo, consistPtr->args.flags,
			    &reply.cachedAttr);
#ifdef CONSIST_DEBUG
    if (fsTraceConsistMinor == handlePtr->rmt.hdr.fileID.minor) {
	printf(
	"ProcessConsist, %s msg for file \"%s\" <%d,%d> version %d status %x\n",
	    ConsistType(consistPtr->args.flags), 
	    FsHandleName(handlePtr),
	    handlePtr->rmt.hdr.fileID.major, handlePtr->rmt.hdr.fileID.minor,
	    consistPtr->args.version, reply.status);
    }
#endif /* CONSIST_DEBUG */

    FS_CACHE_DEBUG_PRINT2("Returning: mod (%d), acc (%d),",
		       reply.cachedAttr.modifyTime,
		       reply.cachedAttr.accessTime);
    FS_CACHE_DEBUG_PRINT2(" first (%d), last (%d)\n",
		       reply.cachedAttr.firstByte, reply.cachedAttr.lastByte);
    reply.fileID = handlePtr->rmt.hdr.fileID;
    reply.fileID.type = FS_LCL_FILE_STREAM;
    FsHandleRelease(handlePtr, FALSE);
    /*
     * Set up the reply buffer.
     */
    storage.requestParamPtr = (Address)&reply;
    storage.requestParamSize = sizeof(ConsistReply);
    storage.requestDataPtr = (Address)NIL;
    storage.requestDataSize = 0;
    storage.replyParamPtr = (Address)NIL;
    storage.replyParamSize = 0;
    storage.replyDataPtr = (Address)NIL;
    storage.replyDataSize = 0;

    status = Rpc_Call(consistPtr->serverID, RPC_FS_CONSIST_REPLY, &storage);
    if (status != SUCCESS) {
	printf("Got error (%x) from consist reply on <%d,%d>\n", status,
	    reply.fileID.major, reply.fileID.minor);
    }
    free((Address)consistPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * Fs_RpcConsistReply --
 *
 *	Service stub for RPC_FS_CONSIST_REPLY.  This indicates that
 *	the client has handled the consistency command.
 *
 * Results:
 *	SUCCESS.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
/*ARGSUSED*/
ReturnStatus
Fs_RpcConsistReply(srvToken, clientID, command, storagePtr)
    ClientData srvToken;	/* Handle on server process passed to
				 * Rpc_Reply */
    int clientID;		/* Sprite ID of client host */
    int command;		/* Command identifier */
    Rpc_Storage *storagePtr;    /* The request fields refer to the request
				 * buffers and also indicate the exact amount
				 * of data in the request buffers.  The reply
				 * fields are initialized to NIL for the
				 * pointers and 0 for the lengths.  This can
				 * be passed to Rpc_Reply */
{
    FsLocalFileIOHandle	*handlePtr;
    ConsistReply	*replyPtr;

    replyPtr = (ConsistReply *) storagePtr->requestParamPtr;
    if (replyPtr->fileID.type != FS_LCL_FILE_STREAM) {
	panic( "Fs_RpcConsistReply bad stream type <%d>\n",
	    replyPtr->fileID.type);
    }
    handlePtr = FsHandleFetchType(FsLocalFileIOHandle, &(replyPtr->fileID));
    if (handlePtr == (FsLocalFileIOHandle *) NIL) {
	printf("Fs_RpcConsistReply: no handle <%d,%d> for client %d\n",
	    replyPtr->fileID.major, replyPtr->fileID.minor, clientID);
	return(FS_STALE_HANDLE);
    }
    ProcessConsistReply(&handlePtr->consist, clientID, replyPtr);
    FsHandleRelease(handlePtr, TRUE);
    Rpc_Reply(srvToken, SUCCESS, storagePtr, 
	      (int (*)())NIL, (ClientData)NIL);
}


/*
 *----------------------------------------------------------------------
 *
 * ProcessConsistReply --
 *
 *	Process the reply sent by the client for the given handle.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Element deleted from the list of outstanding client consist 
 *	messages.  Also if the message was for invalidation then the
 *	client list entry is marked as non-cacheable.  IMPORTANT:
 *	client list entry may be REMOVED here which prevents safe
 *	use of LIST_FORALL iteration over the client list by any
 *	routine that calls ClientCommand (which eventually gets us called)
 *
 *----------------------------------------------------------------------
 */
ENTRY void
ProcessConsistReply(consistPtr, clientID, replyPtr)
    FsConsistInfo		*consistPtr;	/* File to process reply for*/
    int				clientID;	/* Client who sent us the 
						 * reply. */
    register	ConsistReply	*replyPtr;	/* The reply that was sent. */
{
    register	ConsistMsgInfo		*msgPtr;
    register	FsClientInfo 		*clientPtr;
    Boolean				found = FALSE;
    FsLocalFileIOHandle			*handlePtr;

    LOCK_MONITOR;

    /*
     * Find the client in the list of pending messages.  Notify the
     * opening process after all the clients have responded.
     */
    LIST_FORALL(&(consistPtr->msgList), (List_Links *) msgPtr) {
	if (msgPtr->clientID == clientID) {
	    List_Remove((List_Links *) msgPtr);
	    found = TRUE;
	    break;
	}
    }
    if (List_IsEmpty(&(consistPtr->msgList))) {
	Sync_Broadcast(&consistPtr->repliesIn);
    }
    if (!found) {
	/*
	 * Old message from the client, probably queued in the network
	 * interface or from a gateway.
	 */
	printf( "ProcessConsistReply: Client %d not found\n",
			clientID);
	UNLOCK_MONITOR;
	return;
    }
#ifdef CONSIST_DEBUG
    if (fsTraceConsistMinor == consistPtr->hdrPtr->fileID.minor) {
	printf(
	"ConsistReply, %s msg for file \"%s\" <%d,%d> writer %d status %x\n",
	    ConsistType(msgPtr->flags), 
	    FsHandleName(consistPtr->hdrPtr),
	    consistPtr->hdrPtr->fileID.major, consistPtr->hdrPtr->fileID.minor,
	    consistPtr->lastWriter,
	    replyPtr->status);
    }
#endif /* CONSIST_DEBUG */
    if (replyPtr->status != SUCCESS) {
	printf(
	    "ProcessConsist: %s request failed <%x> file \"%s\" <%d,%d>\n",
		ConsistType(msgPtr->flags), replyPtr->status,
		FsHandleName(consistPtr->hdrPtr),
		consistPtr->hdrPtr->fileID.major,
		consistPtr->hdrPtr->fileID.minor);
	consistPtr->flags |= FS_CONSIST_ERROR;
    } else {
	if ((msgPtr->flags & FS_WRITE_BACK_BLOCKS) &&
	    (consistPtr->lastWriter == clientID)) {
	    /*
	     * We just got the most recent blocks so we don't care who the
	     * last writer is anymore.
	     */
	    consistPtr->lastWriter = -1;
	}
	/*
	 * Look for client list entry that match the client, and update
	 * its state to reflect the consistency action by the client.
	 * Because we've release the monitor lock during the previous
	 * call-back, an unrelated action may have removed the client
	 * from the list - no big deal.
	 */
	LIST_FORALL(&(consistPtr->clientList), (List_Links *) clientPtr) {
	    if (clientPtr->clientID == clientID) {
		handlePtr = (FsLocalFileIOHandle *)consistPtr->hdrPtr;

		if (msgPtr->flags & (FS_WRITE_BACK_BLOCKS |
				     FS_WRITE_BACK_ATTRS)) {
		    FsUpdateAttrFromClient(clientID, &handlePtr->cacheInfo,
			&replyPtr->cachedAttr);
		}
		
		if (clientPtr->use.ref == 0 &&
		    consistPtr->lastWriter != clientID) {
		    List_Remove((List_Links *) clientPtr);
		    free((Address) clientPtr);
		} else if (msgPtr->flags & FS_INVALIDATE_BLOCKS) {
		    clientPtr->cached = FALSE;
		}
		break;
	    }
	}
    }
    free((Address) msgPtr);

    UNLOCK_MONITOR;
}

/*
 *----------------------------------------------------------------------
 *
 * ConsistType --
 *
 *	Utility routine to map from consistency flags to a printable string.
 *
 * Results:
 *	A pointer to a printable string.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

char *
ConsistType(flags)
    int flags;		/* Cache consistency message flags */
{
    register char *result;
    switch (flags & ~FS_DEBUG_CONSIST) {
	case FS_WRITE_BACK_BLOCKS:
	    result = "write-back";
	    break;
	case FS_INVALIDATE_BLOCKS:
	    result = "invalidate";
	    break;
	case (FS_WRITE_BACK_BLOCKS|FS_INVALIDATE_BLOCKS):
	    result = "write-back & invalidate";
	    break;
	case FS_DELETE_FILE:
	    result = "delete";
	    break;
	case FS_WRITE_BACK_ATTRS:
	    result = "return-attrs";
	    break;
    }
    return(result);
}
