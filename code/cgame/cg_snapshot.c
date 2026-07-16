// Copyright (C) 1999-2000 Id Software, Inc.
//
// cg_snapshot.c -- things that happen on snapshot transition,
// not necessarily every single rendered frame

#include "cg_local.h"


/*
=====================================================================

KILLCAM SNAPSHOT RECORDING AND PLAYBACK

Every snapshot the live context reads from the engine is copied into a
ring buffer. The killcam context replays them with a time delay: its
CG_ProcessSnapshots reads from the ring instead of trap_GetSnapshot.

=====================================================================
*/

// How many of the most recent snapshots are kept for killcam playback.
// The engine itself only buffers PACKET_BACKUP (32); a useful killcam
// delay needs more history. Raw snapshot_t storage is large (~55 KB
// each): 512 slots is ~27.6 MB of bss and covers ~25.6 s at snaps 20
// or ~12.8 s at snaps 40.
// TODO: consider compressing (delta encoding like the engine's) to
// afford a longer history in less memory.
#define KILLCAM_SNAPSHOT_BACKUP	512

// when the preroll gets clamped to the oldest recorded snapshot, keep
// this much slack (ms) so playback doesn't ride the ring's eviction
// edge and abort mid-replay
#define KILLCAM_CLAMP_MARGIN	200

static snapshot_t	cg_killcamSnapshots[KILLCAM_SNAPSHOT_BACKUP];
// total snapshots ever recorded; snapshot n (1-based) lives in
// slot (n-1) % KILLCAM_SNAPSHOT_BACKUP until overwritten
static int			cg_killcamRecordedCount;
// the killcam context's read cursor: how many recorded snapshots it has
// consumed. Counterpart of cgs.processedSnapshotNum for the live context.
static int			cg_killcamProcessedNum;
static qboolean		cg_killcamRunning;
static killcamMode_t	cg_killcamMode = KILLCAM_OFF;	// of the current run
static int			cg_killcamCurDelay;		// ms the current run lags behind live time
static int			cg_killcamKillerNum = -1;

// scheduled death replay (set when the local player gets killed)
static qboolean		cg_killcamDeathPending;
static int			cg_killcamDeathTime;	// cg.time when the obituary arrived
static int			cg_killcamDeathKiller;


// `trap_GetSnapshot` might actually be faster than copying the whole struct,
// because a `snapshot_t` struct has space for `MAX_GENTITIES` ents,
// whereas actual snaps only contain a few, so we need to copy less stuff.
#ifndef KILLCAM_COPY_SNAPSHOT
static void CG_KillcamRecordSnapshot( int num ) {
	qboolean	r;
	snapshot_t	*dest = &cg_killcamSnapshots[
		cg_killcamRecordedCount % KILLCAM_SNAPSHOT_BACKUP
	];
	r = trap_GetSnapshot( num, dest );
	if ( !r ) {
		CG_Printf( S_COLOR_YELLOW "WARNING: expected CG_KillcamRecordSnapshot to get called only when a snapshot exists\n" );
		return;
	}
	if ( dest->snapFlags & SNAPFLAG_NOT_ACTIVE ) {
		return;
	}
	cg_killcamRecordedCount++;
}
#else
static void CG_KillcamRecordSnapshot( const snapshot_t *snap ) {
	if ( snap->snapFlags & SNAPFLAG_NOT_ACTIVE ) {
		return;
	}
	cg_killcamSnapshots[cg_killcamRecordedCount % KILLCAM_SNAPSHOT_BACKUP] = *snap;
	cg_killcamRecordedCount++;
}
#endif


qboolean CG_KillcamRunning( void ) {
	return cg_killcamRunning;
}


/*
==================
CG_KillcamOldestSnapshotTime

serverTime of the oldest snapshot still in the ring, or -1 if none
==================
*/
static int CG_KillcamOldestSnapshotTime( void ) {
	int		oldest;

	if ( cg_killcamRecordedCount == 0 ) {
		return -1;
	}
	oldest = cg_killcamRecordedCount - KILLCAM_SNAPSHOT_BACKUP;
	if ( oldest < 0 ) {
		oldest = 0;
	}
	return cg_killcamSnapshots[oldest % KILLCAM_SNAPSHOT_BACKUP].serverTime;
}


