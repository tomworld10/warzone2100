/*
	This file is part of Warzone 2100.
	Copyright (C) 1999-2004  Eidos Interactive
	Copyright (C) 2005-2007  Warzone Resurrection Project

	Warzone 2100 is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	Warzone 2100 is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Warzone 2100; if not, write to the Free Software
	Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
*/
/*
 * Event.c
 *
 * The event management system.
 */

#include <stdio.h>
#include <string.h>

// event tracing printf's
#define DEBUG_GROUP0
// display tested triggers
//#define DEBUG_GROUP1
#include "lib/framework/frame.h"
#include "interp.h"
#include "script.h"
#include "event.h"

// array to store release functions
static VAL_CREATE_FUNC		*asCreateFuncs;
static VAL_RELEASE_FUNC		*asReleaseFuncs;
static UDWORD		numFuncs;

// The list of currently active triggers
ACTIVE_TRIGGER	*psTrigList;

// The list of callback triggers
ACTIVE_TRIGGER	*psCallbackList;

// The new triggers added this loop
static ACTIVE_TRIGGER	*psAddedTriggers;

// The trigger which is currently firing
static ACTIVE_TRIGGER	*psFiringTrigger;
static BOOL				triggerChanged;
static UDWORD			updateTime;

// The currently allocated contexts
SCRIPT_CONTEXT	*psContList;

// The current event trace level
static SDWORD			eventTraceLevel=3;

// print info on trigger
#ifdef DEBUG
#define DB_TRIGINF(psTrigger, level)\
	if (eventTraceLevel >= (level)) \
		eventPrintTriggerInfo(psTrigger)
#else
#define DB_TRIGINF(psTrigger, level)
#endif

// event tracing printf
#ifdef DEBUG
#define DB_TRACE(x, level) \
	if (eventTraceLevel >= (level)) \
		debug( LOG_NEVER, x)
#else
#define DB_TRACE(x,level)
#endif

// Initialise a trigger
static BOOL eventInitTrigger(ACTIVE_TRIGGER **ppsTrigger, SCRIPT_CONTEXT *psContext,
							 UDWORD event, SDWORD trigger, UDWORD currTime);

// Add a trigger to the list in order
static void eventAddTrigger(ACTIVE_TRIGGER *psTrigger);

// Free up a trigger
static void eventFreeTrigger(ACTIVE_TRIGGER *psTrigger);

//resets the event timer - updateTime
void eventTimeReset(UDWORD initTime)
{
    updateTime = initTime;
}

/* Initialise the event system */
BOOL eventInitialise(EVENT_INIT *psInit)
{
	psTrigList = NULL;
	psCallbackList = NULL;
	psContList = NULL;
	eventTraceLevel = 0;
	asCreateFuncs = NULL;
	asReleaseFuncs = NULL;
	numFuncs = 0;
	strcpy(last_called_script_event, "<none>");
	return TRUE;
}


// reset the event system
void eventReset(void)
{
	ACTIVE_TRIGGER	*psCurr;
#ifdef DEBUG
	SDWORD			count=0;
#endif

	// Free any active triggers and their context's
	while (psTrigList)
	{
		psCurr = psTrigList;
		psTrigList = psTrigList->psNext;
#ifdef DEBUG
		if (!psCurr->psContext->release)
		{
			count += 1;
		}
#endif
		eventRemoveContext(psCurr->psContext);
		free(psCurr);
	}
	// Free any active callback triggers and their context's
	while (psCallbackList)
	{
		psCurr = psCallbackList;
		psCallbackList = psCallbackList->psNext;
#ifdef DEBUG
		if (!psCurr->psContext->release)
		{
			count += 1;
		}
#endif
		eventRemoveContext(psCurr->psContext);
		free(psCurr);
	}

	// Now free any context's that are left
	while (psContList)
	{
#ifdef DEBUG
		if (!psContList->release)
		{
			count += 1;
		}
#endif
		eventRemoveContext(psContList);
	}

#ifdef DEBUG
	if (count>0)
	{
		debug( LOG_NEVER, "eventReset: %d contexts still allocated at shutdown\n", count );
	}
#endif
}


// Shutdown the event system
void eventShutDown(void)
{
	eventReset();

	if (asCreateFuncs)
	{
		free(asCreateFuncs);
		asCreateFuncs = NULL;
	}
	if (asReleaseFuncs)
	{
		free(asReleaseFuncs);
		asReleaseFuncs = NULL;
	}
}


