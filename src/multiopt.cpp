/*
	This file is part of Warzone 2100.
	Copyright (C) 1999-2004  Eidos Interactive
	Copyright (C) 2005-2011  Warzone 2100 Project

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
 * MultiOpt.c
 *
 * Alex Lee,97/98, Pumpkin Studios
 *
 * Routines for setting the game options and starting the init process.
 */
#include "lib/framework/frame.h"			// for everything
#include "map.h"
#include "game.h"			// for loading maps
#include "message.h"		// for clearing messages.
#include "main.h"
#include "display3d.h"		// for changing the viewpoint
#include "power.h"
#include "lib/widget/widget.h"
#include "lib/gamelib/gtime.h"
#include "lib/netplay/netplay.h"
#include "hci.h"
#include "configuration.h"			// lobby cfg.
#include "clparse.h"
#include "lib/ivis_opengl/piestate.h"

#include "component.h"
#include "console.h"
#include "multiplay.h"
#include "lib/sound/audio.h"
#include "multijoin.h"
#include "frontend.h"
#include "levels.h"
#include "multistat.h"
#include "multiint.h"
#include "multilimit.h"
#include "multigifts.h"
#include "multiint.h"
#include "multirecv.h"
#include "scriptfuncs.h"

#include <SDL.h>
// ////////////////////////////////////////////////////////////////////////////
// External Variables

extern char	buildTime[8];

// ////////////////////////////////////////////////////////////////////////////
// ////////////////////////////////////////////////////////////////////////////

// send complete game info set!
void sendOptions()
{
	unsigned int i;

	NETbeginEncode(NETbroadcastQueue(), NET_OPTIONS);

	// First send information about the game
	NETuint8_t(&game.type);
	NETstring(game.map, 128);
	NETuint8_t(&game.maxPlayers);
	NETstring(game.name, 128);
	NETbool(&game.fog);
	NETuint32_t(&game.power);
	NETuint8_t(&game.base);
	NETuint8_t(&game.alliance);
	NETbool(&game.scavengers);

	for (i = 0; i < MAX_PLAYERS; i++)
	{
		NETuint8_t(&game.skDiff[i]);
	}

	// Send the list of who is still joining
	for (i = 0; i < MAX_PLAYERS; i++)
	{
		NETbool(&ingame.JoiningInProgress[i]);
	}

	// Same goes for the alliances
	for (i = 0; i < MAX_PLAYERS; i++)
	{
		unsigned int j;

		for (j = 0; j < MAX_PLAYERS; j++)
		{
			NETuint8_t(&alliances[i][j]);
		}
	}

	// Send the number of structure limits to expect
	NETuint32_t(&ingame.numStructureLimits);
	debug(LOG_NET, "(Host) Structure limits to process on client is %u", ingame.numStructureLimits);
	// Send the structures changed
	for (i = 0; i < ingame.numStructureLimits; i++)
	{
		NETuint32_t(&ingame.pStructureLimits[i].id);
		NETuint32_t(&ingame.pStructureLimits[i].limit);
	}
	updateLimitFlags();
	NETuint8_t(&ingame.flags);

	NETend();
}

// ////////////////////////////////////////////////////////////////////////////
/*!
 * check the wdg files that are being used.
 */
static bool checkGameWdg(const char *nm)
{
	LEVEL_DATASET *lev;

	for (lev = psLevels; lev; lev = lev->psNext)
	{
		if (strcmp(lev->pName, nm) == 0)
		{
			return true;
		}
	}

	return false;
}

