/*
 * RejoinTest.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"

#include <gtest/gtest.h>

#include "../../server/activities/ActivityProcessor.h"
#include "../../server/activities/BattleActivities.h"
#include "../../server/activities/MapActivities.h"
#include "../../server/TurnTimerHandler.h"
#include "../../server/battles/BattleProcessor.h"
#include "CGameHandler.h"
#include "CVCMIServer.h"

#include "mock/GameHandlerTestServer.h"
#include "mock/TinyH3MBuilder.h"
#include "mock/TinyMapGameTest.h"

#include "lib/battle/BattleInfo.h"
#include "lib/battle/Unit.h"
#include "lib/mapObjects/CGHeroInstance.h"
#include "lib/networkPacks/PacksForClientBattle.h"
#include "lib/networkPacks/PacksForLobby.h"
#include "lib/battle/BattleAction.h"
#include "lib/gameState/ReplayLog.h"
#include "lib/CStack.h"
#include "lib/GameLibrary.h"
#include "lib/texts/MetaString.h"

namespace
{

template<typename T>
std::vector<const T *> repeatedPacksOfType(const GameHandlerTestServer & server)
{
	std::vector<const T *> result;
	for(const auto & pack : server.repeatedPacks)
		if(const auto * typed = dynamic_cast<const T *>(pack.get()))
			result.push_back(typed);
	return result;
}

const battle::Unit * unitOf(const BattleInfo & battle, PlayerColor player)
{
	for(const auto * unit : battle.battleGetAllStacks(false))
		if(battle.battleGetOwner(unit) == player)
			return unit;
	throw std::runtime_error("Player has no unit in battle");
}

class RejoinTest : public TinyMapGameTest
{
protected:
	static constexpr int AI_PLAYER = 1;

	void configurePlayer(PlayerSettings & settings) const override
	{
		if(settings.color == PlayerColor(AI_PLAYER))
			settings.connectedPlayerIDs.clear(); // a player with no connection is an AI
	}

	void buildHeroAboutToLevel()
	{
		TinyH3M::TinyH3MBuilder builder(EMapFormat::SOD);
		builder.size(36, false)
			.playerActive(PlayerColor(0))
			.hero(int3(5, 5, 0), HeroTypeID(0), PlayerColor(0))
			.heroExperience(999);
		startWithMap(std::move(builder));
	}

	void buildTwoPlayerMap()
	{
		TinyH3M::TinyH3MBuilder builder(EMapFormat::SOD);
		builder.size(36, false)
			.playerActive(PlayerColor(0))
			.playerActive(PlayerColor(AI_PLAYER))
			.hero(int3(5, 5, 0), HeroTypeID(0), PlayerColor(0))
			.heroGarrison({{CreatureID(0), 10}})
			.hero(int3(8, 8, 0), HeroTypeID(1), PlayerColor(AI_PLAYER))
			.heroGarrison({{CreatureID(0), 10}});
		startWithMap(std::move(builder));
	}
};

}

TEST_F(RejoinTest, unansweredLevelUpIsAskedAgainAndCanBeAnswered)
{
	const PlayerColor player(0);
	buildHeroAboutToLevel();

	auto * hero = findHeroByOwner(player);
	ASSERT_NE(hero, nullptr);

	GameHandlerTestServer server(gameState(), player);
	CGameHandler gameHandler(server, gameState());

	gameHandler.giveExperience(hero, 1000);
	auto pending = gameHandler.activities->topActivity(player);
	ASSERT_NE(pending, nullptr);
	const auto levelBefore = hero->level;

	gameHandler.handleClientRejoin(GameConnectionID::FIRST_CONNECTION);

	// The client of a rejoined player never saw the dialog, it has to be asked by the same question id
	const auto levelUps = repeatedPacksOfType<HeroLevelUp>(server);
	ASSERT_EQ(levelUps.size(), 1u);
	EXPECT_EQ(levelUps.front()->questionID, pending->getActiveQuestionID());
	EXPECT_EQ(hero->level, levelBefore) << "repeating the question must not level the hero again";

	EXPECT_EQ(gameHandler.activities->submitReply(levelUps.front()->questionID, player, 0), ReplyOutcome::Accepted);
}

TEST_F(RejoinTest, pauseOfDisconnectedPlayerEndsAndTheirTimerRunsAgain)
{
	const PlayerColor player(0);
	buildHeroAboutToLevel();

	GameHandlerTestServer server(gameState(), player);
	CGameHandler gameHandler(server, gameState());

	auto pause = std::make_shared<TimerPauseActivity>(&gameHandler, player);
	pause->expectAnswerTo(QuestionID::CLIENT);
	gameHandler.activities->addActivity(pause);
	ASSERT_FALSE(gameHandler.turnTimerHandler->isTimerEnabled(player));

	// only the player could end the pause, and the rejoined client does not know about it
	gameHandler.handleClientDisconnection(GameConnectionID::FIRST_CONNECTION);

	EXPECT_EQ(gameHandler.activities->topActivity(player), nullptr);
	EXPECT_TRUE(gameHandler.turnTimerHandler->isTimerEnabled(player));
}

TEST_F(RejoinTest, notificationTopicIsShortAndStaysTheSameForThePlayer)
{
	const PlayerColor player(0);
	buildTwoPlayerMap();

	GameHandlerTestServer server(gameState(), player);
	CGameHandler gameHandler(server, gameState());
	gameHandler.notificationSecret = "secret";

	const auto topic = gameHandler.notificationTopic(player);
	EXPECT_TRUE(std::regex_match(topic, std::regex("vcmi-[a-z2-9]{4}"))) << topic;

	// a restarted server computes it again from the save
	CGameHandler restarted(server, gameState());
	restarted.notificationSecret = "secret";
	EXPECT_EQ(restarted.notificationTopic(player), topic);

	restarted.notificationSecret = "secret of another server";
	EXPECT_NE(restarted.notificationTopic(player), topic);
	EXPECT_NE(gameHandler.notificationTopic(PlayerColor(AI_PLAYER)), topic);
}

TEST_F(RejoinTest, rejoinedPlayerGetsOwnNotificationTopicOnlyIfServerSendsNotifications)
{
	const PlayerColor player(0);
	buildTwoPlayerMap();

	GameHandlerTestServer server(gameState(), player);
	server.alsoHostedPlayers = {PlayerColor(AI_PLAYER)};
	CGameHandler gameHandler(server, gameState());

	EXPECT_FALSE(gameHandler.sendNotificationTopics(GameConnectionID::FIRST_CONNECTION));
	gameHandler.handleClientRejoin(GameConnectionID::FIRST_CONNECTION);
	EXPECT_TRUE(server.sentSystemMessages.empty());

	gameHandler.notificationSecret = "secret";
	gameHandler.handleClientRejoin(GameConnectionID::FIRST_CONNECTION);

	// the AI player of the same connection has no turns to be notified of
	ASSERT_EQ(server.sentSystemMessages.size(), 1u);
	EXPECT_EQ(server.sentSystemMessages.front().first, GameConnectionID::FIRST_CONNECTION);
	EXPECT_NE(server.sentSystemMessages.front().second.toString(LIBRARY->staticTexts()).find(gameHandler.notificationTopic(player)), std::string::npos);
}

TEST_F(RejoinTest, answeredQuestionsAreNotRepeated)
{
	const PlayerColor player(0);
	buildHeroAboutToLevel();

	auto * hero = findHeroByOwner(player);
	ASSERT_NE(hero, nullptr);

	GameHandlerTestServer server(gameState(), player);
	CGameHandler gameHandler(server, gameState());

	gameHandler.giveExperience(hero, 1000);
	while(auto pending = gameHandler.activities->topActivity(player))
		ASSERT_EQ(gameHandler.activities->submitReply(pending->getActiveQuestionID(), player, 0), ReplyOutcome::Accepted);

	gameHandler.handleClientRejoin(GameConnectionID::FIRST_CONNECTION);

	EXPECT_TRUE(server.repeatedPacks.empty());
}

TEST_F(RejoinTest, battleInProgressIsOpenedWithTheUnitThatWaitsForOrders)
{
	buildTwoPlayerMap();

	auto * attacker = findHeroByOwner(PlayerColor(0));
	auto * defender = findHeroByOwner(PlayerColor(AI_PLAYER));
	ASSERT_NE(attacker, nullptr);
	ASSERT_NE(defender, nullptr);

	GameHandlerTestServer server(gameState(), PlayerColor(0));
	CGameHandler gameHandler(server, gameState());

	gameHandler.battles->startBattle(attacker, defender);
	auto * battle = gameState()->getBattle(gameState()->getBattle(PlayerColor(0))->getBattleID());
	ASSERT_NE(battle, nullptr);
	battle->activeStack = unitOf(*battle, PlayerColor(0))->unitId();

	gameHandler.handleClientRejoin(GameConnectionID::FIRST_CONNECTION);

	ASSERT_EQ(server.resumedBattles.size(), 1u);
	EXPECT_EQ(server.resumedBattles.front().battleID, battle->getBattleID());
	EXPECT_EQ(server.resumedBattles.front().players, std::vector<PlayerColor>{PlayerColor(0)});

	const auto activations = repeatedPacksOfType<BattleSetActiveStack>(server);
	ASSERT_EQ(activations.size(), 1u);
	EXPECT_EQ(activations.front()->stack, static_cast<uint32_t>(battle->activeStack));
}

TEST_F(RejoinTest, newHostOpensTheBattleOnlyForTheAiSideItTookOver)
{
	buildTwoPlayerMap();

	auto * attacker = findHeroByOwner(PlayerColor(0));
	auto * defender = findHeroByOwner(PlayerColor(AI_PLAYER));
	ASSERT_NE(attacker, nullptr);
	ASSERT_NE(defender, nullptr);

	// the human player of this client was already in the battle when the AI players came to it
	GameHandlerTestServer server(gameState(), PlayerColor(0));
	server.alsoHostedPlayers = {PlayerColor(AI_PLAYER)};
	CGameHandler gameHandler(server, gameState());

	gameHandler.battles->startBattle(attacker, defender);
	auto * battle = gameState()->getBattle(gameState()->getBattle(PlayerColor(0))->getBattleID());
	ASSERT_NE(battle, nullptr);
	battle->activeStack = unitOf(*battle, PlayerColor(0))->unitId();

	gameHandler.handleHostChange(GameConnectionID::FIRST_CONNECTION);

	ASSERT_EQ(server.resumedBattles.size(), 1u);
	EXPECT_EQ(server.resumedBattles.front().players, std::vector<PlayerColor>{PlayerColor(AI_PLAYER)});
	EXPECT_TRUE(repeatedPacksOfType<BattleSetActiveStack>(server).empty()) << "the unit of the human player is already active on this client";

	battle->activeStack = unitOf(*battle, PlayerColor(AI_PLAYER))->unitId();
	gameHandler.handleHostChange(GameConnectionID::FIRST_CONNECTION);

	const auto activations = repeatedPacksOfType<BattleSetActiveStack>(server);
	ASSERT_EQ(activations.size(), 1u);
	EXPECT_EQ(activations.front()->stack, static_cast<uint32_t>(battle->activeStack));
}

TEST_F(RejoinTest, newHostDoesNotGetQuestionsOfItsOwnHumanPlayerAgain)
{
	const PlayerColor player(0);
	buildHeroAboutToLevel();

	auto * hero = findHeroByOwner(player);
	ASSERT_NE(hero, nullptr);

	GameHandlerTestServer server(gameState(), player);
	CGameHandler gameHandler(server, gameState());

	gameHandler.giveExperience(hero, 1000);
	ASSERT_NE(gameHandler.activities->topActivity(player), nullptr);

	gameHandler.handleHostChange(GameConnectionID::FIRST_CONNECTION);

	EXPECT_TRUE(server.repeatedPacks.empty());
}

TEST_F(RejoinTest, battleWaitingForResultConfirmationRepeatsTheResultInsteadOfTheActiveUnit)
{
	buildTwoPlayerMap();

	auto * attacker = findHeroByOwner(PlayerColor(0));
	auto * defender = findHeroByOwner(PlayerColor(AI_PLAYER));
	ASSERT_NE(attacker, nullptr);
	ASSERT_NE(defender, nullptr);

	GameHandlerTestServer server(gameState(), PlayerColor(0));
	CGameHandler gameHandler(server, gameState());

	gameHandler.battles->startBattle(attacker, defender);
	gameHandler.battles->cheatBattleVictory(PlayerColor(0));

	auto resultDialog = gameHandler.activities->topActivity(PlayerColor(0));
	ASSERT_NE(gameHandler.activities->activityAs<BattleResultActivity>(resultDialog), nullptr);

	gameHandler.handleClientRejoin(GameConnectionID::FIRST_CONNECTION);

	EXPECT_EQ(server.resumedBattles.size(), 1u);
	EXPECT_TRUE(repeatedPacksOfType<BattleSetActiveStack>(server).empty());

	const auto results = repeatedPacksOfType<BattleResult>(server);
	ASSERT_EQ(results.size(), 1u);
	EXPECT_EQ(results.front()->questionID, resultDialog->getActiveQuestionID());
}

TEST_F(RejoinTest, battleInProgressIsRestoredFromTheDayStartAndThePacksSince)
{
	buildTwoPlayerMap();

	// the server records every game, the fixture has to turn it on
	gameState()->replayLog.configure(false, 1);

	auto * attacker = findHeroByOwner(PlayerColor(0));
	auto * defender = findHeroByOwner(PlayerColor(AI_PLAYER));
	ASSERT_NE(attacker, nullptr);
	ASSERT_NE(defender, nullptr);

	GameHandlerTestServer server(gameState(), PlayerColor(0));
	CGameHandler gameHandler(server, gameState());

	gameHandler.battles->startBattle(attacker, defender);
	const auto * battle = gameState()->getBattle(PlayerColor(0));
	ASSERT_NE(battle, nullptr);

	// progress past the start of the battle, which a serialized battle could not carry
	const auto * defendingUnit = battle->battleActiveUnit();
	ASSERT_NE(defendingUnit, nullptr);
	ASSERT_TRUE(gameHandler.battles->makePlayerBattleAction(battle->getBattleID(), battle->sideToPlayer(defendingUnit->unitSide()), BattleAction::makeDefend(defendingUnit)));
	ASSERT_NE(battle->getActiveStackID(), static_cast<int32_t>(defendingUnit->unitId()));

	LobbyRejoinGame rejoin;
	ASSERT_TRUE(gameHandler.fillRejoinGamestate(rejoin));

	auto restored = std::make_shared<CGameState>();
	restored->preInit(gameServices());
	restored->loadFromMemory(rejoin.gamestate, rejoin.version);
	for(const auto & data : rejoin.packsSinceGamestate)
	{
		auto pack = ReplayPackSerializer::read(data, restored.get(), rejoin.version);
		restored->apply(dynamic_cast<CPackForClient &>(*pack));
	}

	const auto * restoredBattle = restored->getBattle(battle->getBattleID());
	ASSERT_NE(restoredBattle, nullptr);
	EXPECT_EQ(restoredBattle->getActiveStackID(), battle->getActiveStackID());

	const auto units = battle->battleGetAllStacks(true);
	ASSERT_EQ(restoredBattle->battleGetAllStacks(true).size(), units.size());
	for(const auto * unit : units)
	{
		const auto * restoredUnit = restoredBattle->battleGetStackByID(unit->unitId(), false);
		ASSERT_NE(restoredUnit, nullptr);
		EXPECT_EQ(restoredUnit->getPosition(), unit->getPosition());
		EXPECT_EQ(restoredUnit->getCount(), unit->getCount());
		EXPECT_EQ(restoredUnit->defending, unit->defending);
	}
}

namespace
{

CVCMIServer::RejoinCandidate seat(int playerId, const std::string & name, const std::string & deviceId, bool online, bool replaceable = true)
{
	CVCMIServer::RejoinCandidate result;
	result.playerId = PlayerConnectionID(playerId);
	result.name = name;
	result.deviceId = deviceId;
	result.online = online;
	result.replaceable = online && replaceable;
	return result;
}

}

TEST(RejoinSeatsTest, deviceGetsItsOwnSeatWhenPlayersShareTheName)
{
	const std::vector seats = {seat(1, "Player", "phone", false), seat(2, "Player", "laptop", false)};

	EXPECT_EQ(CVCMIServer::chooseRejoinSeats(seats, {"Player"}, "laptop"), std::vector{PlayerConnectionID(2)});
	EXPECT_EQ(CVCMIServer::chooseRejoinSeats(seats, {"Player"}, "phone"), std::vector{PlayerConnectionID(1)});
}

TEST(RejoinSeatsTest, anotherDeviceTakesNoSeatOfSharedName)
{
	const std::vector seats = {seat(1, "Player", "phone", true), seat(2, "Player", "laptop", false)};

	EXPECT_TRUE(CVCMIServer::chooseRejoinSeats(seats, {"Player"}, "tablet").empty());
	EXPECT_TRUE(CVCMIServer::chooseRejoinSeats(seats, {"Player"}, "").empty()) << "a client without device id is not the owner either";
}

TEST(RejoinSeatsTest, deviceTakesBackItsSeatFromConnectionThatWasNotClosed)
{
	const std::vector seats = {seat(1, "Player", "phone", true), seat(2, "Player", "laptop", false)};

	EXPECT_EQ(CVCMIServer::chooseRejoinSeats(seats, {"Player"}, "phone"), std::vector{PlayerConnectionID(1)});
	EXPECT_TRUE(CVCMIServer::chooseRejoinSeats({seat(1, "Player", "phone", true, false)}, {"Player"}, "phone").empty())
		<< "the host of a server started by a client can not be replaced";
}

TEST(RejoinSeatsTest, uniqueNameIsEnoughForAnotherDevice)
{
	EXPECT_EQ(CVCMIServer::chooseRejoinSeats({seat(1, "Alice", "phone", false)}, {"Alice"}, "new phone"), std::vector{PlayerConnectionID(1)});
	EXPECT_TRUE(CVCMIServer::chooseRejoinSeats({seat(1, "Alice", "phone", true)}, {"Alice"}, "new phone").empty());
}

TEST(RejoinSeatsTest, seatsOfUnknownDeviceGoToClientsWithTheirName)
{
	// a restarted server does not know the devices
	const std::vector seats = {seat(1, "Player", "", false), seat(2, "Player", "", false), seat(3, "Bob", "", false)};

	EXPECT_EQ(CVCMIServer::chooseRejoinSeats(seats, {"Player", "Player"}, "phone"), (std::vector{PlayerConnectionID(1), PlayerConnectionID(2)}));
	EXPECT_EQ(CVCMIServer::chooseRejoinSeats(seats, {"Bob"}, "phone"), std::vector{PlayerConnectionID(3)});
}