// get the trigger id string
const char *eventGetTriggerID(SCRIPT_CODE *psCode, SDWORD trigger)
{
	const char	*pID, *pTrigType;
	static char	aIDNum[255];
	SDWORD			i;
	TRIGGER_TYPE	type;

	if ((trigger >= 0) && (trigger < psCode->numTriggers))
	{
		type = psCode->psTriggerData[trigger].type;
	}
	else
	{
		return "INACTIVE";
	}

	pTrigType = "UNKNOWN";
	switch (type)
	{
	case TR_INIT:
		pTrigType = "INIT";
		break;
	case TR_CODE:
		pTrigType = "CODE";
		break;
	case TR_WAIT:
		pTrigType = "WAIT";
		break;
	case TR_EVERY:
		pTrigType = "EVERY";
		break;
	case TR_PAUSE:
		pTrigType = "PAUSE";
		break;
	default:
		if (asScrCallbackTab)
		{
			pTrigType = asScrCallbackTab[type - TR_CALLBACKSTART].pIdent;
		}
		break;
	}

	if (psCode->psDebug == NULL || type != TR_CODE)
	{
		sprintf(aIDNum, "%d (%s)", trigger, pTrigType);
	}
	else
	{
		pID = "NOT FOUND";
		for(i=0; i<psCode->debugEntries; i++)
		{
			if (psCode->psDebug[i].offset == psCode->pTriggerTab[trigger])
			{
				pID = psCode->psDebug[i].pLabel;
				break;
			}
		}
		sprintf(aIDNum, "%s (%s)", pID, pTrigType);
	}

	return aIDNum;
}

// get the event id string
const char *eventGetEventID(SCRIPT_CODE *psCode, SDWORD event)
{
	const char	*pID;
	static char	aIDNum[255];
	SDWORD			i;

	if ((psCode->psDebug == NULL) ||
		(event < 0) || (event > psCode->numEvents))
	{
		sprintf(aIDNum, "%d", event);
		return aIDNum;
	}

	pID = "NOT FOUND";
	for(i=0; i<psCode->debugEntries; i++)
	{
		if (psCode->psDebug[i].offset == psCode->pEventTab[event])
		{
			pID = psCode->psDebug[i].pLabel;
			break;
		}
	}

	return pID;
}

// Print out all the info available about a trigger
void eventPrintTriggerInfo(ACTIVE_TRIGGER *psTrigger)
{
	SCRIPT_CODE *psCode = psTrigger->psContext->psCode;
//	BOOL		debugInfo = psCode->psDebug != NULL;
	const char	*pTrigLab, *pEventLab;

	// find the debug info for the trigger
	pTrigLab = eventGetTriggerID(psCode, psTrigger->trigger);
	// find the debug info for the event
	pEventLab = eventGetEventID(psCode, psTrigger->event);

	debug( LOG_NEVER, "trigger %s at %d -> %s", pTrigLab, psTrigger->testTime, pEventLab );
	if (psTrigger->offset != 0)
	{
		debug( LOG_NEVER, " %d", psTrigger->offset );
	}
}

// Initialise the create/release function array - specify the maximum value type
BOOL eventInitValueFuncs(SDWORD maxType)
{
	if(asReleaseFuncs != NULL)	//<NEW> 13.05.05
		debug(LOG_ERROR, "eventInitValueFuncs: array already initialised");

	ASSERT( asReleaseFuncs == NULL,
		"eventInitValueFuncs: array already initialised" );

	asCreateFuncs = (VAL_CREATE_FUNC *)malloc(sizeof(VAL_CREATE_FUNC) * maxType);
	if (!asCreateFuncs)
	{
		debug(LOG_ERROR, "eventInitValueFuncs: Out of memory");
		return FALSE;
	}
	asReleaseFuncs = (VAL_RELEASE_FUNC *)malloc(sizeof(VAL_RELEASE_FUNC) * maxType);
	if (!asReleaseFuncs)
	{
		debug(LOG_ERROR, "eventInitValueFuncs: Out of memory");
		return FALSE;
	}

	memset(asCreateFuncs, 0, sizeof(VAL_CREATE_FUNC) * maxType);
	memset(asReleaseFuncs, 0, sizeof(VAL_RELEASE_FUNC) * maxType);
	numFuncs = maxType;

	return TRUE;
}

// Add a new value create function
BOOL eventAddValueCreate(INTERP_TYPE type, VAL_CREATE_FUNC create)
{
	if (type >= numFuncs)
	{
		debug( LOG_ERROR, "eventAddValueCreate: type out of range" );
		abort();
		return FALSE;
	}

	asCreateFuncs[type] = create;

	return TRUE;
}

// Add a new value release function
BOOL eventAddValueRelease(INTERP_TYPE type, VAL_RELEASE_FUNC release)
{
	if (type >= numFuncs)
	{
		debug( LOG_ERROR, "eventAddValueRelease: type out of range" );
		abort();
		return FALSE;
	}

	asReleaseFuncs[type] = release;

	return TRUE;
}