// ////////////////////////////////////////////////////////////////////////////
// options for a game. (usually recvd in frontend)
void recvOptions(NETQUEUE queue)
{
	unsigned int i;

	debug(LOG_NET, "Receiving options from host");
	NETbeginDecode(queue, NET_OPTIONS);

	// Get general information about the game
	NETuint8_t(&game.type);
	NETstring(game.map, 128);
	NETuint8_t(&game.maxPlayers);
	NETstring(game.name, 128);
	NETbool(&game.fog);
	NETuint32_t(&game.power);
	NETuint8_t(&game.base);
	NETuint8_t(&game.alliance);
	NETbool(&game.scavengers);

	for (i = 0; i < MAX_PLAYERS; i++)
	{
		NETuint8_t(&game.skDiff[i]);
	}

	// Send the list of who is still joining
	for (i = 0; i < MAX_PLAYERS; i++)
	{
		NETbool(&ingame.JoiningInProgress[i]);
	}

	// Alliances
	for (i = 0; i < MAX_PLAYERS; i++)
	{
		unsigned int j;

		for (j = 0; j < MAX_PLAYERS; j++)
		{
			NETuint8_t(&alliances[i][j]);
		}
	}
	netPlayersUpdated = true;

	// Free any structure limits we may have in-place
	if (ingame.numStructureLimits)
	{
		ingame.numStructureLimits = 0;
		free(ingame.pStructureLimits);
		ingame.pStructureLimits = NULL;
	}

	// Get the number of structure limits to expect
	NETuint32_t(&ingame.numStructureLimits);
	debug(LOG_NET, "Host is sending us %u structure limits", ingame.numStructureLimits);
	// If there were any changes allocate memory for them
	if (ingame.numStructureLimits)
	{
		ingame.pStructureLimits = (MULTISTRUCTLIMITS *)malloc(ingame.numStructureLimits * sizeof(MULTISTRUCTLIMITS));
	}

	for (i = 0; i < ingame.numStructureLimits; i++)
	{
		NETuint32_t(&ingame.pStructureLimits[i].id);
		NETuint32_t(&ingame.pStructureLimits[i].limit);
	}
	NETuint8_t(&ingame.flags);

	NETend();

	// Do the skirmish slider settings if they are up
	for (i = 0; i < MAX_PLAYERS; i++)
 	{
		if (widgGetFromID(psWScreen, MULTIOP_SKSLIDE + i))
		{
			widgSetSliderPos(psWScreen, MULTIOP_SKSLIDE + i, game.skDiff[i]);
		}
	}
	debug(LOG_NET, "Rebuilding map list");
	// clear out the old level list.
	levShutDown();
	levInitialise();
	rebuildSearchPath(mod_multiplay, true);	// MUST rebuild search path for the new maps we just got!
	buildMapList();
	// See if we have the map or not
	if (!checkGameWdg(game.map))
	{
		uint32_t player = selectedPlayer;

		debug(LOG_NET, "Map was not found, requesting map %s from host.", game.map);
		// Request the map from the host
		NETbeginEncode(NETnetQueue(NET_HOST_ONLY), NET_FILE_REQUESTED);
		NETuint32_t(&player);
		NETend();

		addConsoleMessage("MAP REQUESTED!",DEFAULT_JUSTIFY, SYSTEM_MESSAGE);
	}
	else
	{
		loadMapPreview(false);
	}
}


// ////////////////////////////////////////////////////////////////////////////
// Host Campaign.
bool hostCampaign(char *sGame, char *sPlayer)
{
	PLAYERSTATS playerStats;
	UDWORD		i;

	debug(LOG_WZ, "Hosting campaign: '%s', player: '%s'", sGame, sPlayer);

	freeMessages();

	// If we had a single player (i.e. campaign) game before this value will
	// have been set to 0. So revert back to the default value.
	if (game.maxPlayers == 0)
	{
		game.maxPlayers = 4;
	}

	if (!NEThostGame(sGame, sPlayer, game.type, 0, 0, 0, game.maxPlayers))
	{
		return false;
	}

	for (i = 0; i < MAX_PLAYERS; i++)
	{
		if (NetPlay.bComms)
		{
			game.skDiff[i] = 0;     	// disable AI
		}
		setPlayerName(i, ""); //Clear custom names (use default ones instead)
	}

	NetPlay.players[selectedPlayer].ready = false;

	ingame.localJoiningInProgress = true;
	ingame.JoiningInProgress[selectedPlayer] = true;
	bMultiPlayer = true;
	bMultiMessages = true; // enable messages

	loadMultiStats(sPlayer,&playerStats);				// stats stuff
	setMultiStats(selectedPlayer, playerStats, false);
	setMultiStats(selectedPlayer, playerStats, true);

	if(!NetPlay.bComms)
	{
		strcpy(NetPlay.players[0].name,sPlayer);
	}

	return true;
}

