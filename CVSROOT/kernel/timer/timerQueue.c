/* 
 * timerQueue.c --
 *
 *	Routines to handle interrupts from the timer chip.
 *
 *      The timer call back routine is called at every callback timer
 *      interrupt. The callback timer is used to enable various modules of
 *      the kernel to have routines called at a particular time in the
 *      future.  For example, to run the "clock" paging algorithm once a
 *      second, to see if a Fs_Select call has timed-out, etc.  The timer
 *      queue can only be used by kernel modules because it is assumed
 *      that the routines to be called exist in the system segment.  The
 *      routines to be called are maintained on the timer queue.  The
 *      callback timer is active only when the timer queue is not empty.
 *
 *
 * Copyright 1986, 1988 Regents of the University of California
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
#include "timer.h"
#include "timerInt.h"
#include "devTimer.h"
#include "sys.h"
#include "sync.h"
#include "sched.h"
#include "list.h"
#include "vm.h"
#include "dbg.h"
#include "byte.h"



/* DATA STRUCTURES */

/*
 *  The timer queue is a linked list of routines that need to be called at
 *  certain times. TimerQueueList points to the head structure for the queue.
 *
 * >>>>>>>>>>>>>>>>>>>
 * N.B. For debugging purposes, timerQueueList is global
 *      so it can be accessed by routines outside this module.
 * <<<<<<<<<<<<<<<<<<<
 */ 

/* static */ List_Links	*timerQueueList;

/*
 * The timer module mutex semaphore.  
 */

int timerMutex = 0;

/*
 * The value to update the time of day at every timer interrupt.
 * It equals the amount of time between callback timer interrupts.
 */

static Time todUpdate = {
    0, DEV_CALLBACK_INTERVAL * (ONE_SECOND/ONE_MILLISECOND)
};

/*
 * UpdateTimeOfDay() adjusts timerTimeOfDay to the real time of day.
 */

static void UpdateTimeOfDay();
static Timer_QueueElement      updateElement;

/*
 *  Debugging routine and data.
 */

#ifdef DEBUG
#define SIZE 500
static unsigned char array[SIZE+1];
static int count = 0;
#endif DEBUG

/*
 * Instrumentation for counting how many times the routines get called.
 */

Timer_Statistics timer_Statistics;



/*
 *----------------------------------------------------------------------
 *
 * Timer_Init --
 *
 *	Initializes the data structures necessary to manage the timer
 *	queue of procedures.
 *
 * Results:
 *     None.
 *
 * Side effects:
 *     The timer queue structure is created and initialized.
 *
 *----------------------------------------------------------------------
 */

void
Timer_Init()
{
    TimerTicksInit();

    Byte_Zero(sizeof(timer_Statistics), (Address) &timer_Statistics);

    timerQueueList = (List_Links *) Vm_BootAlloc(sizeof(List_Links));
    List_Init(timerQueueList);

    /*
     * Add the routine to fix the time of day to the timer queue.
     * The routine is called every 10 seconds.
     */

    updateElement.routine = UpdateTimeOfDay;
    updateElement.interval = 10 * timer_IntOneSecond;
    Timer_ScheduleRoutine(&updateElement, TRUE);
}


/*
 *----------------------------------------------------------------------
 *
 * UpdateTimeOfDay --
 *
 *	Called from the timer queue to make timerTimeOfDay close
 *	to the real current time as calculated by Timer_GetTimeOfDay.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	timerTimeOfDay is updated.
 *
 *----------------------------------------------------------------------
 */

static void
UpdateTimeOfDay()
{
    Timer_GetRealTimeOfDay(&timerTimeOfDay, (int *) NIL, (int *) NIL);
    Timer_RescheduleRoutine(&updateElement, TRUE);
}


/*
 *----------------------------------------------------------------------
 *
 *  Timer_CallBack --
 *
 *      This routine is called at every call back timer interrupt. 
 *      It calls routines on the timer queue. 
 *
 *  Results:
 *	None.
 *
 *  Side Effects:
 *	Routines on the timer queue may cause side effects.
 *
 *----------------------------------------------------------------------
 */

