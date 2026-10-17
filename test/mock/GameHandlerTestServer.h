/*
 * GameHandlerTestServer.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#pragma once

#include "../../server/IGameServer.h"
#include "../../lib/GameConstants.h"

#include "../../lib/networkPacks/PacksForClient.h"
#include "../../lib/networkPacks/PacksForClientBattle.h"

#include <memory>
#include <optional>
#include <vector>

class CGameState;

class GameHandlerTestServer : public IGameServer
{
public:
	explicit GameHandlerTestServer(std::shared_ptr<CGameState> gameState = nullptr);
	GameHandlerTestServer(std::shared_ptr<CGameState> gameState, PlayerColor hostedPlayer);

	void setState(EServerState value) override;
	EServerState getState() const override;
	bool isPlayerHost(const PlayerColor & color) const override;
	bool wasStartedByClient() const override;
	bool hasPlayerAt(PlayerColor player, GameConnectionID connectionID) const override;
	bool hasBothPlayersAtSameConnection(PlayerColor left, PlayerColor right) const override;
	bool isPlayerConnected(PlayerColor player) const override;
	void applyPack(CPackForClient & pack) override;
	void sendPack(CPackForClient & pack, GameConnectionID connectionID) override;

	/// Question ids of the level-up dialogs that the server sent, in order.
	std::vector<QuestionID> levelUpPromptIDs;

	/// Whether each of those dialogs announced a further level.
	std::vector<bool> levelUpMoreLevelsFollow;

	/// Question ids that the server reported as resolved, in order.
	std::vector<QuestionID> resolvedQuestionIDs;

	/// Number of battles whose result the server actually applied.
	int battlesConfirmed = 0;

	/// Packs that the server sent again to a rejoined client, in order.
	std::vector<std::shared_ptr<CPackForClient>> repeatedPacks;

	/// Battles that the server sent to a rejoined client, with the players that open them, in order.
	std::vector<BattleResumed> resumedBattles;

	/// Players that the connection of the hosted player controls as well, e.g. AI players of the host.
	std::vector<PlayerColor> alsoHostedPlayers;

	/// System messages that the server sent to a single connection, in order.
	std::vector<std::pair<GameConnectionID, MetaString>> sentSystemMessages;

	/// Players whose client is not connected, every other player is.
	std::vector<PlayerColor> offlinePlayers;

private:
	EServerState state = EServerState::LOBBY;
	std::shared_ptr<CGameState> gameState;
	std::optional<PlayerColor> hostedPlayer;
};