// Create a new context for a script
BOOL eventNewContext(SCRIPT_CODE *psCode, CONTEXT_RELEASE release,
					 SCRIPT_CONTEXT **ppsContext)
{
	SCRIPT_CONTEXT	*psContext;
	SDWORD		val, storeIndex, arrayNum, arraySize;
	UDWORD		i, j;
	INTERP_TYPE	type;
	VAL_CHUNK	*psNewChunk, *psNextChunk;

	ASSERT( psCode != NULL,
		"eventNewContext: Invalid code pointer" );

	// Get a new context
	psContext = malloc(sizeof(SCRIPT_CONTEXT));
	if (psContext == NULL)
	{
		debug(LOG_ERROR, "eventNewContext: Out of memory");
		return FALSE;
	}

	// Initialise the context
	psContext->psCode = psCode;
	psContext->triggerCount = 0;
	psContext->release = release;
	psContext->psGlobals = NULL;
	psContext->id = -1;		// only used by the save game
	val = psCode->numGlobals + psCode->arraySize - 1;
	arrayNum = psCode->numArrays - 1;
	arraySize = 1;
	if (psCode->numArrays > 0)
	{
		for(i=0; i<psCode->psArrayInfo[arrayNum].dimensions; i++)
		{
			arraySize *= psCode->psArrayInfo[arrayNum].elements[i];
		}
	}


	//prepare local variables (initialize, store type)
	//-------------------------------
	psCode->ppsLocalVarVal = (INTERP_VAL **)malloc(sizeof(INTERP_VAL*) * psCode->numEvents);	//allocate space for array of local var arrays for each event

	debug(LOG_SCRIPT,"allocated space for %d events", psCode->numEvents);

	for(i=0;i < psCode->numEvents; i++)
	{
		if(psCode->numLocalVars[i] > 0)	//this event has any local vars declared
		{
			psCode->ppsLocalVarVal[i] = (INTERP_VAL*)malloc(sizeof(INTERP_VAL) * psCode->numLocalVars[i]);	//allocate space for local vars array (for the current event)

			debug(LOG_SCRIPT,"Event %d has %d local variables", i, psCode->numLocalVars[i]);

			for(j=0; j < psCode->numLocalVars[i]; j++)
			{
				type = psCode->ppsLocalVars[i][j];

				//debug(LOG_SCRIPT,"var %d's type: %d", i, type);

				/* initialize Strings, integers, floats etc
				   memset to 0, the only special case is strings */
				memset (&(psCode->ppsLocalVarVal[i][j]), 0, sizeof(INTERP_VAL));
				if (type == VAL_STRING) {
					psCode->ppsLocalVarVal[i][j].v.sval = (char*)malloc(MAXSTRLEN);
					strcpy(psCode->ppsLocalVarVal[i][j].v.sval,"\0");
				}

				//Initialize objects
				if (asCreateFuncs != NULL && type < numFuncs && asCreateFuncs[type])
				{
					if (!asCreateFuncs[type](&(psCode->ppsLocalVarVal[i][j]) ))
					{
						debug(LOG_ERROR,"eventNewContext: asCreateFuncs failed for local var");
						return FALSE;
					}
				}

				psCode->ppsLocalVarVal[i][j].type = type; //store (copy) var type (data used during parsing -> data used during interpreting)

				//debug(LOG_SCRIPT, "i=%d, j=%d, value=%d",i,j,psCode->ppsLocalVarVal[i][j].v.ival);
			}

			//debug(LOG_SCRIPT,"------");

		}
		else	//this event has no local vars
		{
			psCode->ppsLocalVarVal[i] = NULL;
		}

	}

	while (val >= 0)
	{
		psNewChunk = malloc(sizeof(VAL_CHUNK));
		if (psNewChunk == NULL)
		{
			for(psNewChunk=psContext->psGlobals; psNewChunk; psNewChunk = psNextChunk)
			{
				psNextChunk = psNewChunk->psNext;
				free(psNewChunk);
			}
			free(psContext);
			return FALSE;
		}

		// Set the value types
		storeIndex = val % CONTEXT_VALS;
		while (storeIndex >= 0)
		{
			if (val >= psCode->numGlobals)
			{
				type = psCode->psArrayInfo[arrayNum].type;
			}
			else
			{
				type = psCode->pGlobals[val];
			}

			// initialize Strings, integers etc
			// memset to 0
			memset(&(psNewChunk->asVals[storeIndex]), 0, sizeof(INTERP_VAL));
			if (type == VAL_STRING) {
				psNewChunk->asVals[storeIndex].v.sval = (char*)malloc(MAXSTRLEN);
				strcpy(psNewChunk->asVals[storeIndex].v.sval,"\0");
			}

			// set type
			psNewChunk->asVals[storeIndex].type = type;

			//initialize objects
			if (asCreateFuncs != NULL && type < numFuncs && asCreateFuncs[type])
			{
				if (!asCreateFuncs[type](psNewChunk->asVals + storeIndex))
				{
					free(psNewChunk);
					for(psNewChunk=psContext->psGlobals; psNewChunk; psNewChunk = psNextChunk)
					{
						psNextChunk = psNewChunk->psNext;
						free(psNewChunk);
					}
					free(psContext);
					return FALSE;
				}
			}
			storeIndex -= 1;
			val -= 1;
			arraySize -= 1;
			if (arraySize <= 0)
			{
				// finished this array
				arrayNum -= 1;
				if (arrayNum >= 0)
				{
					// calculate the next array size
					arraySize = 1;
					for(i=0; i<psCode->psArrayInfo[arrayNum].dimensions; i++)
					{
						arraySize *= psCode->psArrayInfo[arrayNum].elements[i];
					}
				}
			}
		}
		psNewChunk->psNext = psContext->psGlobals;
		psContext->psGlobals = psNewChunk;
	}
	psContext->psNext = psContList;
	psContList = psContext;

	*ppsContext = psContext;

	return TRUE;
}