void
Timer_CallBack()
{
	register List_Links	*itemPtr;	/* Used to examine TQE's. */
	register List_Links	*readyPtr;	/* Ptr to TQE that's ready
						 * to be called. */
	Timer_Ticks		currentTime;

	/*
	 *  The callback timer has expired. This means at least the first
	 *  routine on the timer queue is ready to be called.  Go through
	 *  the queue and call all routines that are scheduled to be
	 *  called. Since the queue is ordered by time, we can quit looking 
	 *  when we find the first routine that does not need to be called.
	 */

#ifdef GATHER_STAT
	timer_Statistics.callback++;
#endif

	MASTER_LOCK(timerMutex);

	Time_Add(timerTimeOfDay, todUpdate, &timerTimeOfDay);
	if (vm_Tracing) {
	    Vm_StoreTraceTime(timerTimeOfDay);
	}
	Sched_GatherProcessInfo();

	if (!List_IsEmpty(timerQueueList)) {

	    Timer_GetCurrentTicks(&currentTime);

	    itemPtr = List_First(timerQueueList); 
	    while (!List_IsAtEnd((timerQueueList), itemPtr)) {
		if(Timer_TickGT(((Timer_QueueElement *)itemPtr)->time, 
		  		  currentTime)) {
		    break;
		} else {

		    /*
		     *  First remove the item before calling it so the routine 
		     *  can call Timer_ScheduleRoutine to reschedule itself on 
		     *  the timer queue and not mess up the pointers on the 
		     *  queue.
		     */

		    readyPtr = itemPtr;
		    itemPtr  = List_Next(itemPtr);
		    List_Remove(readyPtr);

		    /*
		     *  Now call the routine.  It is interrupt time and 
		     *  the routine must do as little as possible.  The 
		     *  routine is passed the time it was scheduled to 
		     *  be called at and a client-specified argument.
		     */

#define  ELEMENTPTR ((Timer_QueueElement *) readyPtr)

		    if (ELEMENTPTR->routine == 0) {
			Sys_Panic(SYS_FATAL,
			"Timer_ServiceInterrupt: t.q.e. routine == 0\n");
		    } else {
			ELEMENTPTR->processed = TRUE;
			(ELEMENTPTR->routine) 
				(ELEMENTPTR->time,ELEMENTPTR->clientData);
		    }
		}
	    }
#undef  ELEMENTPTR


	} 
	MASTER_UNLOCK(timerMutex);
    
}


/*
 *----------------------------------------------------------------------
 *
 * Timer_ScheduleRoutine --
 *
 *	Schedules a routine to be called at a certain time by adding 
 *	it to the timer queue. A routine is specified using a
 *	Timer_QueueElement structure, which is described in timer.h.
 *
 *	When the client routine is called at its scheduled time, it is 
 *	passed two parameters:
 *	 a) the time it is scheduled to be called at, and
 *	 b) uninterpreted data.
 *	Hence the routine should be declared as:
 *
 *	    void
 *	    ExampleRoutine(time, data)
 *		Timer_Ticks time;
 *		ClientData data;
 *	    {}
 *
 *
 *	The time a routine should be called at can be specified in two
 *	ways: an absolute time or an interval. For example, to have 
 *	ExampleRoutine called in 1 hour, the timer queue element would 
 *	be setup as:
 *	    Timer_QueueElement	element;
 *
 *	    element.routine	= ExampleRoutine;
 *	    element.clientData	= (ClientData) 0;
 *	    element.interval	= timer_IntOneHour;
 *	    Timer_ScheduleRoutine(&element, TRUE);
 *
 *	The 2nd argument (TRUE) to Timer_ScheduleRoutine means the routine
 *	will be called at the interval + the current time.
 *
 *      Once ExampleRoutine is called, it can schedule itself to be
 *      called again using Timer_RescheduleRoutine().  Timer_ScheduleRoutine 
 *	must not be used because it can't be called by routines that 
 *	are called from the timer queue.
 *
 *	    Timer_RescheduleRoutine(&element, TRUE);
 *
 *	The 2nd argument again means schedule the routine relative to the
 *	current time. Since we still want ExampleRoutine to be called in
 *	an hour, we don't have to change the interval field in the timer
 *	queue element.
 *      Obviously, the timer queue element has to be accessible 
 *	to ExampleRoutine.
 *
 *	If we want ExampleRoutine to be called at a specific time, say
 *	March 1, 1986, the time field in the t.q. element must be set:
 *
 *	    element.routine	= ExampleRoutine;
 *	    element.clientData	= (ClientData) 0;
 *	    element.time	= march1;
 *	    Timer_ScheduleRoutine(&element, FALSE);
 *
 *	(Assume march1 has the appropriate value for the date 3/1/86.)
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The timer queue element is added to the timer queue.
 *
 *----------------------------------------------------------------------
 */

