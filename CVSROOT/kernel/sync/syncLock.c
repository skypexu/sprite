/* 
 * syncLock.c --
 *
 *	These are internal locking routines of the Synchronization module.
 *	These routines are slower but safer versions of the routines (found
 *	in syncMonitor.c) to get and release monitor locks, and to wait on
 *	and notify condition variables.
 *
 *	A process is blocked by making it wait on an event.  An event is
 *	just an uninterpreted integer that gets 'signaled' by the routine
 *	Sync_SlowBroadcast.
 *
 * Copyright 1985 Regents of the University of California
 * All rights reserved.
 */

#ifndef lint
static char rcsid[] = "$Header$ SPRITE (Berkeley)";
#endif not lint

#include "sprite.h"
#include "list.h"
#include "sync.h"
#include "syncInt.h"
#include "sched.h"
#include "proc.h"
#include "byte.h"
#include "timer.h"
#include "rpc.h"
#include "vmMach.h"

/*
 * A counter to record the number of busy wait loops executed
 * while trying to P a semaphore.  This is incremented inside the
 * loop of MASTER_LOCK.
 */
int sync_BusyWaits = 0;

/*
 * A counter to record the number of events that had clashes on the hash.
 */

int	sync_Collisions = 0;

/*
 * The event hash chain.  Process control blocks are placed in a hash
 * chain keyed on the event that the process is waiting on.
 */

#define PROC_HASHBUCKETS	63
static List_Links eventChainHeaders[PROC_HASHBUCKETS];

/*
 * Instrumentation to record the number of calls to the wakeup routine
 * and to record how many processes were woken up.
 */

Sync_Instrument sync_Instrument;

/*
 * Statistics related to remote waiting.
 */
int syncProcWakeupRaces = 0;

static void	ProcessWakeup();



/*
 *----------------------------------------------------------------------------
 *
 * Sync_Init --
 *
 *	This initializes the event hash chain.  The hash table is
 *	an array of list headers.
 *
 *	Instrumentation variables are also initialized.
 *
 * Results:
 *     None.
 *
 * Side effects:
 *	The hash chain headers are initialized to be dummy list elements
 *
 *----------------------------------------------------------------------------
 */
void
Sync_Init()
{
    register int i;

    for (i=0 ; i<PROC_HASHBUCKETS ; i++) {
	List_Init(&eventChainHeaders[i]);
    }
    Byte_Zero(sizeof(sync_Instrument), (Address) &sync_Instrument);
}


/*
 *----------------------------------------------------------------------------
 *
 * Sync_SlowLock --
 *
 *	Acquire a lock while holding the synchronization master lock.
 *
 *      Inside the critical section the inUse bit is checked.  If we have
 *      to wait the process is put to sleep waiting on an event associated
 *      with the lock.
 *
 * Results:
 *	SUCCESS		is always returned.
 *
 * Side effects:
 *      The lock is acquired when this procedure returns.  The process may
 *      have been put to sleep while waiting for the lock to become
 *      available.
 *
 *----------------------------------------------------------------------------
 */

ENTRY ReturnStatus
Sync_SlowLock(lockPtr)
    register	Sync_Lock	*lockPtr;
{
    MASTER_LOCK(sched_Mutex);

    while (Mach_TestAndSet(&(lockPtr->inUse)) != 0) {
	lockPtr->waiting = TRUE;
	(void) SyncEventWaitInt((unsigned int)lockPtr, FALSE);
	MASTER_UNLOCK(sched_Mutex);
	VmMach_SetupContext(Proc_GetCurrentProc(Sys_GetProcessorNumber()));
	MASTER_LOCK(sched_Mutex);
    }

    MASTER_UNLOCK(sched_Mutex);
    return(SUCCESS);
}


/*
 *----------------------------------------------------------------------------
 *
 * Sync_SlowWait --
 *
 *      Wait on a condition.  The lock is released and the process is blocked
 *      on the event.  A future call to SyncSlowBroadcast will signal the
 *      condition and make this process runnable again.  Before returning
 *	the lock is reaquired.
 *
 *      This can only be called while a lock is held.  This forces our
 *      client to safely check global state while in a monitor.
 *
 * Results:
 *	TRUE if interrupted because of signal, FALSE otherwise.
 *
 * Side effects:
 *      Put the process to sleep and release the monitor lock.  Other
 *      processes waiting on the monitor lock become runnable.
 *
 *----------------------------------------------------------------------------
 */