// Copy a context, including variable values
BOOL eventCopyContext(SCRIPT_CONTEXT *psContext, SCRIPT_CONTEXT **ppsNew)
{
	SCRIPT_CONTEXT	*psNew;
	SDWORD			val;
	VAL_CHUNK		*psChunk, *psOChunk;

	ASSERT( psContext,
		"eventCopyContext: Invalid context pointer" );

	// Get a new context
	if (!eventNewContext(psContext->psCode, psContext->release, &psNew))
	{
		return FALSE;
	}

	// Now copy the values over
	psChunk = psNew->psGlobals;
	psOChunk = psContext->psGlobals;
	while (psChunk)
	{
		for(val=0; val<CONTEXT_VALS; val++)
		{
			psChunk->asVals[val].v.ival = psOChunk->asVals[val].v.ival;
		}
		psChunk = psChunk->psNext;
		psOChunk = psOChunk->psNext;
	}

	*ppsNew = psNew;

	return TRUE;
}


// Add a new object to the trigger system
// Time is the application time at which all the triggers are to be started
BOOL eventRunContext(SCRIPT_CONTEXT *psContext, UDWORD time)
{
	SDWORD			event;
	ACTIVE_TRIGGER	*psTrigger;
	TRIGGER_DATA	*psData;
	SCRIPT_CODE		*psCode;

	ASSERT( psContext != NULL,
		"eventNewObject: Invalid context pointer" );

	// Now setup all the triggers
	psContext->triggerCount = 0;
	psCode = psContext->psCode;
	for(event = 0; event < psCode->numEvents; event++)
	{
		if (psCode->pEventLinks[event] >= 0)
		{
			// See if this is an init event
			psData = psCode->psTriggerData + psCode->pEventLinks[event];
			if (psData->type == TR_INIT)
			{
				if (!interpRunScript(psContext,	IRT_EVENT, event, 0))
				{
					return FALSE;
				}
			}
			else
			{
				if (!eventInitTrigger(&psTrigger, psContext, event,
						psCode->pEventLinks[event], time))
				{
					return FALSE;
				}
				eventAddTrigger(psTrigger);
				DB_TRACE(("added "),2);
				DB_TRIGINF(psTrigger, 2);
				DB_TRACE(("\n"),2);
			}
		}
	}

	return TRUE;
}

// Remove an object from the event system
void eventRemoveContext(SCRIPT_CONTEXT *psContext)
{
	ACTIVE_TRIGGER	*psCurr, *psPrev=NULL, *psNext;
	VAL_CHUNK		*psCChunk, *psNChunk;
	SCRIPT_CONTEXT	*psCCont, *psPCont=NULL;
	SDWORD			i, chunkStart;
	INTERP_VAL		*psVal;

	// Get rid of all it's triggers
	while(psTrigList && psTrigList->psContext == psContext)
	{
		psNext = psTrigList->psNext;
		eventFreeTrigger(psTrigList);
		psTrigList = psNext;
	}
	for(psCurr = psTrigList; psCurr; psCurr = psNext)
	{
		psNext = psCurr->psNext;
		if (psCurr->psContext == psContext)
		{
			eventFreeTrigger(psCurr);
			psPrev->psNext = psNext;
		}
		else
		{
			psPrev = psCurr;
		}
	}
	// Get rid of all it's callback triggers
	while(psCallbackList && psCallbackList->psContext == psContext)
	{
		psNext = psCallbackList->psNext;
		eventFreeTrigger(psCallbackList);
		psCallbackList = psNext;
	}
	for(psCurr = psCallbackList; psCurr; psCurr = psNext)
	{
		psNext = psCurr->psNext;
		if (psCurr->psContext == psContext)
		{
			eventFreeTrigger(psCurr);
			psPrev->psNext = psNext;
		}
		else
		{
			psPrev = psCurr;
		}
	}

	// Call the release function for all the values
	if (asReleaseFuncs != NULL)
	{
		psCChunk = psContext->psGlobals;
		chunkStart = 0;
		for(i=0; i<psContext->psCode->numGlobals; i++)
		{
			if (i - chunkStart >= CONTEXT_VALS)
			{
				chunkStart += CONTEXT_VALS;
				psCChunk = psCChunk->psNext;
				ASSERT( psCChunk != NULL,
					"eventRemoveContext: not enough value chunks" );
			}
			psVal = psCChunk->asVals + (i - chunkStart);
			if (psVal->type < numFuncs && asReleaseFuncs[psVal->type] != NULL)
			{
				asReleaseFuncs[psVal->type](psVal);
			}
		}
	}

	// Free it's variables
	for(psCChunk = psContext->psGlobals; psCChunk; psCChunk = psNChunk)
	{
		psNChunk = psCChunk->psNext;
		free(psCChunk);
	}

	// Remove it from the context list
	if (psContext == psContList)
	{
		psCCont = psContList;
		psContList = psContList->psNext;
		free(psCCont);
	}
	else
	{
		for(psCCont = psContList; psCCont && psCCont!=psContext;
			psCCont = psCCont->psNext)
		{
			psPCont = psCCont;
		}
		if (psCCont)
		{
			psPCont->psNext = psCCont->psNext;
			free(psContext);
		}
		else
		{
			ASSERT( FALSE, "eventRemoveContext: context not found" );
		}
	}
}