/*
==================
CG_KillcamHasSnapshotFor

qtrue if the ring still holds a snapshot at or before the given time,
i.e. a killcam view of that time can be rendered
==================
*/
qboolean CG_KillcamHasSnapshotFor( int time ) {
	int		oldestTime = CG_KillcamOldestSnapshotTime();

	return oldestTime != -1 && oldestTime <= time;
}


/*
==================
CG_KillcamStart

Resets the killcam context and points its snapshot cursor at the newest
recorded snapshot at or before the given playback time, so the first
killcam frame doesn't fast-forward through (and fire the events of)
older history. CG_ProcessSnapshots does the rest on the next killcam
frame, just like after a fresh connect.
==================
*/
void CG_KillcamStart( int time, killcamMode_t mode ) {
	cgContext_t	*kc = &cg_contexts[CG_CONTEXT_KILLCAM];
	int			oldest;
	int			i;

	memset( kc, 0, sizeof( *kc ) );
	kc->state.clientNum = cg_contexts[CG_CONTEXT_LIVE].state.clientNum;
	// the replayed stream is conceptually a demo: interpolate instead of
	// predicting, never send anything to the server
	kc->state.demoPlayback = qtrue;

	CG_InitLocalEntitiesCtx( CG_CONTEXT_KILLCAM );
	CG_InitMarkPolysCtx( CG_CONTEXT_KILLCAM );
	CG_ClearParticlesCtx( CG_CONTEXT_KILLCAM );

	oldest = cg_killcamRecordedCount - KILLCAM_SNAPSHOT_BACKUP;
	if ( oldest < 0 ) {
		oldest = 0;
	}
	cg_killcamProcessedNum = oldest;
	for ( i = cg_killcamRecordedCount - 1 ; i >= oldest ; i-- ) {
		if ( cg_killcamSnapshots[i % KILLCAM_SNAPSHOT_BACKUP].serverTime <= time ) {
			cg_killcamProcessedNum = i;
			break;
		}
	}

	cg_killcamMode = mode;
	cg_killcamRunning = qtrue;
}


void CG_KillcamStop( void ) {
	cg_killcamRunning = qfalse;
	cg_killcamMode = KILLCAM_OFF;
}


killcamMode_t CG_KillcamMode( void ) {
	return cg_killcamRunning ? cg_killcamMode : KILLCAM_OFF;
}


int CG_KillcamKillerNum( void ) {
	return cg_killcamKillerNum;
}


/*
==================
CG_KillcamScheduleDeathReplay

Called from the obituary event when the local player gets killed by
another player. The replay itself is started later by CG_KillcamUpdate.
==================
*/
void CG_KillcamScheduleDeathReplay( int killerNum, int time ) {
	if ( cg_contextNum != CG_CONTEXT_LIVE ) {
		// obituary events re-fired by the replay itself
		return;
	}
	cg_killcamDeathPending = qtrue;
	cg_killcamDeathKiller = killerNum;
	cg_killcamDeathTime = time;
}