ENTRY Boolean
Sync_SlowWait(conditionPtr, lockPtr, wakeIfSignal)
    Sync_Condition	*conditionPtr;	/* Condition to wait on. */
    register Sync_Lock 	*lockPtr;	/* Lock to release. */
    Boolean		wakeIfSignal;	/* TRUE => wake if signal pending. */
{
    Boolean	sigPending;

    conditionPtr->waiting = TRUE;

    MASTER_LOCK(sched_Mutex);
    /*
     * release the monitor lock and wait on the condition
     */
    lockPtr->inUse = 0;
    lockPtr->waiting = FALSE;
    SyncEventWakeupInt((unsigned int)lockPtr);
    sigPending = SyncEventWaitInt((unsigned int) conditionPtr, wakeIfSignal);
    MASTER_UNLOCK(sched_Mutex);
    VmMach_SetupContext(Proc_GetCurrentProc(Sys_GetProcessorNumber()));

    Sync_GetLock(lockPtr);

    return(sigPending);
}


/*
 *----------------------------------------------------------------------------
 *
 * Sync_SlowBroadcast --
 *
 *      Mark all processes waiting on an event as runable.  The flag that
 *      indicates there are waiters is cleared here inside the protected
 *      critical section.  This has "broadcast" semantics because everyone
 *      waiting is made runable.  We don't yet have a mechanism to wake up
 *      just one waiting process.
 *
 * Results:
 *	SUCCESS		is always returned.
 *
 * Side effects:
 *	Make processes waiting on the event runnable.
 *
 *----------------------------------------------------------------------------
 */

ENTRY ReturnStatus
Sync_SlowBroadcast(event, waitFlagPtr)
    unsigned int event;
    int *waitFlagPtr;
{
    MASTER_LOCK(sched_Mutex);

    *waitFlagPtr = FALSE;
    SyncEventWakeupInt(event);

    MASTER_UNLOCK(sched_Mutex);
    return(SUCCESS);
}


/*
 *----------------------------------------------------------------------------
 *
 * Sync_SlowMasterWait --
 *
 *      Wait on an event.  Like SyncSlowWait except that the lock that
 *	is released is a master lock, not a monitor lock.
 *
 * Results:
 *	TRUE if wake up because of a signal, FALSE otherwise.
 *
 * Side effects:
 *      Put the process to sleep and release the master lock.  Other
 *      processes waiting on the monitor lock become runnable.
 *	
 *
 *----------------------------------------------------------------------------
 */

ENTRY Boolean
Sync_SlowMasterWait(event, mutexPtr, wakeIfSignal)
    unsigned int 	event;		/* Event to wait on. */
    int 		*mutexPtr;	/* Mutex to release and reaquire. */
    Boolean 		wakeIfSignal;	/* TRUE => wake if signal pending. */
{
    Boolean	sigPending;

    MASTER_LOCK(sched_Mutex);

    /*
     * release the master lock and wait on the condition
     */
    MASTER_UNLOCK(*mutexPtr);

    sigPending = SyncEventWaitInt(event, wakeIfSignal);

    MASTER_UNLOCK(sched_Mutex);
    VmMach_SetupContext(Proc_GetCurrentProc(Sys_GetProcessorNumber()));
    /*
     * re-acquire master lock before proceeding
     */
    MASTER_LOCK(*mutexPtr);

    return(sigPending);
}


/*
 *----------------------------------------------------------------------------
 *
 * Sync_UnlockAndSwitch --
 *
 *      Release the monitor lock and then perform a context switch to the
 *	given state.  The monitor lock is reaquired before this routine 
 *	returns.
 *
 * Results:
 *	SUCCESS		is always returned.
 *
 * Side effects:
 *      Context switch the process and release the monitor lock.  Other
 *      processes waiting on the monitor lock become runnable.
 *	
 *----------------------------------------------------------------------------
 */