// Get the value pointer for a variable index
BOOL eventGetContextVal(SCRIPT_CONTEXT *psContext, UDWORD index, INTERP_VAL **ppsVal)
{
	VAL_CHUNK	*psChunk;

	// Find the chunk for the variable
	psChunk = psContext->psGlobals;
	while (psChunk && index >= CONTEXT_VALS)
	{
		index -= CONTEXT_VALS;
		psChunk = psChunk->psNext;
	}
	if (!psChunk)
	{
		ASSERT( FALSE, "eventGetContextVal: Variable not found" );
		return FALSE;
	}

	*ppsVal = psChunk->asVals + index;

	return TRUE;
}

// Set a global variable value for a context
BOOL eventSetContextVar(SCRIPT_CONTEXT *psContext, UDWORD index, INTERP_VAL *data)
{
	INTERP_VAL *psVal;

	if (!eventGetContextVal(psContext, index, &psVal))
	{
		return FALSE;
	}

	if(!interpCheckEquiv(psVal->type, data->type))
	{
		ASSERT( FALSE, "eventSetContextVar: Variable type mismatch (var type: %d, data type: %d)", psVal->type, data->type);
		return FALSE;
	}

	// Store the data
	if(data->type == VAL_STRING)
	{
		ASSERT(data->v.sval != NULL,
			"eventSetContextVar: ininitialized source string pointer");

		strcpy(psVal->v.sval, data->v.sval);

		free(data->v.sval);		//not needed anymore
		data->v.sval = NULL;
	}
	else
	{
		memcpy(psVal, data, sizeof(INTERP_VAL));
	}

	return TRUE;
}

// Add a trigger to the list in order
static void eventAddTrigger(ACTIVE_TRIGGER *psTrigger)
{
	ACTIVE_TRIGGER	*psCurr, *psPrev=NULL;
	UDWORD	testTime = psTrigger->testTime;

	if (psTrigger->type >= TR_CALLBACKSTART)
	{
		// Add this to the callback trigger list
		if (psCallbackList == NULL)
		{
			psTrigger->psNext = NULL;
			psCallbackList = psTrigger;
		}
		else if (psTrigger->type <= psCallbackList->type)
		{
			psTrigger->psNext = psCallbackList;
			psCallbackList = psTrigger;
		}
		else
		{
			for(psCurr=psCallbackList; psCurr && psCurr->type < psTrigger->type;
				psCurr=psCurr->psNext)
			{
				psPrev = psCurr;
			}
			psTrigger->psNext = psPrev->psNext;
			psPrev->psNext =  psTrigger;
		}
	}
	else if (psTrigList == NULL)
	{
		psTrigger->psNext = NULL;
		psTrigList = psTrigger;
	}
	else if (testTime <= psTrigList->testTime)
	{
		psTrigger->psNext = psTrigList;
		psTrigList = psTrigger;
	}
	else
	{
		for(psCurr=psTrigList; psCurr && psCurr->testTime < testTime;
			psCurr=psCurr->psNext)
		{
			psPrev = psCurr;
		}
		psTrigger->psNext = psPrev->psNext;
		psPrev->psNext =  psTrigger;
	}
}


// Initialise a trigger
static BOOL eventInitTrigger(ACTIVE_TRIGGER **ppsTrigger, SCRIPT_CONTEXT *psContext,
							 UDWORD event, SDWORD trigger, UDWORD currTime)
{
	ACTIVE_TRIGGER	*psNewTrig;
	TRIGGER_DATA	*psTrigData;
	UDWORD			testTime;

	ASSERT( event < psContext->psCode->numEvents,
		"eventAddTrigger: Event out of range" );
	ASSERT( trigger < psContext->psCode->numTriggers,
		"eventAddTrigger: Trigger out of range" );
	if (trigger == -1)
	{
		return FALSE;
	}

	// Get a trigger object
	psNewTrig = malloc(sizeof(ACTIVE_TRIGGER));
	if (psNewTrig == NULL)
	{
		debug(LOG_ERROR, "eventInitTrigger: Out of memory");
		return FALSE;
	}

	// Initialise the trigger
	psNewTrig->psContext = psContext;
	psContext->triggerCount += 1;
	psTrigData = psContext->psCode->psTriggerData + trigger;
	testTime = currTime + psTrigData->time;
	psNewTrig->testTime = testTime;
	psNewTrig->trigger = (SWORD)trigger;
	psNewTrig->type = (SWORD)psTrigData->type;
	psNewTrig->event = (UWORD)event;
	psNewTrig->offset = 0;

	*ppsTrigger = psNewTrig;

	return TRUE;
}