/*
==================
CG_KillcamUpdate

Runs the killcam state machine once per frame (with the live context
current). Returns how many milliseconds behind live time the killcam
context should be rendered this frame, or 0 to render the live view.
==================
*/
int CG_KillcamUpdate( int serverTime ) {
	// dev/test mode (cg_killcamTest <ms>) takes priority: replay of the
	// own view at a fixed delay
	if ( cg_killcamTest.integer > 0 ) {
		int delay = cg_killcamTest.integer;

		cg_killcamDeathPending = qfalse;
		if ( !cg_killcamRunning && CG_KillcamHasSnapshotFor( serverTime - delay ) ) {
			CG_KillcamStart( serverTime - delay, KILLCAM_TEST );
		}
		if ( cg_killcamRunning && cg_killcamMode == KILLCAM_TEST &&
			CG_KillcamHasSnapshotFor( serverTime - delay ) )
		{
			return delay;
		}
		return 0;
	}
	if ( cg_killcamRunning && cg_killcamMode == KILLCAM_TEST ) {
		CG_KillcamStop();
	}

	if ( !cg_killcam.integer ) {
		cg_killcamDeathPending = qfalse;
		if ( cg_killcamRunning ) {
			CG_KillcamStop();
		}
		return 0;
	}

	// death replay in progress?
	if ( cg_killcamRunning && cg_killcamMode == KILLCAM_KILLER ) {
		int postroll = cg_killcamPostroll.integer;

		if ( postroll < 0 ) {
			postroll = 0;
		}
		if (
			// replay finished
			serverTime - cg_killcamCurDelay > cg_killcamDeathTime + postroll
			// the player respawned (e.g. clicked): hand the view back
			|| cg.predictedPlayerState.stats[STAT_HEALTH] > 0
			// recording outran the playback; can't render this frame
			|| !CG_KillcamHasSnapshotFor( serverTime - cg_killcamCurDelay ) )
		{
			CG_KillcamStop();
			return 0;
		}
		return cg_killcamCurDelay;
	}

	// scheduled death replay waiting to start?
	if ( cg_killcamDeathPending ) {
		int		replayStartTime;
		int		oldestTime;
		int		preroll = cg_killcamPreroll.integer;
		int		startDelay = cg_killcamStartDelay.integer;

		if ( preroll < 0 ) {
			preroll = 0;
		}
		if ( startDelay < 0 ) {
			startDelay = 0;
		}

		// let the death register on screen before switching views
		if ( serverTime < cg_killcamDeathTime + startDelay ) {
			return 0;
		}
		cg_killcamDeathPending = qfalse;

		// skip if the player already respawned or the game is ending
		if ( cg.predictedPlayerState.stats[STAT_HEALTH] > 0 || cg.intermissionStarted ) {
			return 0;
		}

		// skip if not even the death itself is in the recorded history
		// (e.g. very high snaps rate combined with a long start delay)
		oldestTime = CG_KillcamOldestSnapshotTime();
		if ( oldestTime == -1 || oldestTime > cg_killcamDeathTime ) {
			return 0;
		}

		// if the ring no longer holds the full preroll, start at the
		// oldest snapshot we still have (with some slack so playback
		// doesn't ride the ring's eviction edge)
		replayStartTime = cg_killcamDeathTime - preroll;
		if ( replayStartTime < oldestTime + KILLCAM_CLAMP_MARGIN ) {
			replayStartTime = oldestTime + KILLCAM_CLAMP_MARGIN;
			if ( replayStartTime > cg_killcamDeathTime ) {
				replayStartTime = cg_killcamDeathTime;
			}
		}

		cg_killcamCurDelay = serverTime - replayStartTime;
		cg_killcamKillerNum = cg_killcamDeathKiller;
		CG_KillcamStart( replayStartTime, KILLCAM_KILLER );
		return cg_killcamCurDelay;
	}

	return 0;
}


/*
==================
CG_KillcamReadNextSnapshot

The killcam context's counterpart of CG_ReadNextSnapshot: reads from the
ring buffer instead of the client system.
==================
*/
static snapshot_t *CG_KillcamReadNextSnapshot( void ) {
	snapshot_t	*dest;
	int			oldestAvailable;

	// if playback fell behind the recording, skip the overwritten ones
	oldestAvailable = cg_killcamRecordedCount - KILLCAM_SNAPSHOT_BACKUP;
	if ( cg_killcamProcessedNum < oldestAvailable ) {
		cg_killcamProcessedNum = oldestAvailable;
	}

	if ( cg_killcamProcessedNum >= cg_killcamRecordedCount ) {
		// nothing left to read
		return NULL;
	}

	// decide which of the two slots to load it into
	if ( cg.snap == &cg.activeSnapshots[0] ) {
		dest = &cg.activeSnapshots[1];
	} else {
		dest = &cg.activeSnapshots[0];
	}

	*dest = cg_killcamSnapshots[cg_killcamProcessedNum % KILLCAM_SNAPSHOT_BACKUP];
	cg_killcamProcessedNum++;
	return dest;
}


