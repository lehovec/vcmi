/*
 * GameHandlerTestServer.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#include "StdInc.h"

#include "GameHandlerTestServer.h"

#include "../../lib/gameState/CGameState.h"
#include "../../lib/networkPacks/PacksForClientBattle.h"

GameHandlerTestServer::GameHandlerTestServer(std::shared_ptr<CGameState> gameState)
	: gameState(std::move(gameState))
{
}

GameHandlerTestServer::GameHandlerTestServer(std::shared_ptr<CGameState> gameState, PlayerColor hostedPlayer)
	: state(EServerState::GAMEPLAY)
	, gameState(std::move(gameState))
	, hostedPlayer(hostedPlayer)
{
}

void GameHandlerTestServer::setState(EServerState value)
{
	state = value;
}

EServerState GameHandlerTestServer::getState() const
{
	return state;
}

bool GameHandlerTestServer::isPlayerHost(const PlayerColor & color) const
{
	return hostedPlayer == color;
}

bool GameHandlerTestServer::wasStartedByClient() const
{
	return true;
}

bool GameHandlerTestServer::hasPlayerAt(PlayerColor player, GameConnectionID connectionID) const
{
	return (hostedPlayer == player || vstd::contains(alsoHostedPlayers, player)) && connectionID == GameConnectionID::FIRST_CONNECTION;
}

bool GameHandlerTestServer::hasBothPlayersAtSameConnection(PlayerColor, PlayerColor) const
{
	return false;
}

bool GameHandlerTestServer::isPlayerConnected(PlayerColor player) const
{
	return !vstd::contains(offlinePlayers, player);
}

void GameHandlerTestServer::applyPack(CPackForClient & pack)
{
	// Record the packs that carry the client-facing activity contract, so that tests can
	// check what the client would receive.
	if(const auto * heroLevelUp = dynamic_cast<const HeroLevelUp *>(&pack))
	{
		levelUpPromptIDs.push_back(heroLevelUp->questionID);
		levelUpMoreLevelsFollow.push_back(heroLevelUp->moreLevelsFollow);
	}

	if(const auto * commanderLevelUp = dynamic_cast<const CommanderLevelUp *>(&pack))
	{
		levelUpPromptIDs.push_back(commanderLevelUp->questionID);
		levelUpMoreLevelsFollow.push_back(commanderLevelUp->moreLevelsFollow);
	}

	if(const auto * resolved = dynamic_cast<const QuestionResolved *>(&pack))
		resolvedQuestionIDs.push_back(resolved->questionID);

	if(dynamic_cast<const BattleResultAccepted *>(&pack))
		battlesConfirmed++;

	if(gameState)
		gameState->apply(pack);
}

void GameHandlerTestServer::sendPack(CPackForClient & pack, GameConnectionID connectionID)
{
	// Test state changes are applied through applyPack.
	if(const auto * message = dynamic_cast<const SystemMessage *>(&pack))
		sentSystemMessages.emplace_back(connectionID, message->text);

	if(const auto * repeated = dynamic_cast<const PackRepeatedOnRejoin *>(&pack))
		repeatedPacks.push_back(repeated->pack);

	if(const auto * resumed = dynamic_cast<const BattleResumed *>(&pack))
		resumedBattles.push_back(*resumed);
}