// Load a trigger into the system from a save game
BOOL eventLoadTrigger(UDWORD time, SCRIPT_CONTEXT *psContext,
					  SDWORD type, SDWORD trigger, UDWORD event, UDWORD offset)
{
	ACTIVE_TRIGGER	*psNewTrig;
	TRIGGER_DATA	*psTrigData;

	ASSERT( event < psContext->psCode->numEvents,
		"eventLoadTrigger: Event out of range" );
	ASSERT( trigger < psContext->psCode->numTriggers,
		"eventLoadTrigger: Trigger out of range" );

	// Get a trigger object
	psNewTrig = malloc(sizeof(ACTIVE_TRIGGER));
	if (psNewTrig == NULL)
	{
		debug( LOG_ERROR, "eventLoadTrigger: out of memory" );
		abort();
		return FALSE;
	}

	// Initialise the trigger
	psNewTrig->psContext = psContext;
	psContext->triggerCount += 1;
	psTrigData = psContext->psCode->psTriggerData + trigger;
	psNewTrig->testTime = time;
	psNewTrig->trigger = (SWORD)trigger;
	psNewTrig->type = (SWORD)type;
	psNewTrig->event = (UWORD)event;
	psNewTrig->offset = (UWORD)offset;

	eventAddTrigger(psNewTrig);

	return TRUE;
}

// add a TR_PAUSE trigger to the event system.
BOOL eventAddPauseTrigger(SCRIPT_CONTEXT *psContext, UDWORD event, UDWORD offset,
						  UDWORD time)
{
	ACTIVE_TRIGGER	*psNewTrig;
	SDWORD			trigger;

	ASSERT( event < psContext->psCode->numEvents,
		"eventAddTrigger: Event out of range" );

	// Get a trigger object
	psNewTrig = malloc(sizeof(ACTIVE_TRIGGER));
	if (psNewTrig == NULL)
	{
		debug(LOG_ERROR, "eventAddPauseTrigger: Out of memory");
		return FALSE;
	}

	// figure out what type of trigger will go into the system when the pause
	// finishes
	switch (psFiringTrigger->type)
	{
	case TR_WAIT:
		// fired by a wait trigger, no trigger to replace it
		trigger = -1;
		break;
	case TR_PAUSE:
		// fired by a pause trigger, use the trigger stored with it
		trigger = psFiringTrigger->trigger;
		break;
	default:
		// just store the trigger that fired
		trigger = psFiringTrigger->trigger;
		break;
	}

	// Initialise the trigger
	psNewTrig->psContext = psContext;
	psContext->triggerCount += 1;
	psNewTrig->testTime = updateTime + time;
	psNewTrig->trigger = (SWORD)trigger;
	psNewTrig->type = TR_PAUSE;
	psNewTrig->event = (UWORD)event;
	psNewTrig->offset = (UWORD)offset;

	// store the new trigger
	psNewTrig->psNext = psAddedTriggers;
	psAddedTriggers = psNewTrig;

	// tell the event system the trigger has been changed
	triggerChanged = TRUE;

	return TRUE;
}


// Free up a trigger
static void eventFreeTrigger(ACTIVE_TRIGGER *psTrigger)
{
	if (psTrigger->psContext->release && psTrigger->psContext->triggerCount <= 1)
	{
		// Free the context as well
		eventRemoveContext(psTrigger->psContext);
	}
	free(psTrigger);
}


// Activate a callback trigger
void eventFireCallbackTrigger(TRIGGER_TYPE callback)
{
	ACTIVE_TRIGGER	*psPrev,*psCurr,*psNext;
	TRIGGER_DATA	*psTrigDat;
	BOOL			fired;

	// FIXME temporary check
	// FIXME This needs to be removed as soon as we are sure the transport comes and we don't have any more problems with the VIDEO_QUIT callback !!
	if(callback == 15)
		debug(LOG_SCRIPT, "eventFireCallbackTrigger: processing CALL_VIDEO_QUIT");

	if (interpProcessorActive())
	{
		ASSERT( FALSE,
			"eventFireCallbackTrigger: script interpreter is already running" );
		return;
	}

	//this can be called from eventProcessTriggers and so will wipe out all the current added ones
	//psAddedTriggers = NULL;
	psPrev = NULL;
	for(psCurr = psCallbackList; psCurr && psCurr->type <= (int)callback;
		psCurr = psNext)
	{
		psNext = psCurr->psNext;
		if (psCurr->type == (int)callback)
		{
			// see if the callback should be fired
			fired = FALSE;
			if (psCurr->type != TR_PAUSE)
			{
				ASSERT( psCurr->trigger >= 0 &&
						psCurr->trigger < psCurr->psContext->psCode->numTriggers,
					"eventFireCallbackTrigger: invalid trigger number" );
				psTrigDat = psCurr->psContext->psCode->psTriggerData + psCurr->trigger;
			}
			else
			{
				psTrigDat = NULL;
			}
			if (psTrigDat && psTrigDat->code)
			{
				if (!interpRunScript(psCurr->psContext, IRT_TRIGGER,
							psCurr->trigger, 0))
				{
					ASSERT( FALSE, "eventFireCallbackTrigger: trigger %s: code failed",
							eventGetTriggerID(psCurr->psContext->psCode, psCurr->trigger) );
					psPrev = psCurr;
					continue;
				}
				if (!stackPopParams(1, VAL_BOOL, &fired))
				{
					ASSERT( FALSE, "eventFireCallbackTrigger: trigger %s: code failed",
							eventGetTriggerID(psCurr->psContext->psCode, psCurr->trigger) );
					psPrev = psCurr;
					continue;
				}
			}
			else
			{
				fired = TRUE;
			}

			// run the event
			if (fired)
			{
				DB_TRIGINF(psCurr,1);
				DB_TRACE((" fired\n"),1);

				// remove the trigger from the list
				if (psPrev == NULL)
				{
					psCallbackList = psCallbackList->psNext;
				}
				else
				{
					psPrev->psNext = psNext;
				}

				triggerChanged = FALSE;
				psFiringTrigger = psCurr;
				if (!interpRunScript(psCurr->psContext,
								IRT_EVENT, psCurr->event, psCurr->offset)) // this could set triggerChanged
				{
					ASSERT( FALSE, "eventFireCallbackTrigger: event %s: code failed",
						eventGetEventID(psCurr->psContext->psCode, psCurr->event) );
				}
				if (triggerChanged)
				{
					// don't need to add the trigger again - just free it
					eventFreeTrigger(psCurr);
				}
				else
				{
					// make sure the trigger goes back into the system
					psFiringTrigger->psNext = psAddedTriggers;
					psAddedTriggers = psFiringTrigger;
				}
			}
			else
			{
				psPrev = psCurr;
			}
		}
		else
		{
			psPrev = psCurr;
		}
	}

	// Now add all the new triggers
	for(psCurr = psAddedTriggers; psCurr; psCurr=psNext)
	{
		psNext = psCurr->psNext;
		eventAddTrigger(psCurr);
	}
	//clear out after added them all
	psAddedTriggers = NULL;
}