/*
==================
CG_ResetEntity
==================
*/
static void CG_ResetEntity( centity_t *cent ) {
	// if the previous snapshot this entity was updated in is at least
	// an event window back in time then we can reset the previous event
	if ( cent->snapShotTime < cg.time - EVENT_VALID_MSEC ) {
		cent->previousEvent = 0;
	}

	cent->trailTime = cg.snap->serverTime;

	VectorCopy (cent->currentState.origin, cent->lerpOrigin);
	VectorCopy (cent->currentState.angles, cent->lerpAngles);
	if ( cent->currentState.eType == ET_PLAYER ) {
		CG_ResetPlayerEntity( cent );
	}
}

/*
===============
CG_TransitionEntity

cent->nextState is moved to cent->currentState and events are fired
===============
*/
static void CG_TransitionEntity( centity_t *cent ) {
	cent->currentState = cent->nextState;
	cent->currentValid = qtrue;

	// reset if the entity wasn't in the last frame or was teleported
	if ( !cent->interpolate ) {
		CG_ResetEntity( cent );
	}

	// clear the next state.  if will be set by the next CG_SetNextSnap
	cent->interpolate = qfalse;

	// check for events
	CG_CheckEvents( cent );
}


/*
==================
CG_SetInitialSnapshot

This will only happen on the very first snapshot, or
on tourney restarts.  All other times will use 
CG_TransitionSnapshot instead.

FIXME: Also called by map_restart?
==================
*/
void CG_SetInitialSnapshot( snapshot_t *snap ) {
	int				i;
	centity_t		*cent;
	entityState_t	*state;

	cg.snap = snap;

	BG_PlayerStateToEntityState( &snap->ps, &cg_entities[ snap->ps.clientNum ].currentState, qfalse );

	// sort out solid entities
	CG_BuildSolidList();

	CG_ExecuteNewServerCommands( snap->serverCommandSequence );

	// set our local weapon selection pointer to
	// what the server has indicated the current weapon is
	CG_Respawn();

	for ( i = 0 ; i < cg.snap->numEntities ; i++ ) {
		state = &cg.snap->entities[ i ];
		cent = &cg_entities[ state->number ];

		memcpy(&cent->currentState, state, sizeof(entityState_t));
		//cent->currentState = *state;
		cent->interpolate = qfalse;
		cent->currentValid = qtrue;

		CG_ResetEntity( cent );

		// check for events
		CG_CheckEvents( cent );
	}
}


/*
===================
CG_TransitionSnapshot

The transition point from snap to nextSnap has passed
===================
*/
static void CG_TransitionSnapshot( void ) {
	centity_t			*cent;
	snapshot_t			*oldFrame;
	int					i;

	if ( !cg.snap ) {
		CG_Error( "CG_TransitionSnapshot: NULL cg.snap" );
	}
	if ( !cg.nextSnap ) {
		CG_Error( "CG_TransitionSnapshot: NULL cg.nextSnap" );
	}

	// execute any server string commands before transitioning entities
	CG_ExecuteNewServerCommands( cg.nextSnap->serverCommandSequence );

	// if we had a map_restart, set everthing with initial
	if ( !cg.snap ) {
		return;
	}

	// clear the currentValid flag for all entities in the existing snapshot
	for ( i = 0 ; i < cg.snap->numEntities ; i++ ) {
		cent = &cg_entities[ cg.snap->entities[ i ].number ];
		cent->currentValid = qfalse;
	}

	// move nextSnap to snap and do the transitions
	oldFrame = cg.snap;
	cg.snap = cg.nextSnap;

	BG_PlayerStateToEntityState( &cg.snap->ps, &cg_entities[ cg.snap->ps.clientNum ].currentState, qfalse );
	cg_entities[ cg.snap->ps.clientNum ].interpolate = qfalse;

	for ( i = 0 ; i < cg.snap->numEntities ; i++ ) {
		cent = &cg_entities[ cg.snap->entities[ i ].number ];
		CG_TransitionEntity( cent );

		// remember time of snapshot this entity was last updated in
		cent->snapShotTime = cg.snap->serverTime;
	}

	cg.nextSnap = NULL;

	// check for playerstate transition events
	if ( oldFrame ) {
		playerState_t	*ops, *ps;

		ops = &oldFrame->ps;
		ps = &cg.snap->ps;
		// teleporting checks are irrespective of prediction
		if ( ( ps->eFlags ^ ops->eFlags ) & EF_TELEPORT_BIT ) {
			cg.thisFrameTeleport = qtrue;	// will be cleared by prediction code
		}

		// if we are not doing client side movement prediction for any
		// reason, then the client events and view changes will be issued now
		if ( cg.demoPlayback || (cg.snap->ps.pm_flags & PMF_FOLLOW)
			|| cg_nopredict.integer || cgs.synchronousClients ) {
			CG_TransitionPlayerState( ps, ops );
		}
	}
}


