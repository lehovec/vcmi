/*
 * CVCMIServer.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "CVCMIServer.h"

#include "CGameHandler.h"
#include "GlobalLobbyProcessor.h"
#include "LobbyNetPackVisitors.h"
#include "processors/PlayerMessageProcessor.h"

#include "../lib/CThreadHelper.h"
#include "../lib/GameLibrary.h"
#include "../lib/CPlayerState.h"
#include "../lib/campaign/CampaignState.h"
#include "../lib/entities/hero/CHeroHandler.h"
#include "../lib/entities/hero/CHeroClass.h"
#include "../lib/entities/ResourceTypeHandler.h"
#include "../lib/gameState/CGameState.h"
#include "../lib/filesystem/Filesystem.h"
#include "../lib/filesystem/SavegamePath.h"
#include "../lib/mapping/CMapInfo.h"
#include "../lib/texts/CompositeTranslator.h"
#include "../lib/mapping/CMapHeader.h"
#include "../lib/modding/CModHandler.h"
#include "../lib/modding/ModDescription.h"
#include "../lib/modding/ModIncompatibility.h"
#include "../lib/rmg/CMapGenOptions.h"
#include "../lib/serializer/CMemorySerializer.h"
#include "../lib/serializer/GameConnection.h"
#include "../lib/texts/CGeneralTextHandler.h"

// UUID generation
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/program_options.hpp>

class CVCMIServerPackVisitor : public ::ICPackVisitor
{
private:
	CVCMIServer & handler;
	std::shared_ptr<CGameHandler> gh;
	std::shared_ptr<GameConnection> connection;

public:
	CVCMIServerPackVisitor(CVCMIServer & handler, const std::shared_ptr<CGameHandler> & gh, const std::shared_ptr<GameConnection> & connection)
		: handler(handler)
		, gh(gh)
		, connection(connection)
	{
	}

	bool callTyped() override { return false; }

	void visitForLobby(CPackForLobby & packForLobby) override
	{
		handler.handleReceivedPack(connection, packForLobby);
	}

	void visitForServer(CPackForServer & serverPack) override
	{
		if (gh)
			gh->handleReceivedPack(connection->connectionID, serverPack);
		else
			logNetwork->error("Received pack for game server while in lobby!");
	}

	void visitForClient(CPackForClient & clientPack) override
	{
	}
};

namespace
{

/// Persistent device id that a client puts in front of its uuid, empty for an older client
std::string deviceIdOf(const std::string & clientUuid)
{
	const auto separator = clientUuid.find(LobbyClientConnected::DEVICE_ID_SEPARATOR);
	if(separator == std::string::npos)
		return {};
	return clientUuid.substr(0, separator);
}

}

CVCMIServer::CVCMIServer(uint16_t port, bool runByClient)
	: currentClientId(GameConnectionID::FIRST_CONNECTION)
	, currentPlayerId(PlayerConnectionID::FIRST_HUMAN)
	, port(port)
	, runByClient(runByClient)
{
	uuid = boost::uuids::to_string(boost::uuids::random_generator()());
	logNetwork->trace("CVCMIServer created! UUID: %s", uuid);

	networkHandler = INetworkHandler::createHandler();

	if(state == EServerState::LOBBY)
		startDiscoveryListener();
}

CVCMIServer::~CVCMIServer()
{
	stopDiscoveryListener();
}

uint16_t CVCMIServer::prepare(bool connectToLobby, bool listenForConnections) {
	if(connectToLobby) {
		lobbyProcessor = std::make_unique<GlobalLobbyProcessor>(*this);
		return 0;
	} else {
		return startAcceptingIncomingConnections(listenForConnections);
	}
}

uint16_t CVCMIServer::startAcceptingIncomingConnections(bool listenForConnections)
{
	networkServer = networkHandler->createServerTCP(*this);

	port
		? logNetwork->info("Port %d will be used", port)
		: logNetwork->info("Randomly assigned port will be used");

	// config port may be 0 => srvport will contain the OS-assigned port value

	if (listenForConnections)
	{
		auto srvport = networkServer->start(port);
		logNetwork->info("Listening for connections at port %d", srvport);
		return srvport;
	}
	else
		return 0;
}

void CVCMIServer::onNewConnection(const std::shared_ptr<INetworkConnection> & connection)
{
	if(getState() == EServerState::LOBBY)
	{
		activeConnections.push_back(std::make_shared<GameConnection>(connection));
		activeConnections.back()->enterLobbyConnectionMode();
	}
	else if(getState() == EServerState::GAMEPLAY)
	{
		pendingConnections.push_back(std::make_shared<GameConnection>(connection));
		pendingConnections.back()->enterLobbyConnectionMode();
	}
	else
	{
		connection->close();
	}
}

void CVCMIServer::onPacketReceived(const std::shared_ptr<INetworkConnection> & connection, const std::vector<std::byte> & message)
{
	std::shared_ptr<GameConnection> c = findConnection(connection);
	if (c == nullptr)
		throw std::out_of_range("Unknown connection received in CVCMIServer::findConnection");

	auto pack = c->retrievePack(message);

	if(isPendingConnection(c) && dynamic_cast<CPackForLobby *>(pack.get()) == nullptr)
	{
		const CPack & receivedPack = *pack;
		logNetwork->error("Received game pack %s from connection that has not rejoined the game", typeid(receivedPack).name());
		return;
	}

	CVCMIServerPackVisitor visitor(*this, this->gh, c);
	pack->visit(visitor);
}

void CVCMIServer::setState(EServerState value)
{
	if (value == EServerState::SHUTDOWN && state == EServerState::SHUTDOWN)
		logGlobal->warn("Attempt to shutdown already shutdown server!");

	// do not attempt to restart dying server
	assert(state != EServerState::SHUTDOWN || state == value);

	if(state != EServerState::LOBBY && value == EServerState::LOBBY && discoveryListener)
		startDiscoveryListener();
	if(state == EServerState::LOBBY && value != EServerState::LOBBY && discoveryListener)
		stopDiscoveryListener();

	state = value;

	if (state == EServerState::SHUTDOWN)
		networkHandler->stop();
}
void CVCMIServer::startDiscoveryListener()
{
	if(!discoveryListener)
		discoveryListener = getNetworkHandler().createServerDiscoveryListener(*this);

	discoveryListener->start();
}

void CVCMIServer::stopDiscoveryListener()
{
	if(discoveryListener)
	{
		discoveryListener->stop();
		discoveryListener.reset();
	}
}

EServerState CVCMIServer::getState() const
{
	return state;
}

bool CVCMIServer::isInLobby() const
{
	return getState() == EServerState::LOBBY;
}

std::shared_ptr<GameConnection> CVCMIServer::findConnection(const std::shared_ptr<INetworkConnection> & netConnection)
{
	for(const auto & gameConnection : activeConnections)
	{
		if (gameConnection->isMyConnection(netConnection))
			return gameConnection;
	}

	for(const auto & gameConnection : pendingConnections)
	{
		if (gameConnection->isMyConnection(netConnection))
			return gameConnection;
	}

	return nullptr;
}

bool CVCMIServer::isPendingConnection(const std::shared_ptr<GameConnection> & connection) const
{
	return vstd::contains(pendingConnections, connection);
}

bool CVCMIServer::wasStartedByClient() const
{
	return runByClient;
}

void CVCMIServer::run()
{
	networkHandler->run();
}

void CVCMIServer::onTimer()
{
	// we might receive onTimer call after transitioning from GAMEPLAY to LOBBY state, e.g. on game restart
	if (getState() != EServerState::GAMEPLAY)
		return;

	static const auto serverUpdateInterval = std::chrono::milliseconds(100);

	auto timeNow = std::chrono::steady_clock::now();
	auto timePassedBefore = lastTimerUpdateTime - gameplayStartTime;
	auto timePassedNow = timeNow - gameplayStartTime;

	lastTimerUpdateTime = timeNow;

	auto msPassedBefore = std::chrono::duration_cast<std::chrono::milliseconds>(timePassedBefore);
	auto msPassedNow = std::chrono::duration_cast<std::chrono::milliseconds>(timePassedNow);
	auto msDelta = msPassedNow - msPassedBefore;

	if (msDelta.count())
		gh->tick(msDelta.count());
	networkHandler->createTimer(*this, serverUpdateInterval);
}

void CVCMIServer::prepareToRestart()
{
	if(getState() != EServerState::GAMEPLAY)
	{
		assert(0);
		return;
	}

	setState(EServerState::LOBBY);
	if (si->campState)
	{
		assert(si->campState->currentScenario().has_value());
		campaignMap = si->campState->currentScenario().value_or(CampaignScenarioID(0));
		campaignBonus = si->campState->getBonusID(campaignMap).value_or(-1);
	}

	for(auto activeConnection : activeConnections)
		activeConnection->enterLobbyConnectionMode();

	dropOfflinePlayers();

	gh = nullptr;
}

void CVCMIServer::dropOfflinePlayers()
{
	for(auto it = playerNames.begin(); it != playerNames.end();)
	{
		if(it->second.connection != GameConnectionID::INVALID)
		{
			++it;
			continue;
		}

		for(auto & playerInfo : si->playerInfos)
			playerInfo.second.connectedPlayerIDs.erase(it->first);

		it = playerNames.erase(it);
	}

	for(const auto & connection : pendingConnections)
	{
		if(auto networkConnection = connection->getConnection())
			networkConnection->close();
	}
	pendingConnections.clear();
}

bool CVCMIServer::prepareToStartGame()
{
	Load::ProgressAccumulator progressTracking;
	Load::Progress current(1);
	progressTracking.include(current);

	auto progressTrackingThread = std::thread([this, &progressTracking]()
	{
		setThreadName("progressTrackingThread");
		auto currentProgress = std::numeric_limits<Load::Type>::max();

		while(!progressTracking.finished())
		{
			if(progressTracking.get() != currentProgress)
			{
				//FIXME: UNGUARDED MULTITHREADED ACCESS!!!
				currentProgress = progressTracking.get();
				LobbyLoadProgress loadProgress;
				loadProgress.progress = currentProgress;
				announcePack(loadProgress);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		//send final progress
		LobbyLoadProgress loadProgress;
		loadProgress.progress = std::numeric_limits<Load::Type>::max();
		announcePack(loadProgress);
	});

	auto newGH = std::make_shared<CGameHandler>(*this);
	bool started = false;
	try
	{
		switch(si->mode)
		{
		case EStartMode::CAMPAIGN:
			logNetwork->info("Preparing to start new campaign");
			if(si->campState->getStartTime() == 0)
				si->campState->setStartTime(std::time(nullptr));
			si->startTime = si->campState->getStartTime();
			si->saveDirectory = si->campState->getSaveDirectory();
			si->fileURI = mi->fileURI;
			si->campState->setCurrentMap(campaignMap);
			si->campState->setCurrentMapBonus(campaignBonus);
			newGH->init(si.get(), progressTracking);	// may throw
			started = true;
			break;
		case EStartMode::NEW_GAME:
			logNetwork->info("Preparing to start new game");
			si->startTime = std::time(nullptr);
			si->saveDirectory.clear();
			si->fileURI = mi->fileURI;
			newGH->init(si.get(), progressTracking);	// may throw
			started = true;
			break;

		case EStartMode::LOAD_GAME:
			logNetwork->info("Preparing to start loaded game");
			if(loadSavedGame(*newGH, *si))
				started = true;
			break;
		default:
			logNetwork->error("Wrong mode in StartInfo!");
			assert(0);
			break;
		}
	}
	catch(const ModIncompatibility & e)
	{
		logGlobal->error("Failed to launch game: %s", e.what());
		announceMessage(e.getFullErrorMsg());
	}
	catch(const IdentifierResolutionException & e)
	{
		logGlobal->error("Failed to launch game: %s", e.what());
		MetaString errorMsg;
		errorMsg.appendTextID("vcmi.server.errors.campOrMapFile.unknownEntity");
		errorMsg.replaceRawString(e.identifierName);
		announceMessage(errorMsg);
	}
	catch(const std::exception & e)
	{
		logGlobal->error("Failed to launch game: %s", e.what());
		auto str = MetaString::createFromTextID("vcmi.client.errors.invalidMap");
		str.appendRawString(":\n");
		str.replaceRawString(e.what());
		announceMessage(str);
	}
	current.finish();
	progressTrackingThread.join();

	if (!started)
		return false;

	// prevent loading or starting game where human player is not present on map
	// actual bug might be someplace else, from more likely to less likely:
	// a) RMG bug that fails to place player on map
	// b) this is saved game and player picked already eliminated player
	// c) corrupted map with invalid player
	for (const auto & [color, info] : si->playerInfos)
	{
		if (!info.isControlledByHuman())
			continue;
		const auto * ps = newGH->gameInfo().getPlayerState(color, false);
		if (!ps || ps->checkVanquished())
		{
			logGlobal->error("Human player %s has no heroes and no towns! Aborting game start!", color.toString());
			auto str = MetaString::createFromTextID("vcmi.broadcast.failedLoadGame");
			return false;
		}
	}

	gh = std::move(newGH);
	gh->humanTurnStartListener = humanTurnStartListener;
	gh->notificationSecret = notificationSecret;

	if(lobbyProcessor)
		lobbyProcessor->sendGameStarted();

	return true;
}

void CVCMIServer::startGameImmediately()
{
	for(auto activeConnection : activeConnections)
		activeConnection->setCallback(gh->gameInfo());

	for(auto activeConnection : activeConnections)
	{
		auto players = getAllClientPlayers(activeConnection->connectionID);
		std::stringstream sbuffer;
		sbuffer << "Connection " << static_cast<int>(activeConnection->connectionID) << " will handle " << players.size() << " player: ";
		for (PlayerColor color : players)
			sbuffer << color << " ";

		logGlobal->info(sbuffer.str());
	}

	gh->start(si->mode == EStartMode::LOAD_GAME);
	setState(EServerState::GAMEPLAY);
	lastTimerUpdateTime = gameplayStartTime = std::chrono::steady_clock::now();
	onTimer();

	multiplayerWelcomeMessage();

	for(const auto & connection : activeConnections)
		gh->sendNotificationTopics(connection->connectionID);
}

void CVCMIServer::onDisconnected(const std::shared_ptr<INetworkConnection> & connection, const std::string & errorMessage)
{
	logNetwork->error("Network error receiving a pack. Connection has been closed");

	std::shared_ptr<GameConnection> c = findConnection(connection);

	// player may have already disconnected via clientDisconnected call
	if (c)
	{
		LobbyClientDisconnected lcd;
		lcd.clientId = c->connectionID;
		handleReceivedPack(c, lcd);
	}
}

void CVCMIServer::handleReceivedPack(std::shared_ptr<GameConnection> connection, CPackForLobby & pack)
{
	// a connection that has not rejoined the game can only query the server, join or leave
	if(isPendingConnection(connection)
		&& dynamic_cast<LobbyClientConnected *>(&pack) == nullptr
		&& dynamic_cast<LobbyQueryState *>(&pack) == nullptr
		&& dynamic_cast<LobbyClientDisconnected *>(&pack) == nullptr)
	{
		logNetwork->error("Received lobby pack %s from connection that has not rejoined the game", typeid(pack).name());
		return;
	}

	ClientPermissionsCheckerNetPackVisitor checker(*this, connection);
	pack.visit(checker);

	if(checker.getResult())
	{
		ApplyOnServerNetPackVisitor applier(*this, connection);
		pack.visit(applier);
		if (applier.getResult())
			announcePack(pack);
	}
}

void CVCMIServer::announcePack(CPackForLobby & pack)
{
	for(auto activeConnection : activeConnections)
	{
		// FIXME: we need to avoid sending something to client that not yet get answer for LobbyClientConnected
		// Until UUID set we only pass LobbyClientConnected to this client
		//if(c->uuid == uuid && !dynamic_cast<LobbyClientConnected *>(pack.get()))
		//	continue;
		activeConnection->sendPack(pack);
	}

	ApplyOnServerAfterAnnounceNetPackVisitor applier(*this);
	pack.visit(applier);
}

void CVCMIServer::announceMessage(const MetaString & txt)
{
	logNetwork->info("Show message: %s", txt.toString(LIBRARY->staticTexts()));
	LobbyShowMessage cm;
	cm.message = txt;
	announcePack(cm);
}

void CVCMIServer::announceMessage(const std::string & txt)
{
	MetaString str;
	str.appendRawString(txt);
	announceMessage(str);
}

void CVCMIServer::announceTxt(const MetaString & txt, const std::string & playerName)
{
	logNetwork->info("%s says: %s", playerName, txt.toString(LIBRARY->staticTexts()));
	LobbyChatMessage cm;
	cm.playerName = playerName;
	cm.message = txt;
	announcePack(cm);
}

void CVCMIServer::announceTxt(const std::string & txt, const std::string & playerName)
{
	MetaString str;
	str.appendRawString(txt);
	announceTxt(str, playerName);
}

bool CVCMIServer::passHost(GameConnectionID toConnectionId)
{
	for(auto activeConnection : activeConnections)
	{
		if(isClientHost(activeConnection->connectionID))
			continue;
		if(activeConnection->connectionID != toConnectionId)
			continue;

		hostClientId = activeConnection->connectionID;
		announceTxt(boost::str(boost::format("Pass host to connection %d") % static_cast<int>(toConnectionId)));
		return true;
	}
	return false;
}

void CVCMIServer::clientConnected(std::shared_ptr<GameConnection> c, const std::vector<std::string> & names, const std::string & uuid, EStartMode mode)
{
	assert(getState() == EServerState::LOBBY);

	c->connectionID = currentClientId;
	currentClientId = vstd::next(currentClientId, 1);
	c->uuid = uuid;

	if(hostClientId == GameConnectionID::INVALID)
	{
		hostClientId = c->connectionID;
		si->mode = mode;
	}

	logNetwork->info("Connection with client %d established. UUID: %s", static_cast<int>(c->connectionID), c->uuid);

	for(auto & name : names)
	{
		logNetwork->info("Client %d player: %s", static_cast<int>(c->connectionID), name);
		PlayerConnectionID id = currentPlayerId;
		currentPlayerId = vstd::next(currentPlayerId, 1);

		ClientPlayer cp;
		cp.connection = c->connectionID;
		cp.name = name;
		playerNames.try_emplace(id, cp);
		if(!deviceIdOf(uuid).empty())
			playerDevices[id] = deviceIdOf(uuid);
		logNetwork->info("Player joined lobby: name='%s', playerId=%d, connectionId=%d", name, static_cast<int>(id), static_cast<int>(c->connectionID));
		MetaString joinMessage;
		joinMessage.appendTextID("vcmi.lobby.system.playerJoined");
		joinMessage.replaceRawString(name);
		announceTxt(joinMessage);

		//put new player in first slot with AI
		for(auto & elem : si->playerInfos)
		{
			if(elem.second.isControlledByAI() && !elem.second.compOnly)
			{
				setPlayerConnectedId(elem.second, id);
				break;
			}
		}
	}
}

void CVCMIServer::clientDisconnected(std::shared_ptr<GameConnection> connection)
{
	if(isPendingConnection(connection))
	{
		vstd::erase(pendingConnections, connection);
		return;
	}

	assert(vstd::contains(activeConnections, connection));
	logGlobal->trace("Received disconnection request");
	vstd::erase(activeConnections, connection);

	// e.g. a lobby preview, that only queried the server state. Its id is the same as the id of a host that did not connect yet
	if(connection->connectionID == GameConnectionID::INVALID)
	{
		logNetwork->info("Connection that did not join the lobby has been closed");
		return;
	}

	std::vector<PlayerConnectionID> disconnectedPlayerIds;
	for(const auto & playerEntry : playerNames)
	{
		if(playerEntry.second.connection != connection->connectionID)
			continue;

		disconnectedPlayerIds.push_back(playerEntry.first);
		logNetwork->info("Player disconnected from lobby: name='%s', connectionId=%d", playerEntry.second.name, static_cast<int>(connection->connectionID));
		MetaString disconnectMessage;
		disconnectMessage.appendTextID("vcmi.lobby.system.playerDisconnected");
		disconnectMessage.replaceRawString(playerEntry.second.name);
		announceTxt(disconnectMessage);
	}

	if(disconnectedPlayerIds.empty())
		logNetwork->info("Connection %d disconnected from lobby with no mapped player names", static_cast<int>(connection->connectionID));

	const bool hostLeft = hostClientId == connection->connectionID;
	const bool gameContinues = gh && getState() == EServerState::GAMEPLAY && gh->hasHumanPlayerInGame();

	// a server started by a client stops together with it, a dedicated server keeps the game going for the players that left
	if((activeConnections.empty() || hostLeft) && (wasStartedByClient() || !gameContinues))
	{
		setState(EServerState::SHUTDOWN);
		return;
	}

	if(gh && getState() == EServerState::GAMEPLAY)
	{
		gh->handleClientDisconnection(connection->connectionID, disconnectedPlayerIds);

		// the seats stay reserved, so that the players can rejoin the game
		for(const auto & playerId : disconnectedPlayerIds)
			playerNames.at(playerId).connection = GameConnectionID::INVALID;

		if(hostLeft)
			passHostToConnectedClient();
		return;
	}

	for(const auto & playerId : disconnectedPlayerIds)
		playerNames.erase(playerId);

	for(auto & playerInfoEntry : si->playerInfos)
	{
		auto & connectedPlayerIDs = playerInfoEntry.second.connectedPlayerIDs;
		for(const auto & playerId : disconnectedPlayerIds)
			connectedPlayerIDs.erase(playerId);
	}
}

bool CVCMIServer::resumeNewestAutosave()
{
	assert(getState() == EServerState::LOBBY);

	auto * filesystem = CResourceHandler::get("local");
	filesystem->updateFilteredFiles([](const std::string & mount)
	{
		return boost::algorithm::starts_with(mount, "SAVES/");
	});

	std::optional<ResourcePath> newestAutosave;
	std::time_t newestWriteTime = 0;
	for(const auto & resource : filesystem->getFilteredFiles([](const ResourcePath & resource)
		{
			return resource.getType() == EResType::SAVEGAME && SavegamePath::isAutosaveName(resource.getOriginalName());
		}))
	{
		const auto writeTime = filesystem->getLastWriteTime(resource);
		if(!newestAutosave || writeTime > newestWriteTime)
		{
			newestAutosave = resource;
			newestWriteTime = writeTime;
		}
	}

	if(!newestAutosave)
		return false;

	logNetwork->info("Resuming the game from %s", newestAutosave->getOriginalName());

	auto mapInfo = std::make_shared<CMapInfo>();
	mapInfo->saveInit(*newestAutosave);
	mi = mapInfo;
	si = CMemorySerializer::deepCopy(*mapInfo->scenarioOptionsOfSave);
	si->mode = EStartMode::LOAD_GAME;
	si->mapname = mapInfo->fileURI;

	// the seat of every human player waits until the player rejoins
	playerNames.clear();
	for(const auto & [color, playerSettings] : si->playerInfos)
	{
		for(const auto & playerId : playerSettings.connectedPlayerIDs)
		{
			playerNames[playerId] = ClientPlayer{GameConnectionID::INVALID, playerSettings.name};
			if(playerId >= currentPlayerId)
				currentPlayerId = vstd::next(playerId, 1);
		}
	}

	// an id that no client gets, the first player that rejoins becomes the host
	hostClientId = currentClientId;
	currentClientId = vstd::next(currentClientId, 1);

	if(!prepareToStartGame())
		throw std::runtime_error("Failed to resume the game from " + newestAutosave->getOriginalName());

	startGameImmediately();
	return true;
}

bool CVCMIServer::isHostConnected() const
{
	for(const auto & activeConnection : activeConnections)
		if(isClientHost(activeConnection->connectionID))
			return true;
	return false;
}

void CVCMIServer::announceHostChange()
{
	for(const auto & [playerId, player] : playerNames)
	{
		if(player.connection != hostClientId)
			continue;

		MetaString message;
		message.appendTextID("vcmi.lobby.system.hostChanged");
		message.replaceRawString(player.name);
		announceTxt(message);
		return;
	}
}

void CVCMIServer::passHostToConnectedClient()
{
	// a connection that never sent LobbyClientConnected has no id and can not control players
	const auto newHost = std::ranges::find_if(activeConnections, [](const auto & connection)
	{
		return connection->connectionID != GameConnectionID::INVALID;
	});

	if(newHost == activeConnections.end())
	{
		// hostClientId keeps the connection that left, so that no other connection is mistaken for the host
		logNetwork->info("No client is connected, the game waits until a player rejoins it");
		return;
	}

	hostClientId = (*newHost)->connectionID;
	logNetwork->info("Connection %d is the new host and controls the AI players", static_cast<int>(hostClientId));
	announceHostChange();

	// the client has to install the interfaces of the AI players before they are asked anything
	updateAndPropagateLobbyState();
	gh->handleHostChange(hostClientId);
}

std::vector<PlayerConnectionID> CVCMIServer::chooseRejoinSeats(const std::vector<RejoinCandidate> & seats, const std::vector<std::string> & names, const std::string & deviceId)
{
	std::vector<PlayerConnectionID> result;

	for(const auto & name : names)
	{
		std::vector<const RejoinCandidate *> candidates;
		for(const auto & seat : seats)
		{
			if(seat.name == name && !vstd::contains(result, seat.playerId))
				candidates.push_back(&seat);
		}

		auto isOfDevice = [&deviceId](const RejoinCandidate * seat)
		{
			return !deviceId.empty() && seat->deviceId == deviceId;
		};

		const RejoinCandidate * chosen = nullptr;
		for(const auto * seat : candidates)
		{
			if(!chosen && !seat->online && isOfDevice(seat))
				chosen = seat;
		}

		for(const auto * seat : candidates)
		{
			if(!chosen && seat->online && seat->replaceable && isOfDevice(seat))
				chosen = seat;
		}

		// e.g. after a restart of the server
		for(const auto * seat : candidates)
		{
			if(!chosen && !seat->online && seat->deviceId.empty())
				chosen = seat;
		}

		// e.g. after the app was reinstalled, only when the name identifies the seat
		if(!chosen && candidates.size() == 1 && !candidates.front()->online)
			chosen = candidates.front();

		if(chosen)
			result.push_back(chosen->playerId);
	}
	return result;
}

CVCMIServer::RejoinSeats CVCMIServer::findPlayersToRejoin(const std::vector<std::string> & names, const std::string & deviceId) const
{
	std::vector<RejoinCandidate> candidates;

	for(const auto & [playerId, player] : playerNames)
	{
		bool hasColorInGame = false;
		for(const auto & playerInfo : si->playerInfos)
		{
			if(!vstd::contains(playerInfo.second.connectedPlayerIDs, playerId))
				continue;

			const auto * playerState = gh->gameInfo().getPlayerState(playerInfo.first, false);
			if(playerState && playerState->status == EPlayerStatus::INGAME)
				hasColorInGame = true;
		}

		if(!hasColorInGame)
			continue;

		RejoinCandidate candidate;
		candidate.playerId = playerId;
		candidate.name = player.name;
		candidate.online = player.connection != GameConnectionID::INVALID;
		// a server started by a client stops when its host leaves
		candidate.replaceable = candidate.online && !(wasStartedByClient() && isClientHost(player.connection));
		if(vstd::contains(playerDevices, playerId))
			candidate.deviceId = playerDevices.at(playerId);
		candidates.push_back(candidate);
	}

	RejoinSeats result;
	result.players = chooseRejoinSeats(candidates, names, deviceId);

	for(const auto & playerId : result.players)
	{
		const auto connectionID = playerNames.at(playerId).connection;
		if(connectionID == GameConnectionID::INVALID)
			continue;

		const auto stale = std::ranges::find_if(activeConnections, [connectionID](const auto & activeConnection)
		{
			return activeConnection->connectionID == connectionID;
		});

		if(stale == activeConnections.end())
			throw std::runtime_error("Player " + std::to_string(static_cast<int>(playerId)) + " is at a connection that does not exist");

		if(!vstd::contains(result.staleConnections, *stale))
			result.staleConnections.push_back(*stale);
	}
	return result;
}

void CVCMIServer::clientRejoining(std::shared_ptr<GameConnection> connection, LobbyClientConnected & pack)
{
	assert(getState() == EServerState::GAMEPLAY);
	assert(isPendingConnection(connection));

	connection->connectionID = currentClientId;
	currentClientId = vstd::next(currentClientId, 1);
	connection->uuid = pack.uuid;

	pack.clientId = connection->connectionID;
	pack.mode = si->mode;
	pack.hostClientId = hostClientId;
	connection->sendPack(pack);

	const auto seats = findPlayersToRejoin(pack.names, deviceIdOf(pack.uuid));
	const auto & rejoinedPlayerIds = seats.players;

	// the gamestate is sent as written by this server, an older client can not read it
	LobbyRejoinGame rejoinGame;
	if(rejoinedPlayerIds.empty() || pack.version < ESerializationVersion::CURRENT || !gh->fillRejoinGamestate(rejoinGame))
	{
		logNetwork->info("Connection %d can not rejoin the game", static_cast<int>(connection->connectionID));
		LobbyShowMessage message;
		message.message.appendTextID("vcmi.lobby.system.rejoinRefused");
		connection->sendPack(message);
		return;
	}

	for(const auto & stale : seats.staleConnections)
	{
		logNetwork->info("Connection %d is replaced by connection %d of the same device", static_cast<int>(stale->connectionID), static_cast<int>(connection->connectionID));
		if(auto networkConnection = stale->getConnection())
			networkConnection->close();
		clientDisconnected(stale);
	}

	const bool hostWasConnected = isHostConnected();

	vstd::erase(pendingConnections, connection);
	activeConnections.push_back(connection);
	connection->setCallback(gh->gameInfo());

	for(const auto & playerId : rejoinedPlayerIds)
	{
		auto & player = playerNames.at(playerId);
		player.connection = connection->connectionID;
		if(!deviceIdOf(pack.uuid).empty())
			playerDevices[playerId] = deviceIdOf(pack.uuid);
		logNetwork->info("Player rejoined the game: name='%s', playerId=%d, connectionId=%d", player.name, static_cast<int>(playerId), static_cast<int>(connection->connectionID));

		MetaString joinMessage;
		joinMessage.appendTextID("vcmi.lobby.system.playerJoined");
		joinMessage.replaceRawString(player.name);
		announceTxt(joinMessage);
	}

	// nobody was connected, the first player that rejoins takes over the AI players
	if(!hostWasConnected)
	{
		hostClientId = connection->connectionID;
		logNetwork->info("Connection %d is the new host and controls the AI players", static_cast<int>(hostClientId));
		announceHostChange();
	}

	updateAndPropagateLobbyState();

	connection->sendPack(rejoinGame);

	gh->handleClientRejoin(connection->connectionID);
}

void CVCMIServer::setPlayerConnectedId(PlayerSettings & pset, PlayerConnectionID player) const
{
	if(vstd::contains(playerNames, player))
		pset.name = playerNames.find(player)->second.name;
	else
		pset.name = LIBRARY->generaltexth->allTexts[468]; //Computer

	logGlobal->debug("Player color %d will be controlled from connection %d", pset.color, static_cast<int>(player));
	pset.connectedPlayerIDs.clear();
	if(player != PlayerConnectionID::PLAYER_AI)
		pset.connectedPlayerIDs.insert(player);
}

void CVCMIServer::updateStartInfoOnMapChange(std::shared_ptr<CMapInfo> mapInfo, std::shared_ptr<CMapGenOptions> mapGenOpts)
{
	mi = mapInfo;
	if(!mi)
		return;

	auto namesIt = playerNames.cbegin();
	si->playerInfos.clear();
	if(mi->scenarioOptionsOfSave)
	{
		si = CMemorySerializer::deepCopy(*mi->scenarioOptionsOfSave);
		si->mode = EStartMode::LOAD_GAME;
		if(si->campState)
			campaignMap = si->campState->currentScenario().value();

		for(auto & ps : si->playerInfos)
		{
			if(!ps.second.compOnly && ps.second.connectedPlayerIDs.size() && namesIt != playerNames.cend())
			{
				setPlayerConnectedId(ps.second, namesIt++->first);
			}
			else
			{
				setPlayerConnectedId(ps.second, PlayerConnectionID::PLAYER_AI);
			}
		}
	}
	else if(si->mode == EStartMode::NEW_GAME || si->mode == EStartMode::CAMPAIGN)
	{
		if(mi->campaign)
			return;

		for(int i = 0; i < mi->mapHeader->players.size(); i++)
		{
			const PlayerInfo & pinfo = mi->mapHeader->players[i];

			//neither computer nor human can play - no player
			if(!(pinfo.canHumanPlay || pinfo.canComputerPlay))
				continue;

			PlayerSettings & pset = si->playerInfos[PlayerColor(i)];
			pset.color = PlayerColor(i);
			if(pinfo.canHumanPlay && namesIt != playerNames.cend())
			{
				setPlayerConnectedId(pset, namesIt++->first);
			}
			else
			{
				setPlayerConnectedId(pset, PlayerConnectionID::PLAYER_AI);
				if(!pinfo.canHumanPlay)
				{
					pset.compOnly = true;
				}
			}

			pset.castle = pinfo.defaultCastle();
			pset.hero = pinfo.defaultHero();

			if(pset.hero != HeroTypeID::RANDOM && pinfo.hasCustomMainHero())
			{
				pset.hero = pinfo.mainCustomHeroId;
				pset.heroNameTextId = pinfo.mainCustomHeroNameTextId;
				pset.heroPortrait = pinfo.mainCustomHeroPortrait;
			}
		}

		if(mi->isRandomMap && mapGenOpts)
			si->mapGenOptions = std::shared_ptr<CMapGenOptions>(mapGenOpts);
		else
			si->mapGenOptions.reset();
	}

	if (lobbyProcessor)
	{
		std::string roomDescription;

		if (si->mapGenOptions)
		{
			if (si->mapGenOptions->getMapTemplate())
				roomDescription = si->mapGenOptions->getMapTemplate()->getName();
			// else - no template selected.
			// TODO: handle this somehow?
		}
		else
		{
			// lobby metadata is built server-side, so it resolves the map's own texts
			CompositeTranslator translator;
			translator.install(mi->mapHeader->texts);
			roomDescription = mi->getNameTranslated(&translator);
		}

		lobbyProcessor->sendChangeRoomDescription(roomDescription);
	}

	si->mapname = mi->fileURI;
}

void CVCMIServer::updateAndPropagateLobbyState()
{
	// Update player settings for RMG
	// TODO: find appropriate location for this code
	if(si->mapGenOptions && si->mode == EStartMode::NEW_GAME)
	{
		for(const auto & psetPair : si->playerInfos)
		{
			const auto & pset = psetPair.second;
			si->mapGenOptions->setStartingTownForPlayer(pset.color, pset.castle);
			si->mapGenOptions->setStartingHeroForPlayer(pset.color, pset.hero);
			if(pset.isControlledByHuman())
			{
				si->mapGenOptions->setPlayerTypeForStandardPlayer(pset.color, EPlayerType::HUMAN);
			}
			else
			{
				si->mapGenOptions->setPlayerTypeForStandardPlayer(pset.color, EPlayerType::AI);
			}
		}
	}

	LobbyUpdateState lus;
	lus.state = *static_cast<LobbyState*>(this);
	announcePack(lus);
}

void CVCMIServer::setPlayer(PlayerColor clickedColor)
{
	struct PlayerToRestore
	{
		PlayerColor color;
		PlayerConnectionID id;
		void reset() { id = PlayerConnectionID::INVALID; color = PlayerColor::CANNOT_DETERMINE; }
		PlayerToRestore(){ reset(); }
	};

	PlayerToRestore playerToRestore;
	PlayerSettings & clicked = si->playerInfos[clickedColor];

	//identify clicked player
	PlayerConnectionID clickedNameID = PlayerConnectionID::PLAYER_AI;
	if(clicked.isControlledByHuman())
		clickedNameID = *(clicked.connectedPlayerIDs.begin()); //if not AI - set appropriate ID

	if(clickedNameID > PlayerConnectionID::PLAYER_AI && playerToRestore.id == clickedNameID) //player to restore is about to being replaced -> put him back to the old place
	{
		PlayerSettings & restPos = si->playerInfos[playerToRestore.color];
		setPlayerConnectedId(restPos, playerToRestore.id);
		playerToRestore.reset();
	}

	PlayerConnectionID newPlayer; //which player will take clicked position

	//who will be put here?
	if(clickedNameID == PlayerConnectionID::PLAYER_AI) //AI player clicked -> if possible replace computer with unallocated player
	{
		newPlayer = getIdOfFirstUnallocatedPlayer();
		if(newPlayer == PlayerConnectionID::PLAYER_AI) //no "free" player -> get just first one
			newPlayer = playerNames.begin()->first;
	}
	else //human clicked -> take next
	{
		auto i = playerNames.find(clickedNameID); //clicked one
		i++; //player AFTER clicked one

		if(i != playerNames.end())
			newPlayer = i->first;
		else
			newPlayer = PlayerConnectionID::PLAYER_AI; //AI if we scrolled through all players
	}

	setPlayerConnectedId(clicked, newPlayer); //put player

	//if that player was somewhere else, we need to replace him with computer
	if(newPlayer != PlayerConnectionID::PLAYER_AI) //not AI
	{
		for(auto i = si->playerInfos.begin(); i != si->playerInfos.end(); i++)
		{
			PlayerConnectionID curNameID = *(i->second.connectedPlayerIDs.begin());
			if(i->first != clickedColor && curNameID == newPlayer)
			{
				assert(i->second.connectedPlayerIDs.size());
				playerToRestore.color = i->first;
				playerToRestore.id = newPlayer;
				setPlayerConnectedId(i->second, PlayerConnectionID::PLAYER_AI); //set computer
				break;
			}
		}
	}
}

void CVCMIServer::setPlayerName(PlayerColor color, const std::string & name)
{
	if(color == PlayerColor::CANNOT_DETERMINE)
		return;

	PlayerSettings & player = si->playerInfos.at(color);

	if(!player.isControlledByHuman())
		return;

	if(player.connectedPlayerIDs.empty())
		return;

	PlayerConnectionID nameID = *(player.connectedPlayerIDs.begin()); //if not AI - set appropriate ID

	playerNames[nameID].name = name;
	setPlayerConnectedId(player, nameID);
}

void CVCMIServer::setPlayerHandicap(PlayerColor color, Handicap handicap)
{
	if(color == PlayerColor::CANNOT_DETERMINE)
		return;

	si->playerInfos[color].handicap = handicap;

	int humanPlayer = 0;
	for (const auto & pi : si->playerInfos)
		if(pi.second.isControlledByHuman())
			humanPlayer++;

	if(humanPlayer < 2) // Singleplayer
		return;

	MetaString str;
	str.appendTextID("vcmi.lobby.handicap");
	str.appendRawString(" ");
	str.appendName(color);
	str.appendRawString(":");

	if(handicap.startBonus.empty() && handicap.percentIncome == 100 && handicap.percentGrowth == 100)
	{
		str.appendRawString(" ");
		str.appendTextID("core.genrltxt.523");
		announceTxt(str);
		return;
	}

	for(auto & res : LIBRARY->resourceTypeHandler->getAllObjects())
		if(handicap.startBonus[res] != 0)
		{
			str.appendRawString(" ");
			str.appendName(res);
			str.appendRawString(":");
			str.appendRawString(std::to_string(handicap.startBonus[res]));
		}
	if(handicap.percentIncome != 100)
	{
		str.appendRawString(" ");
		str.appendTextID("core.jktext.32");
		str.appendRawString(":");
		str.appendRawString(std::to_string(handicap.percentIncome) + "%");
	}
	if(handicap.percentGrowth != 100)
	{
		str.appendRawString(" ");
		str.appendTextID("core.genrltxt.194");
		str.appendRawString(":");
		str.appendRawString(std::to_string(handicap.percentGrowth) + "%");
	}
	announceTxt(str);
}

void CVCMIServer::optionNextCastle(PlayerColor player, int dir)
{
	PlayerSettings & s = si->playerInfos[player];
	FactionID & cur = s.castle;
	auto & allowed = getPlayerInfo(player).allowedFactions;
	const bool allowRandomTown = getPlayerInfo(player).isFactionRandom;

	if(cur == FactionID::NONE) //no change
		return;

	if(cur == FactionID::RANDOM) //first/last available
	{
		if(dir > 0)
			cur = *allowed.begin(); //id of first town
		else
			cur = *allowed.rbegin(); //id of last town

	}
	else // next/previous available
	{
		if((cur == *allowed.begin() && dir < 0) || (cur == *allowed.rbegin() && dir > 0))
		{
			if(allowRandomTown)
			{
				cur = FactionID::RANDOM;
			}
			else
			{
				if(dir > 0)
					cur = *allowed.begin();
				else
					cur = *allowed.rbegin();
			}
		}
		else
		{
			assert(dir >= -1 && dir <= 1); //othervice std::advance may go out of range
			auto iter = allowed.find(cur);
			std::advance(iter, dir);
			cur = *iter;
		}
	}

	if(s.hero.isValid() && !getPlayerInfo(player).hasCustomMainHero()) // remove hero unless it set to fixed one in map editor
	{
		s.hero = HeroTypeID::RANDOM;
	}
	if(!cur.isValid() && s.bonus == PlayerStartingBonus::RESOURCE)
		s.bonus = PlayerStartingBonus::RANDOM;
}

void CVCMIServer::optionSetCastle(PlayerColor player, FactionID id)
{
	PlayerSettings & s = si->playerInfos[player];
	FactionID & cur = s.castle;
	auto & allowed = getPlayerInfo(player).allowedFactions;

	if(cur == FactionID::NONE) //no change
		return;

	if(allowed.find(id) == allowed.end() && id != FactionID::RANDOM) // valid id
		return;

	cur = static_cast<FactionID>(id);

	if(s.hero.isValid() && !getPlayerInfo(player).hasCustomMainHero()) // remove hero unless it set to fixed one in map editor
	{
		s.hero = HeroTypeID::RANDOM;
	}
	if(!cur.isValid() && s.bonus == PlayerStartingBonus::RESOURCE)
		s.bonus = PlayerStartingBonus::RANDOM;
}

void CVCMIServer::setCampaignMap(CampaignScenarioID mapId)
{
	campaignMap = mapId;
	si->difficulty = si->campState->scenario(mapId).difficulty;
	campaignBonus = -1;
	updateStartInfoOnMapChange(si->campState->getMapInfo(mapId));
}

void CVCMIServer::setCampaignBonus(int bonusId)
{
	campaignBonus = bonusId;

	const CampaignScenario & scenario = si->campState->scenario(campaignMap);
	const CampaignBonus & bonus = scenario.travelOptions.bonusesToChoose.at(bonusId);
	if(bonus.getType() == CampaignBonusType::HERO || bonus.getType() == CampaignBonusType::HEROES_FROM_PREVIOUS_SCENARIO)
	{
		PlayerColor startingPlayer = bonus.getType() == CampaignBonusType::HERO ?
			bonus.getValue<CampaignBonusStartingHero>().startingPlayer :
			bonus.getValue<CampaignBonusHeroesFromScenario>().startingPlayer;

		for(auto & elem : si->playerInfos)
		{
			if(elem.first == startingPlayer)
				setPlayerConnectedId(elem.second, PlayerConnectionID::FIRST_HUMAN);
			else
				setPlayerConnectedId(elem.second, PlayerConnectionID::PLAYER_AI);
		}
	}
}

void CVCMIServer::optionNextHero(PlayerColor player, int dir)
{
	PlayerSettings & s = si->playerInfos[player];
	if(!s.castle.isValid() || s.hero == HeroTypeID::NONE)
		return;

	if(s.hero == HeroTypeID::RANDOM) // first/last available
	{
		if (dir > 0)
			s.hero = nextAllowedHero(player, HeroTypeID(-1), dir);
		else
			s.hero = nextAllowedHero(player, HeroTypeID(LIBRARY->heroh->size()), dir);
	}
	else
	{
		s.hero = nextAllowedHero(player, s.hero, dir);
	}
}

void CVCMIServer::optionSetHero(PlayerColor player, HeroTypeID id)
{
	PlayerSettings & s = si->playerInfos[player];
	if(!s.castle.isValid() || s.hero == HeroTypeID::NONE)
		return;

	if(id == HeroTypeID::RANDOM)
	{
		s.hero = HeroTypeID::RANDOM;
	}
	if(canUseThisHero(player, id))
		s.hero = static_cast<HeroTypeID>(id);
}

HeroTypeID CVCMIServer::nextAllowedHero(PlayerColor player, HeroTypeID initial, int direction)
{
	HeroTypeID first(initial.getNum() + direction);

	if(direction > 0)
	{
		for (auto i = first; i.getNum() < LIBRARY->heroh->size(); ++i)
			if(canUseThisHero(player, i))
				return i;
	}
	else
	{
		for (auto i = first; i.getNum() >= 0; --i)
			if(canUseThisHero(player, i))
				return i;
	}
	return HeroTypeID::RANDOM;
}

void CVCMIServer::optionNextBonus(PlayerColor player, int dir)
{
	PlayerSettings & s = si->playerInfos[player];
	s.bonus = static_cast<PlayerStartingBonus>(static_cast<int>(s.bonus) + dir);

	if(s.hero == HeroTypeID::NONE &&
		getPlayerInfo(player).heroesNames.empty() &&
		s.bonus == PlayerStartingBonus::ARTIFACT) //no hero - can't be artifact
	{
		if(dir < 0)
			s.bonus = PlayerStartingBonus::RANDOM;
		else
			s.bonus = PlayerStartingBonus::GOLD;
	}

	if(s.bonus > PlayerStartingBonus::RESOURCE)
		s.bonus = PlayerStartingBonus::RANDOM;
	if(s.bonus < PlayerStartingBonus::RANDOM)
		s.bonus = PlayerStartingBonus::RESOURCE;

	if(s.castle == FactionID::RANDOM && s.bonus == PlayerStartingBonus::RESOURCE) //random castle - can't be resource
	{
		if(dir < 0)
			s.bonus = PlayerStartingBonus::GOLD;
		else
			s.bonus = PlayerStartingBonus::RANDOM;
	}
}

void CVCMIServer::optionSetBonus(PlayerColor player, PlayerStartingBonus id)
{
	PlayerSettings & s = si->playerInfos[player];

	if(s.hero == HeroTypeID::NONE &&
		!getPlayerInfo(player).heroesNames.size() &&
		id == PlayerStartingBonus::ARTIFACT) //no hero - can't be artifact
			return;

	if(id > PlayerStartingBonus::RESOURCE)
		return;
	if(id < PlayerStartingBonus::RANDOM)
		return;

	if(s.castle == FactionID::RANDOM && id == PlayerStartingBonus::RESOURCE) //random castle - can't be resource
		return;

	s.bonus = id;
}

bool CVCMIServer::canUseThisHero(PlayerColor player, HeroTypeID ID)
{
	if (!ID.hasValue())
		return false;

	if (ID.getNum() >= LIBRARY->heroh->size())
		return false;

	if (si->playerInfos[player].castle != ID.toHeroType()->heroClass->faction)
		return false;

	if (vstd::contains(getUsedHeroes(), ID))
		return false;

	if (!mi->mapHeader->allowedHeroes.count(ID))
		return false;

	for (const auto & disposedHero : mi->mapHeader->disposedHeroes)
		if (disposedHero.heroId == ID && !disposedHero.players.count(player))
			return false;

	return true;
}

std::vector<HeroTypeID> CVCMIServer::getUsedHeroes()
{
	std::vector<HeroTypeID> heroIds;
	for(const auto & p : si->playerInfos)
	{
		const auto & heroes = getPlayerInfo(p.first).heroesNames;
		for(const auto & hero : heroes)
			if(hero.heroId.hasValue())
				heroIds.push_back(hero.heroId);

		if(p.second.hero != HeroTypeID::RANDOM)
			heroIds.push_back(p.second.hero);
	}
	return heroIds;
}

PlayerConnectionID CVCMIServer::getIdOfFirstUnallocatedPlayer() const
{
	for(auto i = playerNames.cbegin(); i != playerNames.cend(); i++)
	{
		if(!si->getPlayersSettings(i->first))
			return i->first;
	}
	return PlayerConnectionID::PLAYER_AI;
}

void CVCMIServer::multiplayerWelcomeMessage()
{
	int humanPlayer = 0;
	for (const auto & pi : si->playerInfos)
		if(pi.second.isControlledByHuman())
			humanPlayer++;

	if(humanPlayer < 2 || mi->mapHeader->battleOnly) // Singleplayer or Battle only mode
		return;

	gh->playerMessages->broadcastSystemMessage(MetaString::createFromTextID("vcmi.broadcast.command"));

	for (const auto & pi : si->playerInfos)
		if(!pi.second.handicap.startBonus.empty() || pi.second.handicap.percentIncome != 100 || pi.second.handicap.percentGrowth != 100)
		{
			MetaString str;
			str.appendTextID("vcmi.lobby.handicap");
			str.appendRawString(" ");
			str.appendName(pi.first);
			str.appendRawString(":");
			for(auto & res : LIBRARY->resourceTypeHandler->getAllObjects())
				if(pi.second.handicap.startBonus[res] != 0)
				{
					str.appendRawString(" ");
					str.appendName(res);
					str.appendRawString(":");
					str.appendRawString(std::to_string(pi.second.handicap.startBonus[res]));
				}
			if(pi.second.handicap.percentIncome != 100)
			{
				str.appendRawString(" ");
				str.appendTextID("core.jktext.32");
				str.appendRawString(":");
				str.appendRawString(std::to_string(pi.second.handicap.percentIncome) + "%");
			}
			if(pi.second.handicap.percentGrowth != 100)
			{
				str.appendRawString(" ");
				str.appendTextID("core.genrltxt.194");
				str.appendRawString(":");
				str.appendRawString(std::to_string(pi.second.handicap.percentGrowth) + "%");
			}
			gh->playerMessages->broadcastSystemMessage(str);
		}

	std::vector<std::string> optionIds;
	if(si->extraOptionsInfo.cheatsAllowed)
		optionIds.emplace_back("vcmi.optionsTab.cheatAllowed.hover");
	if(si->extraOptionsInfo.unlimitedReplay)
		optionIds.emplace_back("vcmi.optionsTab.unlimitedReplay.hover");

	if(!optionIds.size()) // No settings to publish
		return;

	MetaString str;
	str.appendTextID("vcmi.optionsTab.extraOptions.hover");
	str.appendRawString(": ");
	for(int i = 0; i < optionIds.size(); i++)
	{
		str.appendTextID(optionIds[i]);
		if(i < optionIds.size() - 1)
			str.appendRawString(", ");
	}

	gh->playerMessages->broadcastSystemMessage(str);
}

INetworkHandler & CVCMIServer::getNetworkHandler()
{
	return *networkHandler;
}

INetworkServer & CVCMIServer::getNetworkServer()
{
	return *networkServer;
}

bool CVCMIServer::loadSavedGame(CGameHandler & handler, const StartInfo & info)
{
	try
	{
		handler.load(info);
	}
	catch(const ModIncompatibility & e)
	{
		logGlobal->error("Failed to load game: %s", e.what());
		announceMessage(e.getFullErrorMsg());
		return false;
	}
	catch(const IdentifierResolutionException & e)
	{
		logGlobal->error("Failed to load game: %s", e.what());
		MetaString errorMsg;
		errorMsg.appendTextID("vcmi.server.errors.saveFile.unknownEntity");
		errorMsg.replaceRawString(e.identifierName);
		announceMessage(errorMsg);
		return false;
	}

	catch(const std::exception & e)
	{
		logGlobal->error("Failed to load game: %s", e.what());
		auto str = MetaString::createFromTextID("vcmi.broadcast.failedLoadGame");
		str.appendRawString(": ");
		str.appendRawString(e.what());
		announceMessage(str);
		return false;
	}
	return true;
}

bool CVCMIServer::isPlayerHost(const PlayerColor & color) const
{
	return LobbyInfo::isPlayerHost(color);
}

bool CVCMIServer::hasPlayerAt(PlayerColor player, GameConnectionID connectionID) const
{
	return vstd::contains(getAllClientPlayers(connectionID), player);
}

bool CVCMIServer::hasBothPlayersAtSameConnection(PlayerColor left, PlayerColor right) const
{
	for (const auto & c : activeConnections)
		if (hasPlayerAt(left, c->connectionID) && hasPlayerAt(right, c->connectionID))
			return true;

	return false;
}

bool CVCMIServer::isPlayerConnected(PlayerColor player) const
{
	for (const auto & c : activeConnections)
		if (hasPlayerAt(player, c->connectionID))
			return true;

	return false;
}

void CVCMIServer::applyPack(CPackForClient & pack)
{
	logNetwork->trace("\tSending to all clients: %s", typeid(pack).name());
	for (const auto & c : activeConnections)
		c->sendPack(pack);
	gh->gs->apply(pack);
	logNetwork->trace("\tApplied on gameState(): %s", typeid(pack).name());
}

void CVCMIServer::sendPack(CPackForClient & pack, GameConnectionID connectionID)
{
	for (const auto & c : activeConnections)
		if (c->connectionID == connectionID)
			c->sendPack(pack);
}