// Run a trigger
static BOOL eventFireTrigger(ACTIVE_TRIGGER *psTrigger)
{
//	TRIGGER_DATA	*psTrigData;
	BOOL			fired;
	INTERP_VAL		sResult;

	fired = FALSE;


	// If this is a code trigger see if it fires
	if (psTrigger->type == TR_CODE)
	{
		// Run the trigger
		if (!interpRunScript(psTrigger->psContext,
						IRT_TRIGGER, psTrigger->trigger, 0))
		{
			ASSERT( FALSE, "eventFireTrigger: trigger %s: code failed",
					eventGetTriggerID(psTrigger->psContext->psCode, psTrigger->trigger) );
			return FALSE;
		}
		// Get the result
		sResult.type = VAL_BOOL;
		if (!stackPopType(&sResult))
		{
			return FALSE;
		}
		fired = sResult.v.bval;
	}
	else
	{
		fired = TRUE;
	}

	// If it fired run the event
	if (fired)
	{
		DB_TRIGINF(psTrigger,1);
		DB_TRACE((" fired\n"),1);
		if (!interpRunScript(psTrigger->psContext,
						IRT_EVENT, psTrigger->event, psTrigger->offset))
		{
			DB_TRACE(("\n\n********  script failed  *********\n"), 0);
			DB_TRIGINF(psTrigger,0);
			DB_TRACE(("\n"),0);
			ASSERT( FALSE, "eventFireTrigger: event %s: code failed",
				eventGetEventID(psTrigger->psContext->psCode, psTrigger->event) );
			return FALSE;
		}
	}
#ifdef DEBUG
	else
	{
		DB_TRIGINF(psTrigger,3);
		DB_TRACE(("\n"),3);
	}
#endif

	return TRUE;
}


// Process all the currently active triggers
void eventProcessTriggers(UDWORD currTime)
{
	ACTIVE_TRIGGER	*psCurr, *psNext, *psNew;
	TRIGGER_DATA	*psData;

	// Process all the current triggers
	psAddedTriggers = NULL;
	updateTime = currTime;
	while(psTrigList && psTrigList->testTime <= currTime)
	{
		psCurr = psTrigList;
		psTrigList = psTrigList->psNext;

		// Store the trigger so that I can tell if the event changes
		// the trigger assigned to it
		psFiringTrigger = psCurr;
		triggerChanged = FALSE;

		// Run the trigger
		if (eventFireTrigger(psCurr))	// This might set triggerChanged
		{
			if (triggerChanged || psCurr->type == TR_WAIT)
			{
				// remove the trigger
				eventFreeTrigger(psCurr);
			}
			else if (psCurr->type == TR_PAUSE)
			{
				// restarted a paused event - replace the old trigger
				if (psCurr->trigger != -1)
				{
					if (eventInitTrigger(&psNew, psCurr->psContext,
										psCurr->event, psCurr->trigger, updateTime))
					{
						psNew->psNext = psAddedTriggers;
						psAddedTriggers = psNew;
					}
				}

				// remove the TR_PAUSE trigger
				eventFreeTrigger(psCurr);
			}
			else
			{
				// Add the trigger again
				psData = psCurr->psContext->psCode->psTriggerData +
							psCurr->trigger;
				psCurr->testTime = currTime + psData->time;
				psCurr->psNext = psAddedTriggers;
				psAddedTriggers = psCurr;
			}
		}
	}

	// Now add all the new triggers
	for(psCurr = psAddedTriggers; psCurr; psCurr=psNext)
	{
		psNext = psCurr->psNext;
		eventAddTrigger(psCurr);
	}
	//clear out after added them all
	psAddedTriggers = NULL;
}