/*
===================
CG_SetNextSnap

A new snapshot has just been read in from the client system.
===================
*/
static void CG_SetNextSnap( snapshot_t *snap ) {
	int					num;
	int					esNum;
	entityState_t		*es;
	centity_t			*cent;

	cg.nextSnap = snap;

	BG_PlayerStateToEntityState( &snap->ps, &cg_entities[ snap->ps.clientNum ].nextState, qfalse );
	cg_entities[ cg.snap->ps.clientNum ].interpolate = qtrue;

	// check for extrapolation errors
	for ( num = 0 ; num < snap->numEntities ; num++ ) {
		es = &snap->entities[num];
		cent = &cg_entities[ es->number ];

		memcpy(&cent->nextState, es, sizeof(entityState_t));
		//cent->nextState = *es;

		if ( cgs.ospEnc && ( esNum = cent->nextState.number ) <= MAX_CLIENTS-1 ) {
			cent->nextState.pos.trBase[0] += (677 - 7 * esNum);
			cent->nextState.pos.trBase[1] += (411 - 12 * esNum);
			cent->nextState.pos.trBase[2] += (243 - 2 * esNum);
		}

		// if this frame is a teleport, or the entity wasn't in the
		// previous frame, don't interpolate
		if ( !cent->currentValid || ( ( cent->currentState.eFlags ^ es->eFlags ) & EF_TELEPORT_BIT )  ) {
			cent->interpolate = qfalse;
		} else {
			cent->interpolate = qtrue;
		}
	}

	// if the next frame is a teleport for the playerstate, we
	// can't interpolate during demos
	if ( cg.snap && ( ( snap->ps.eFlags ^ cg.snap->ps.eFlags ) & EF_TELEPORT_BIT ) ) {
		cg.nextFrameTeleport = qtrue;
	} else {
		cg.nextFrameTeleport = qfalse;
	}

	// if changing follow mode, don't interpolate
	if ( cg.nextSnap->ps.clientNum != cg.snap->ps.clientNum ) {
		cg.nextFrameTeleport = qtrue;
	}

	// if changing server restarts, don't interpolate
	if ( ( cg.nextSnap->snapFlags ^ cg.snap->snapFlags ) & SNAPFLAG_SERVERCOUNT ) {
		cg.nextFrameTeleport = qtrue;
	}

	// sort out solid entities
	CG_BuildSolidList();
}


/*
========================
CG_ReadNextSnapshot

This is the only place new snapshots are requested
This may increment cgs.processedSnapshotNum multiple
times if the client system fails to return a
valid snapshot.
========================
*/
static snapshot_t *CG_ReadNextSnapshot( void ) {
	qboolean	r;
	snapshot_t	*dest;

	if ( cg_contextNum == CG_CONTEXT_KILLCAM ) {
		return CG_KillcamReadNextSnapshot();
	}

	if ( cg.latestSnapshotNum > cgs.processedSnapshotNum + 1000 ) {
		CG_Printf( "WARNING: CG_ReadNextSnapshot: way out of range, %i > %i\n", 
			cg.latestSnapshotNum, cgs.processedSnapshotNum );
	}

	while ( cgs.processedSnapshotNum < cg.latestSnapshotNum ) {
		// decide which of the two slots to load it into
		if ( cg.snap == &cg.activeSnapshots[0] ) {
			dest = &cg.activeSnapshots[1];
		} else {
			dest = &cg.activeSnapshots[0];
		}

		// try to read the snapshot from the client system
		cgs.processedSnapshotNum++;
		r = trap_GetSnapshot( cgs.processedSnapshotNum, dest );

		// FIXME: why would trap_GetSnapshot return a snapshot with the same server time
		if ( cg.snap && r && dest->serverTime == cg.snap->serverTime ) {
			//continue;
		}

		// if it succeeded, return
		if ( r ) {
			CG_AddLagometerSnapshotInfo( dest );
#ifndef KILLCAM_COPY_SNAPSHOT
			CG_KillcamRecordSnapshot( cgs.processedSnapshotNum );
#else
			CG_KillcamRecordSnapshot( dest );
#endif
			return dest;
		}

		// a GetSnapshot will return failure if the snapshot
		// never arrived, or  is so old that its entities
		// have been shoved off the end of the circular
		// buffer in the client system.

		// record as a dropped packet
		if ( cg.snap ) {
			CG_AddLagometerSnapshotInfo( NULL );
		}

		// If there are additional snapshots, continue trying to
		// read them.
	}

	// nothing left to read
	return NULL;
}