// ////////////////////////////////////////////////////////////////////////////
// Join Campaign

bool joinCampaign(UDWORD gameNumber, char *sPlayer)
{
	PLAYERSTATS	playerStats;

	if(!ingame.localJoiningInProgress)
	{
		if (!NETjoinGame(gameNumber, sPlayer))	// join
		{
			return false;
		}
		ingame.localJoiningInProgress	= true;

		loadMultiStats(sPlayer,&playerStats);
		setMultiStats(selectedPlayer, playerStats, false);
		setMultiStats(selectedPlayer, playerStats, true);
		return true;
	}

	return false;
}

// ////////////////////////////////////////////////////////////////////////////
// Tell the host we are leaving the game 'nicely', (we wanted to) and not
// because we have some kind of error. (dropped or disconnected)
bool sendLeavingMsg(void)
{
	debug(LOG_NET, "We are leaving 'nicely'");
	NETbeginEncode(NETnetQueue(NET_HOST_ONLY), NET_PLAYER_LEAVING);
	{
		bool host = NetPlay.isHost;
		uint32_t id = selectedPlayer;

		NETuint32_t(&id);
		NETbool(&host);
	}
	NETend();

	return true;
}

// ////////////////////////////////////////////////////////////////////////////
// called in Init.c to shutdown the whole netgame gubbins.
bool multiShutdown(void)
{
	// shut down netplay lib.
	debug(LOG_MAIN, "shutting down networking");
  	NETshutdown();

	debug(LOG_MAIN, "free game data (structure limits)");
	if(ingame.numStructureLimits)
	{
		ingame.numStructureLimits = 0;
		free(ingame.pStructureLimits);
		ingame.pStructureLimits = NULL;
	}

	return true;
}

// ////////////////////////////////////////////////////////////////////////////
// copy templates from one player to another.
bool addTemplateToList(DROID_TEMPLATE *psNew, DROID_TEMPLATE **ppList)
{
	DROID_TEMPLATE *psTempl = new DROID_TEMPLATE(*psNew);
	psTempl->pName = NULL;

	if (psNew->pName)
	{
		psTempl->pName = strdup(psNew->pName);
	}

	psTempl->psNext = *ppList;
	*ppList = psTempl;

	return true;
}

// ////////////////////////////////////////////////////////////////////////////
// copy templates from one player to another.
bool addTemplate(UDWORD player, DROID_TEMPLATE *psNew)
{
	return addTemplateToList(psNew, &apsDroidTemplates[player]);
}

void addTemplateBack(unsigned player, DROID_TEMPLATE *psNew)
{
	DROID_TEMPLATE **ppList = &apsDroidTemplates[player];
	while (*ppList != NULL)
	{
		ppList = &(*ppList)->psNext;
	}
	addTemplateToList(psNew, ppList);
}

// ////////////////////////////////////////////////////////////////////////////
// setup templates
bool multiTemplateSetup(void)
{
	// do nothing now
	return true;
}