void
Sync_UnlockAndSwitch(lockPtr, state)
    register	Sync_Lock 	*lockPtr;
    Proc_State			state;
{
    MASTER_LOCK(sched_Mutex);
    /*
     * release the monitor lock and context switch.
     */
    lockPtr->inUse = 0;
    lockPtr->waiting = FALSE;
    SyncEventWakeupInt((unsigned int)lockPtr);
    Sched_ContextSwitchInt(state);

    MASTER_UNLOCK(sched_Mutex);
    VmMach_SetupContext(Proc_GetCurrentProc(Sys_GetProcessorNumber()));
}


/*
 *----------------------------------------------------------------------------
 *
 * SyncEventWakeupInt --
 *
 *      This looks through the process table for processes waiting on an
 *      event.  For each one it finds it clears its event and marks the
 *      process runnable.  Blocked processes are placed on a hash chain
 *	keyed on the event they are blocked on.  It is this hash chain
 *	that this procedure scans.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      removes process table entries from their event hash chain and
 *      marks them runnable.
 *
 *----------------------------------------------------------------------------
 */

INTERNAL void
SyncEventWakeupInt(event)
    unsigned int event;
{
    register	Proc_ControlBlock 	*procPtr;
    register	Proc_PCBLink 		*hashChainItemPtr;
    register	List_Links 		*chainHeader;
    register	List_Links		*itemPtr;

    if (!sched_Mutex) {
	Sys_Panic(SYS_FATAL, "SyncEventWakeupInt: master lock not held.\n");
    }
    
    sync_Instrument.numWakeupCalls++;
    chainHeader = &eventChainHeaders[event % PROC_HASHBUCKETS];

    itemPtr = List_First(chainHeader);
    while (!List_IsAtEnd(chainHeader, itemPtr)) {
	hashChainItemPtr = (Proc_PCBLink *)itemPtr;
	itemPtr = List_Next(itemPtr);
	procPtr = hashChainItemPtr->procPtr;
	if (procPtr->event != event) {
	    sync_Collisions++;
	    continue;
	}
	switch (procPtr->state) {
	    case PROC_WAITING:
	        break;
	    case PROC_MIGRATING:
		Sys_Panic(SYS_FATAL,
			  "Can't handle waking up a migrating proc.\n");
	        break;
	    case PROC_MIGRATED:
		/*
		 * Need to handle waking up migrated processes.
		 */
		Sys_Panic(SYS_FATAL,
			  "Can't handle waking up a migrated proc.\n");
		break;
	    default:
		Sys_Panic(SYS_FATAL, "%s %s",  
			  "Sys_EventWakeupInt:",
			  "Tried to wakeup a non-waiting proc.\n");
	    break;

	}

	sync_Instrument.numWakeups++;
	List_Remove((List_Links *) hashChainItemPtr);
	procPtr->event = NIL;
	if (procPtr->state == PROC_WAITING) {
	    procPtr->state = PROC_READY;
	    Sched_MoveInQueue(procPtr);
	}
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Sync_EventWakeup --
 *
 *	Perform a broadcast to wake up a process awaiting an arbitrary
 *	event.  Obtain the master lock, then call the internal Wakeup
 *	routine.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Make any processes waiting on this event runnable.
 *
 *----------------------------------------------------------------------
 */

ENTRY void
Sync_EventWakeup(event)
    unsigned int event;	/* arbitrary integer */
{
    MASTER_LOCK(sched_Mutex);
    SyncEventWakeupInt(event);
    MASTER_UNLOCK(sched_Mutex);
}


/*
 *----------------------------------------------------------------------------
 *
 * Sync_WakeWaitingProcess --
 *
 *      Wake up a particular process as though the local or remote event it is 
 *	awaiting has occurred.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *      If waiting on an event, removes the given process from its event hash 
 *	chain and makes it runnable.
 *
 *----------------------------------------------------------------------------
 */

void
Sync_WakeWaitingProcess(procPtr)
    register	Proc_ControlBlock 	*procPtr;
{
    MASTER_LOCK(sched_Mutex);
    if (procPtr->event != NIL) {
	List_Remove(&procPtr->eventHashChain.links);
	procPtr->event = NIL;
	procPtr->state = PROC_READY;
	Sched_MoveInQueue(procPtr);
    } else {
	if (procPtr->state == PROC_WAITING) {
	    if (!(procPtr->syncFlags & SYNC_WAIT_REMOTE)) {
		Sys_Panic(SYS_FATAL, "Sync_WakeWaitingProcess: Proc waiting but event and remote wait NIL\n");
	    }
	    ProcessWakeup(procPtr, procPtr->waitToken);
	}
    }
    MASTER_UNLOCK(sched_Mutex);
}


/*
 *----------------------------------------------------------------------------
 *
 * SyncEventWaitInt --
 *
 *	Make a process sleep waiting for an event.  The blocked process is
 *	placed on a hash chain keyed on the event.  This routine will return
 *	without putting the process to sleep if there is a signal pending
 *	and the proper flag is set in the proc table.
 *
 * Results:
 *	TRUE if woke up because of a signal, FALSE otherwise.
 *
 * Side effects:
 *	The event that the process is waiting for is noted in the process
 *	table.  The process is marked as waiting and a new process
 *	is selected to run.
 *
 *----------------------------------------------------------------------------
 */

INTERNAL Boolean
SyncEventWaitInt(event, wakeIfSignal)
    unsigned 	int 	event;		/* Event to wait on. */
    Boolean		wakeIfSignal;	/* TRUE => wake if signal. */
{
    Proc_ControlBlock *procPtr;
    List_Links *chainHeader;

    procPtr = Proc_GetCurrentProc(Sys_GetProcessorNumber());

    if (wakeIfSignal && Sig_Pending(procPtr)) {
	return(TRUE);
    }

    chainHeader = &eventChainHeaders[event % PROC_HASHBUCKETS];
    List_Insert(&procPtr->eventHashChain.links, LIST_ATREAR(chainHeader));

    procPtr->event = event;
    Sched_ContextSwitchInt(PROC_WAITING);
    return(FALSE);
}


/*
 *----------------------------------------------------------------------------
 *
 * Sync_EventWait --
 *
 *	Make a process sleep waiting for an event.  This is an external
 *	version of the routine which is to be called without the sched_Mutex
 *	master lock held.
 *
 * Results:
 *	TRUE if woke up because of a signal, FALSE otherwise.
 *
 * Side effects:
 *	The sched_Mutex master lock is set, then the "internal" version
 * 	of the EventWait routine is called.
 *
 *----------------------------------------------------------------------------
 */

ENTRY Boolean
Sync_EventWait(event, wakeIfSignal)
    unsigned 	int 	event;		/* Event to wait on. */
    Boolean		wakeIfSignal;	/* TRUE => wait if signal. */
{
    Boolean	sigPending;

    MASTER_LOCK(sched_Mutex);
    sigPending = SyncEventWaitInt(event, wakeIfSignal);
    MASTER_UNLOCK(sched_Mutex);
    VmMach_SetupContext(Proc_GetCurrentProc(Sys_GetProcessorNumber()));
    return(sigPending);
}


/*
 *----------------------------------------------------------------------------
 *
 * Sync_GetWaitToken --
 *
 *	Return the process id and increment and return wait token for the 
 *	current process.
 *
 * Results:
 *	process ID and wait token for current process.
 *
 * Side effects:
 *	Wait token incremented.
 *
 *----------------------------------------------------------------------------
 */

ENTRY void
Sync_GetWaitToken(pidPtr, tokenPtr)
    Proc_PID	*pidPtr;	/* If non-nil pid of current process. */
    int		*tokenPtr;	/* Wait token of current process. */
{
    register	Proc_ControlBlock	*procPtr;

    procPtr = Proc_GetCurrentProc(Sys_GetProcessorNumber());

    MASTER_LOCK(sched_Mutex);

    procPtr->waitToken++;
    if (pidPtr != (Proc_PID *) NIL) {
	*pidPtr = procPtr->processID;
    }
    *tokenPtr = procPtr->waitToken;

    MASTER_UNLOCK(sched_Mutex);
}


/*
 *----------------------------------------------------------------------------
 *
 * Sync_SetWaitToken --
 *
 *	Set the wait token for the given process.  Only exists for process
 *	migration should not be used in general.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Wait token value set in the PCB for the given process.
 *
 *----------------------------------------------------------------------------
 */

ENTRY void
Sync_SetWaitToken(procPtr, waitToken)
    Proc_ControlBlock	*procPtr;	/* Process to set token for. */
    int			waitToken;	/* Token value. */
{
    MASTER_LOCK(sched_Mutex);

    procPtr->waitToken = waitToken;

    MASTER_UNLOCK(sched_Mutex);
}


/*
 *----------------------------------------------------------------------------
 *
 * Sync_ProcWait --
 *
 *	This is called to block a process after it has been told by a
 *	remote host that it has to wait.  The wait completed flag is
 *	checked here to see if the remote host's wakeup message has raced 
 *	(and won) with this process's decision to call this procedure.
 *
 *	For safety, this routine should be expanded to automatically
 *	set up a timeout event which will wake up the process anyway.
 *
 * Results:
 *	TRUE if woke up because of a signal, FALSE otherwise.
 *
 * Side effects:
 *	The wait complete flag of the process is checked and the process
 *	is blocked if a notify message has not already arrived.
 *
 *----------------------------------------------------------------------------
 */

ENTRY Boolean
Sync_ProcWait(lockPtr, wakeIfSignal)
    Sync_Lock	*lockPtr;	/* If non-nil release this lock before going to
				 * sleep and reaquire it after waking up. */
    Boolean	wakeIfSignal;	/* TRUE => Don't go to sleep if a signal is
				 *         pending. */
{
    register	Proc_ControlBlock 	*procPtr;
    Boolean				releasedLock = FALSE;
    Boolean				sigPending = FALSE;

    MASTER_LOCK(sched_Mutex);
    procPtr = Proc_GetCurrentProc(Sys_GetProcessorNumber());
    if (!(procPtr->syncFlags & SYNC_WAIT_COMPLETE)) {
	if (wakeIfSignal && Sig_Pending(procPtr)) {
	    /*
	     * Check for signals.   If a signal is pending, then bail out.
	     */
	    sigPending = TRUE;
	} else {
	    /*
	     * Block the process.  The wakeup message from the remote host
	     * has not arrived.
	     */
	    procPtr->syncFlags |= SYNC_WAIT_REMOTE;
	    if (lockPtr != (Sync_Lock *) NIL) {
		/*
		 * We were given a monitor lock to release, so release it.
		 */
		lockPtr->inUse = 0;
		lockPtr->waiting = FALSE;
		SyncEventWakeupInt((unsigned int)lockPtr);
		releasedLock = TRUE;
	    }
	    Sched_ContextSwitchInt(PROC_WAITING);
	    if (wakeIfSignal && Sig_Pending(procPtr)) {
		sigPending = TRUE;
	    }
	}
    }
    /*
     * After being notified (and context switching back to existence),
     * or if we have already been notified, clear state about the
     * remote wait.  This means our caller should get a new token
     * (ie. retry whatever remote operation it was) before waiting
     * again.
     */
    procPtr->waitToken++;
    procPtr->syncFlags &= ~(SYNC_WAIT_COMPLETE | SYNC_WAIT_REMOTE);
    MASTER_UNLOCK(sched_Mutex);
    VmMach_SetupContext(procPtr);
    if (releasedLock) {
	Sync_GetLock(lockPtr);
    }
    return(sigPending);
}


/*
 *----------------------------------------------------------------------------
 *
 * Sync_ProcWakeup --
 *
 *	Wakeup a blocked process in response to a message from a remote
 *	host.  Call internal routine to do the work.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------------
 */

ENTRY void
Sync_ProcWakeup(pid, token)
    Proc_PID 	pid;	/* PID of process to wake up. */
    int		token;	/* Token to use to wake up process. */
{
    Proc_ControlBlock 	*procPtr;

    procPtr = Proc_GetPCB(pid);
    MASTER_LOCK(sched_Mutex);
    ProcessWakeup(procPtr, token);
    MASTER_UNLOCK(sched_Mutex);
}


/*
 *----------------------------------------------------------------------------
 *
 * ProcessWakeup --
 *
 *	Wakeup a blocked process in response to a message from a remote
 *	host.  It is possible that the wakeup message has raced and
 *	won against the local process's call to Sync_ProcWait.  This
 *	protected against with a token and a wakeup complete flag.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	syncFlags modified.
 *
 *----------------------------------------------------------------------------
 */

static ENTRY void
ProcessWakeup(procPtr, waitToken)
    register	Proc_ControlBlock 	*procPtr;	/* Process to wake up.*/
    int					waitToken;	/* Toekn to use. */
{
#ifdef notdef
    if (procPtr->waitToken == waitToken || waitToken == SYNC_BROADCAST_TOKEN) 
#endif notdef
	/*
	 * Token matches process's token.
	 */
	procPtr->syncFlags |= SYNC_WAIT_COMPLETE;
	if (procPtr->state == PROC_WAITING) {
	    if (procPtr->event == NIL) {
		/*
		 * Only wakeup if are doing this type of wait and not an
		 * event wait.
		 */
		procPtr->state = PROC_READY;
		Sched_MoveInQueue(procPtr);
	    }
	} else {
	    /*
	     * This is a notify message which has raced (and won) with the 
	     * process's call to Sync_ProcWait.
	     */
	    syncProcWakeupRaces++;
	}
#ifdef notdef
    } else {
	/*
	 * Wakeup pertains to an old event.
	 */
    }
#endif notdef
}


/*
 *----------------------------------------------------------------------
 *
 * Sync_RemoteNotify --
 *
 *	Perform an RPC to notify a remote process.
 *
 * Results:
 *	The return code from the RPC.  This enables the caller to decide
 *	if it should wait and retry the notify later if the remote
 *	host is unavailable.
 *
 * Side effects:
 *      This results in a call to Sync_ProcWakeup on the host of the
 *      waiting process.
 *
 *----------------------------------------------------------------------
 */
ReturnStatus
Sync_RemoteNotify(waitPtr)
    Sync_RemoteWaiter *waitPtr;		/* Arguments to remote notify. */
{
    Rpc_Storage storage;
    ReturnStatus status;

    storage.requestParamPtr = (Address)waitPtr;
    storage.requestParamSize = sizeof(Sync_RemoteWaiter);
    storage.requestDataSize = 0;
    storage.replyParamSize = 0;
    storage.replyDataSize = 0;
    status = Rpc_Call(waitPtr->hostID, RPC_REMOTE_WAKEUP, &storage);
    return(status);
}


/*
 *----------------------------------------------------------------------
 *
 * Sync_RemoteNotifyStub --
 *
 *	The service stub for the remote wakeup RPC.
 *
 * Results:
 *	SUCCESS.
 *
 * Side effects:
 *      A call to Sync_ProcWakeup on the process.
 *
 *----------------------------------------------------------------------
 */
ReturnStatus
Sync_RemoteNotifyStub(srvToken, clientID, command, storagePtr)
    ClientData 	srvToken;	/* Handle on server process passed to
				 * Rpc_Reply. */
    int 	clientID;	/* Sprite ID of client host. */
    int 	command;	/* Command identifier. */
    Rpc_Storage *storagePtr;    /* The request fields refer to the request
				 * buffers and also indicate the exact amount
				 * of data in the request buffers.  The reply
				 * fields are initialized to NIL for the
				 * pointers and 0 for the lengths.  This can
				 * be passed to Rpc_Reply. */
{
    register Sync_RemoteWaiter *waitPtr;

    waitPtr = (Sync_RemoteWaiter *)storagePtr->requestParamPtr;
    Sync_ProcWakeup(waitPtr->pid, waitPtr->waitToken);
    Rpc_Reply(srvToken, SUCCESS, storagePtr, NIL, NIL);
    return(SUCCESS);
}