void
Timer_ScheduleRoutine(newElementPtr, interval)
    register	Timer_QueueElement *newElementPtr; /* routine to be added */
    Boolean	interval;	/* TRUE if schedule relative to current time. */
{
#ifdef GATHER_STAT
    timer_Statistics.schedule++;
#endif

    MASTER_LOCK(timerMutex); 
    Timer_RescheduleRoutine(newElementPtr, interval);
    MASTER_UNLOCK(timerMutex); 
}


/*
 *----------------------------------------------------------------------
 *
 * Timer_RescheduleRoutine --
 *
 *	Places a routine on the timer queue so it will be called 
 *	at a specific time.
 *
 *	This routine assumes the timer mutex is held. Therefore
 *	it can be used by routines that are called from the timer queue.
 *	Timer_ScheduleRoutine calls this routine once it obtains the
 *	timer mutex.
 *
 *	Most of the prefatory comments of Timer_ScheduleRoutine are 
 *	appropriate for	this routine. 
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The timer queue is extended. 
 *
 *----------------------------------------------------------------------
 */

void
Timer_RescheduleRoutine(newElementPtr, interval)
    register	Timer_QueueElement *newElementPtr; /* routine to be added */
    Boolean	interval;	/* TRUE if schedule relative to current time. */
{
    register List_Links	 *itemPtr;
    Boolean inserted;		/* TRUE if added to Q in FORALL loop. */

    /*
     *  Go through the timer queue and insert the new routine.  The queue
     *  is ordered by the time field in the element.  The sooner the
     *  routine needs to be called, the closer it is to the front of the
     *  queue.  The new routine will not be added to the queue inside the
     *  FOR loop if its scheduled time is after all elements in the queue
     *  or the queue is empty.  It will be added after the last element in
     *  the queue.
     */

    inserted = FALSE;  /* assume new element not inserted inside FOR loop.*/

#ifdef GATHER_STAT
    timer_Statistics.resched++;
#endif

    /*
     * Safety check.
     */
    if (newElementPtr->routine == 0) {
	Sys_Panic(SYS_FATAL, 
		"Timer_RescheduleRoutine: bad address for t.q.e. routine.\n");
    }

    /* 
     *  Reset the processed flag. It is used by the client to see if 
     *  the routine is being called from the timer queue. This flag is
     *  necessary because the client passes in the timer queue element
     *  and it may need to examine the element to determine its status.
     */
    newElementPtr->processed = FALSE;

    /*
     * Convert the interval into an absolute time by adding the 
     * interval to the current time.
     */
    if (interval) {
	Timer_Ticks currentTime;
	Timer_GetCurrentTicks(&currentTime);
	Timer_AddIntervalToTicks(currentTime, newElementPtr->interval,
            	       &(newElementPtr->time));
    }

    List_InitElement((List_Links *) newElementPtr);

    LIST_FORALL(timerQueueList, itemPtr) {

       if (Timer_TickLT(newElementPtr->time, 
	   ((Timer_QueueElement *)itemPtr)->time)) {
	    List_Insert((List_Links *) newElementPtr, LIST_BEFORE(itemPtr));
	    inserted = TRUE;
	    break;
	}
    }

    if (!inserted) {
	List_Insert((List_Links *) newElementPtr, LIST_ATREAR(timerQueueList));
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Timer_DescheduleRoutine --
 *
 *	Deschedules a routine to be called at a certain time by removing 
 *	it from the timer queue.
 *
 *	Note that Timer_DescheduleRoutine does NOT guarantee that the 
 *	routine to be descheduled will not be called, only that the 
 *	routine will not be on the timer queue when Timer_DescheduleRoutine 
 *	returns.
 *
 *	If Timer_DescheduleRoutine is able to obtain the timer mutex before
 *	the timer interrupts, the routine will be removed from the
 *	timer queue before the interrupt handler has a chance to call it.
 *	If the interrupt handler is able to obtain the timer mutex before
 *	Timer_DescheduleRoutine, then the interrupt handler will remove and 
 *	call the routine before Timer_DescheduleRoutine has a chance 
 *	to remove it.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The timer queue structure is updated. 
 *
 *----------------------------------------------------------------------
 */

void
Timer_DescheduleRoutine(elementPtr)
    register Timer_QueueElement *elementPtr;	/* routine to be removed */
{
    register List_Links	 *itemPtr;

#ifdef GATHER_STAT
    timer_Statistics.desched++;
#endif

    /*
     *  Go through the timer queue and remove the routine.  
     */

    MASTER_LOCK(timerMutex); 

    LIST_FORALL(timerQueueList, itemPtr) {

	if ((List_Links *) elementPtr == itemPtr) {
	    List_Remove(itemPtr);
	    break;
	}
    }

    MASTER_UNLOCK(timerMutex); 
}