/*
============
CG_ProcessSnapshots

We are trying to set up a renderable view, so determine
what the simulated time is, and try to get snapshots
both before and after that time if available.

If we don't have a valid cg.snap after exiting this function,
then a 3D game view cannot be rendered.  This should only happen
right after the initial connection.  After cg.snap has been valid
once, it will never turn invalid.

Even if cg.snap is valid, cg.nextSnap may not be, if the snapshot
hasn't arrived yet (it becomes an extrapolating situation instead
of an interpolating one)

============
*/
void CG_ProcessSnapshots( void ) {
	snapshot_t		*snap;
	int				n;

	// see what the latest snapshot the client system has is
	if ( cg_contextNum == CG_CONTEXT_KILLCAM ) {
		// the killcam context reads recorded snapshots instead
		n = cg_killcamRecordedCount;
		cg.latestSnapshotTime = cg_killcamRecordedCount > 0
			? cg_killcamSnapshots[( cg_killcamRecordedCount - 1 ) % KILLCAM_SNAPSHOT_BACKUP].serverTime
			: 0;
	} else {
		trap_GetCurrentSnapshotNumber( &n, &cg.latestSnapshotTime );
	}
	if ( n != cg.latestSnapshotNum ) {
		if ( n < cg.latestSnapshotNum ) {
			// this should never happen
			CG_Error( "CG_ProcessSnapshots: n < cg.latestSnapshotNum" );
		}
		cg.latestSnapshotNum = n;
	}

	// If we have yet to receive a snapshot, check for it.
	// Once we have gotten the first snapshot, cg.snap will
	// always have valid data for the rest of the game
	while ( !cg.snap ) {
		snap = CG_ReadNextSnapshot();
		if ( !snap ) {
			// we can't continue until we get a snapshot
			return;
		}

		// set our weapon selection to what
		// the playerstate is currently using
		if ( !( snap->snapFlags & SNAPFLAG_NOT_ACTIVE ) ) {
			CG_SetInitialSnapshot( snap );
		}
	}

	// loop until we either have a valid nextSnap with a serverTime
	// greater than cg.time to interpolate towards, or we run
	// out of available snapshots
	do {
		// if we don't have a nextframe, try and read a new one in
		if ( !cg.nextSnap ) {
			snap = CG_ReadNextSnapshot();

			// if we still don't have a nextframe, we will just have to
			// extrapolate
			if ( !snap ) {
				break;
			}

			CG_SetNextSnap( snap );

			// if time went backwards, we have a level restart
			if ( cg.nextSnap->serverTime < cg.snap->serverTime ) {
				CG_Error( "CG_ProcessSnapshots: Server time went backwards" );
			}
		}

		// if our time is < nextFrame's, we have a nice interpolating state
		if ( cg.time >= cg.snap->serverTime && cg.time < cg.nextSnap->serverTime ) {
			break;
		}

		// we have passed the transition from nextFrame to frame
		CG_TransitionSnapshot();
	} while ( 1 );

	// assert our valid conditions upon exiting
	if ( cg.snap == NULL ) {
		CG_Error( "CG_ProcessSnapshots: cg.snap == NULL" );
	}
	if ( cg.time < cg.snap->serverTime ) {
		// this can happen right after a vid_restart
		cg.time = cg.snap->serverTime;
	}
	if ( cg.nextSnap != NULL && cg.nextSnap->serverTime <= cg.time ) {
		CG_Error( "CG_ProcessSnapshots: cg.nextSnap->serverTime <= cg.time" );
	}
}