// ////////////////////////////////////////////////////////////////////////////
// remove structures from map before campaign play.
static bool cleanMap(UDWORD player)
{
	DROID		*psD,*psD2;
	STRUCTURE	*psStruct;
	bool		firstFact,firstRes;

	bMultiPlayer = false;
	bMultiMessages = false;

	firstFact = true;
	firstRes = true;

	switch(game.base)
	{
	case CAMP_CLEAN:									//clean map
		psStruct = apsStructLists[player];
		while (psStruct)						//strip away structures.
		{
			if ((psStruct->pStructureType->type != REF_WALL && psStruct->pStructureType->type != REF_WALLCORNER
			     && psStruct->pStructureType->type != REF_DEFENSE && psStruct->pStructureType->type != REF_GATE
			     && psStruct->pStructureType->type != REF_RESOURCE_EXTRACTOR)
			    || NetPlay.players[player].difficulty != DIFFICULTY_INSANE || NetPlay.players[player].allocated)
			{
				removeStruct(psStruct, true);
				psStruct = apsStructLists[player];			//restart,(list may have changed).
			}
			else
			{
				psStruct = psStruct->psNext;
			}
		}
		psD = apsDroidLists[player];					// remove all but construction droids.
		while(psD)
		{
			psD2=psD->psNext;
			if (psD->droidType != DROID_CONSTRUCT && psD->droidType != DROID_CYBORG_CONSTRUCT)
			{
				killDroid(psD);
			}
			psD = psD2;
		}
		break;

	case CAMP_BASE:												//just structs, no walls
		psStruct = apsStructLists[player];
		while(psStruct)
		{
			if (((psStruct->pStructureType->type == REF_WALL || psStruct->pStructureType->type == REF_WALLCORNER
			      || psStruct->pStructureType->type == REF_DEFENSE || psStruct->pStructureType->type == REF_GATE)
			     && (NetPlay.players[player].difficulty != DIFFICULTY_INSANE || NetPlay.players[player].allocated))
			    || psStruct->pStructureType->type == REF_BLASTDOOR
			    || psStruct->pStructureType->type == REF_CYBORG_FACTORY
			    || psStruct->pStructureType->type == REF_COMMAND_CONTROL)
			{
				removeStruct(psStruct, true);
				psStruct= apsStructLists[player];			//restart,(list may have changed).
			}
			else if( (psStruct->pStructureType->type == REF_FACTORY)
				   ||(psStruct->pStructureType->type == REF_RESEARCH)
				   ||(psStruct->pStructureType->type == REF_POWER_GEN))
			{
				if(psStruct->pStructureType->type == REF_FACTORY )
				{
					if(firstFact == true)
					{
						firstFact = false;
						removeStruct(psStruct, true);
						psStruct= apsStructLists[player];
					}
					else	// don't delete, just rejig!
					{
						if(((FACTORY*)psStruct->pFunctionality)->capacity != 0)
						{
							((FACTORY*)psStruct->pFunctionality)->capacity = 0;
							((FACTORY*)psStruct->pFunctionality)->productionOutput = (UBYTE)((PRODUCTION_FUNCTION*)psStruct->pStructureType->asFuncList[0])->productionOutput;

							psStruct->sDisplay.imd	= psStruct->pStructureType->pIMD;
							psStruct->body			= (UWORD)(structureBody(psStruct));

						}
						psStruct				= psStruct->psNext;
					}
				}
				else if(psStruct->pStructureType->type == REF_RESEARCH)
				{
					if(firstRes == true)
					{
						firstRes = false;
						removeStruct(psStruct, true);
						psStruct= apsStructLists[player];
					}
					else
					{
						if(((RESEARCH_FACILITY*)psStruct->pFunctionality)->capacity != 0)
						{	// downgrade research
							((RESEARCH_FACILITY*)psStruct->pFunctionality)->capacity = 0;
							((RESEARCH_FACILITY*)psStruct->pFunctionality)->researchPoints = ((RESEARCH_FUNCTION*)psStruct->pStructureType->asFuncList[0])->researchPoints;
							psStruct->sDisplay.imd	= psStruct->pStructureType->pIMD;
							psStruct->body			= (UWORD)(structureBody(psStruct));
						}
						psStruct=psStruct->psNext;
					}
				}
				else if(psStruct->pStructureType->type == REF_POWER_GEN)
				{
						if(((POWER_GEN*)psStruct->pFunctionality)->capacity != 0)
						{	// downgrade powergen.
							((POWER_GEN*)psStruct->pFunctionality)->capacity = 0;

							psStruct->sDisplay.imd	= psStruct->pStructureType->pIMD;
							psStruct->body			= (UWORD)(structureBody(psStruct));
						}
						structurePowerUpgrade(psStruct);
						psStruct=psStruct->psNext;
				}
			}

			else
			{
				psStruct=psStruct->psNext;
			}
		}
		break;


	case CAMP_WALLS:												//everything.
		break;
	default:
		debug( LOG_FATAL, "Unknown Campaign Style" );
		abort();
		break;
	}

	bMultiPlayer = true;
	bMultiMessages = true;
	return true;
}