// remove a trigger from a list
static void eventRemoveTriggerFromList(ACTIVE_TRIGGER **ppsList,
								SCRIPT_CONTEXT *psContext,
								SDWORD event, SDWORD *pTrigger)
{
	ACTIVE_TRIGGER	*psCurr, *psPrev=NULL;

	if (((*ppsList) != NULL) &&
		(*ppsList)->event == event &&
		(*ppsList)->psContext == psContext)
	{
		if ((*ppsList)->type == TR_PAUSE)
		{
			// pause trigger, don't remove it,
			// just note the type for when the pause finishes
			(*ppsList)->trigger = (SWORD)*pTrigger;
			*pTrigger = -1;
		}
		else
		{
			psCurr = *ppsList;
			*ppsList = (*ppsList)->psNext;
			free(psCurr);
		}
	}
	else
	{
		for(psCurr=*ppsList; psCurr; psCurr=psCurr->psNext)
		{
			if (psCurr->event == event &&
				psCurr->psContext == psContext)
			{
				break;
			}
			psPrev = psCurr;
		}
		if (psCurr && psCurr->type == TR_PAUSE)
		{
			// pause trigger, don't remove it,
			// just note the type for when the pause finishes
			psCurr->trigger = (SWORD)*pTrigger;
			*pTrigger = -1;
		}
		else if (psCurr)
		{
			psPrev->psNext = psCurr->psNext;
			free(psCurr);
		}
	}
}


// Change the trigger assigned to an event - to be called from script functions
BOOL eventSetTrigger(void)
{
	ACTIVE_TRIGGER	*psTrigger;
	UDWORD			event;
	SDWORD			trigger;
	SCRIPT_CONTEXT	*psContext;

	if (!stackPopParams(2, VAL_EVENT, &event, VAL_TRIGGER, &trigger))
	{
		return FALSE;
	}

#ifdef REALLY_DEBUG_THIS
	DB_TRACE(("eventSetTrigger %s %s\n",
		eventGetEventID(psFiringTrigger->psContext->psCode, event),
		eventGetTriggerID(psFiringTrigger->psContext->psCode, trigger)),2);
#endif

	// See if this is the event that is running
	psContext = psFiringTrigger->psContext;
	if (psFiringTrigger->event == event)
	{
		triggerChanged = TRUE;
	}
	else
	{
		// Remove any old trigger from the list
		eventRemoveTriggerFromList(&psTrigList, psContext, event, &trigger);
		eventRemoveTriggerFromList(&psCallbackList, psContext, event, &trigger);
		eventRemoveTriggerFromList(&psAddedTriggers, psContext, event, &trigger);
/*		if (psTrigList &&
			psTrigList->event == event &&
			psTrigList->psContext == psContext)
		{
			if (psTrigList->type == TR_PAUSE)
			{
				// pause trigger, don't remove it,
				// just note the type for when the pause finishes
				psTrigList->trigger = (SWORD)trigger;
				trigger = -1;
			}
			else
			{
				psCurr = psTrigList;
				psTrigList = psTrigList->psNext;
				free(psCurr);
			}
		}
		else
		{
			for(psCurr=psTrigList; psCurr; psCurr=psCurr->psNext)
			{
				if (psCurr->event == event &&
					psTrigList->psContext == psContext)
				{
					break;
				}
				psPrev = psCurr;
			}
			if (psCurr && psCurr->type == TR_PAUSE)
			{
				// pause trigger, don't remove it,
				// just note the type for when the pause finishes
				psCurr->trigger = (SWORD)trigger;
				trigger = -1;
			}
			else if (psCurr)
			{
				psPrev->psNext = psCurr->psNext;
				free(psCurr);
			}
		}
		// Remove any old callback trigger from the list
		if (psCallbackList && psCallbackList->event == event &&
			psCallbackList->psContext == psContext)
		{
			psCurr = psCallbackList;
			psCallbackList = psCallbackList->psNext;
			free(psCurr);
		}
		else
		{
			for(psCurr=psCallbackList; psCurr; psCurr=psCurr->psNext)
			{
				if (psCurr->psContext == psContext &&
					psCurr->event == event)
				{
					break;
				}
				psPrev = psCurr;
			}
			if (psCurr)
			{
				psPrev->psNext = psCurr->psNext;
				free(psCurr);
			}
		}*/
	}

	// Create a new trigger if necessary
	if (trigger >= 0)
	{
		if (!eventInitTrigger(&psTrigger, psFiringTrigger->psContext,
							event, trigger, updateTime))
		{
			return FALSE;
		}
		psTrigger->psNext = psAddedTriggers;
		psAddedTriggers = psTrigger;
	}

	return TRUE;
}


// set the event tracing level - to be called from script functions
BOOL eventSetTraceLevel(void)
{
	SDWORD	level;

	if (!stackPopParams(1, VAL_INT, &level))
	{
		return FALSE;
	}

	if (level < 0)
	{
		level = 0;
	}
	if (level > 3)
	{
		level = 3;
	}

	eventTraceLevel = level;

	return TRUE;
}