// ////////////////////////////////////////////////////////////////////////////
static bool gameInit(void)
{
	UDWORD			player;

	// If this is from a savegame, stop here!
	if (getSaveGameType() == GTYPE_SAVE_START || getSaveGameType() == GTYPE_SAVE_MIDMISSION)
	{
		// these two lines are the biggest hack in the world.
		// the reticule seems to get detached from 'reticuleup'
		// this forces it back in sync...
		intRemoveReticule();
		intAddReticule();

		return true;
	}

	for(player = 0;player<game.maxPlayers;player++)			// clean up only to the player limit for this map..
	{
		cleanMap(player);
	}

	for (player = 1; player < MAX_PLAYERS; player++)
	{
		// we want to remove disabled AI & all the other players that don't belong
		if ((game.skDiff[player] == 0 || player >= game.maxPlayers) && player != scavengerPlayer())
		{
			clearPlayer(player, true);			// do this quietly
			debug(LOG_NET, "removing disabled AI (%d) from map.", player);
		}
	}

	if (game.scavengers)	// FIXME - not sure if we still need this hack - Per
	{
		// ugly hack for now
		game.skDiff[scavengerPlayer()] = DIFF_SLIDER_STOPS / 2;
	}

	if (NetPlay.isHost)	// add oil drums
	{
		addOilDrum(NetPlay.playercount * 2);
	}

	playerResponding();			// say howdy!

	return true;
}

// ////////////////////////////////////////////////////////////////////////////
// say hi to everyone else....
void playerResponding(void)
{
	ingame.startTime = gameTime;
	ingame.localJoiningInProgress = false; // No longer joining.
	ingame.JoiningInProgress[selectedPlayer] = false;

	// Home the camera to the player
	cameraToHome(selectedPlayer, false);

	// Tell the world we're here
	NETbeginEncode(NETbroadcastQueue(), NET_PLAYERRESPONDING);
	NETuint32_t(&selectedPlayer);
	NETend();
}

// ////////////////////////////////////////////////////////////////////////////
//called when the game finally gets fired up.
bool multiGameInit(void)
{
	UDWORD player;

	for (player = 0; player < MAX_PLAYERS; player++)
	{
		openchannels[player] =true;								//open comms to this player.
	}

	gameInit();
	msgStackReset();	//for multiplayer msgs, reset message stack

	return true;
}

////////////////////////////////
// at the end of every game.
bool multiGameShutdown(void)
{
	PLAYERSTATS	st;

	debug(LOG_NET,"%s is shutting down.",getPlayerName(selectedPlayer));

	sendLeavingMsg();							// say goodbye
	updateMultiStatsGames();					// update games played.

	st = getMultiStats(selectedPlayer);	// save stats

	saveMultiStats(getPlayerName(selectedPlayer), getPlayerName(selectedPlayer), &st);

	// if we terminate the socket too quickly, then, it is possible not to get the leave message
	SDL_Delay(1000);
	// close game
	NETclose();
	NETremRedirects();

	if (ingame.numStructureLimits)
	{
		ingame.numStructureLimits = 0;
		free(ingame.pStructureLimits);
		ingame.pStructureLimits = NULL;
	}
	ingame.flags = 0;

	ingame.localJoiningInProgress = false; // Clean up
	ingame.localOptionsReceived = false;
	ingame.bHostSetup = false;	// Dont attempt a host
	ingame.TimeEveryoneIsInGame = 0;
	ingame.startTime = 0;
	NetPlay.isHost					= false;
	bMultiPlayer					= false;	// Back to single player mode
	bMultiMessages					= false;
	selectedPlayer					= 0;		// Back to use player 0 (single player friendly)

	return true;
}
