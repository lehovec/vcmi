/*
 * CVCMIServer.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "HumanTurnStart.h"
#include "IGameServer.h"

#include "../lib/network/NetworkInterface.h"
#include "../lib/StartInfo.h"

class CMapInfo;

struct CPackForLobby;
struct LobbyClientConnected;

class GameConnection;
struct StartInfo;
struct LobbyInfo;
struct PlayerSettings;
class PlayerColor;
class MetaString;

class CGameHandler;
class CBaseForServerApply;
class CBaseForGHApply;
class GlobalLobbyProcessor;

class CVCMIServer : public LobbyInfo, public INetworkServerListener, public INetworkTimerListener, public IServerDiscoveryAnnouncer, public IGameServer
{
	std::chrono::steady_clock::time_point gameplayStartTime;
	std::chrono::steady_clock::time_point lastTimerUpdateTime;

	std::unique_ptr<INetworkHandler> networkHandler;
	/// Network server instance that receives and processes incoming connections on active socket
	std::unique_ptr<INetworkServer> networkServer;

	/// Handles connection with global lobby. Must be constructed and destroyed after network handler
	std::unique_ptr<GlobalLobbyProcessor> lobbyProcessor;

	EServerState state = EServerState::LOBBY;

	std::shared_ptr<GameConnection> findConnection(const std::shared_ptr<INetworkConnection> &);

	/// Seats that a rejoining client takes, and the connections that hold some of them although the client left
	struct RejoinSeats
	{
		std::vector<PlayerConnectionID> players;
		std::vector<std::shared_ptr<GameConnection>> staleConnections;
	};

	/// Seats with these names that still have a color in game, see chooseRejoinSeats
	RejoinSeats findPlayersToRejoin(const std::vector<std::string> & names, const std::string & deviceId) const;
	/// Device each player joined from, a player of an older client or of a restarted server has none
	std::map<PlayerConnectionID, std::string> playerDevices;
	/// Removes players that left during gameplay, their seats are no longer reserved
	void dropOfflinePlayers();

	bool isHostConnected() const;
	void announceHostChange();
	/// Makes another connected client the host after the host left a game on a dedicated server
	void passHostToConnectedClient();

	GameConnectionID currentClientId;
	PlayerConnectionID currentPlayerId;
	uint16_t port;
	bool runByClient;

	std::shared_ptr<IServerDiscoveryListener> discoveryListener;

	bool loadSavedGame(CGameHandler & handler, const StartInfo & info);
public:
	/// A seat in game that a rejoining client may take
	struct RejoinCandidate
	{
		PlayerConnectionID playerId;
		std::string name;
		std::string deviceId; ///< empty if the device is not known
		bool online = false; ///< a connection holds the seat
		bool replaceable = false; ///< an online seat whose connection may be closed for a new one of the same device
	};

	/// Seats that a client of this device takes for its names. A seat of the device is preferred, also an online one,
	/// because a connection that the client left stays open until a network timeout. With several seats of one name,
	/// a seat of another known device is never taken
	static std::vector<PlayerConnectionID> chooseRejoinSeats(const std::vector<RejoinCandidate> & seats, const std::vector<std::string> & names, const std::string & deviceId);

	uint16_t getPort() const override
	{
		return port;
	}

	bool isInLobby() const override;

	// IGameServer impl
	void setState(EServerState value) override;
	EServerState getState() const override;
	bool isPlayerHost(const PlayerColor & color) const override;
	bool hasPlayerAt(PlayerColor player, GameConnectionID connectionID) const override;
	bool hasBothPlayersAtSameConnection(PlayerColor left, PlayerColor right) const override;
	bool isPlayerConnected(PlayerColor player) const override;
	void applyPack(CPackForClient & pack) override;
	void sendPack(CPackForClient & pack, GameConnectionID connectionID) override;

	/// List of all active connections
	std::vector<std::shared_ptr<GameConnection>> activeConnections;
	/// Connections opened during gameplay, that have not rejoined the game
	std::vector<std::shared_ptr<GameConnection>> pendingConnections;

	uint16_t prepare(bool connectToLobby, bool listenForConnections);

	// INetworkListener impl
	void onDisconnected(const std::shared_ptr<INetworkConnection> & connection, const std::string & errorMessage) override;
	void onPacketReceived(const std::shared_ptr<INetworkConnection> & connection, const std::vector<std::byte> & message) override;
	void onNewConnection(const std::shared_ptr<INetworkConnection> &) override;
	void onTimer() override;

	std::shared_ptr<CGameHandler> gh;
	/// Passed to the game handler of every game that this server starts
	HumanTurnStartListener humanTurnStartListener;
	/// Passed to the game handler of every game, see CGameHandler::notificationSecret
	std::string notificationSecret;

	CVCMIServer(uint16_t port, bool runByClient);
	~CVCMIServer();

	void run();

	bool wasStartedByClient() const override;
	/// Loads the newest autosave, every human player has to rejoin it. Returns false if there is no autosave.
	bool resumeNewestAutosave();
	/// Connection that was opened during gameplay and has not rejoined the game
	bool isPendingConnection(const std::shared_ptr<GameConnection> & connection) const;
	bool prepareToStartGame();
	void prepareToRestart();
	void startGameImmediately();
	uint16_t startAcceptingIncomingConnections(bool listenForConnections);

	void threadHandleClient(std::shared_ptr<GameConnection> c);

	void announcePack(CPackForLobby & pack);
	bool passHost(GameConnectionID toConnectionId);

	void announceTxt(const MetaString & txt, const std::string & playerName = "System");
	void announceTxt(const std::string & txt, const std::string & playerName = "System");

	void setPlayerConnectedId(PlayerSettings & pset, PlayerConnectionID player) const;
	void updateStartInfoOnMapChange(std::shared_ptr<CMapInfo> mapInfo, std::shared_ptr<CMapGenOptions> mapGenOpt = {});

	void clientConnected(std::shared_ptr<GameConnection> c, const std::vector<std::string> & names, const std::string & uuid, EStartMode mode);
	void clientDisconnected(std::shared_ptr<GameConnection> c);
	/// Gives the seats of offline players named in the pack to a connection opened during gameplay
	void clientRejoining(std::shared_ptr<GameConnection> c, LobbyClientConnected & pack);

	void announceMessage(const MetaString & txt);
	void announceMessage(const std::string & txt);

	void handleReceivedPack(std::shared_ptr<GameConnection> connection, CPackForLobby & pack);

	void updateAndPropagateLobbyState();

	INetworkHandler & getNetworkHandler();
	INetworkServer & getNetworkServer();

	// Work with LobbyInfo
	void setPlayer(PlayerColor clickedColor);
	void setPlayerName(PlayerColor player, const std::string & name);
	void setPlayerHandicap(PlayerColor player, Handicap handicap);
	void optionNextHero(PlayerColor player, int dir); //dir == -1 or +1
	void optionSetHero(PlayerColor player, HeroTypeID id);
	HeroTypeID nextAllowedHero(PlayerColor player, HeroTypeID id, int direction);
	bool canUseThisHero(PlayerColor player, HeroTypeID ID);
	std::vector<HeroTypeID> getUsedHeroes();
	void optionNextBonus(PlayerColor player, int dir); //dir == -1 or +1
	void optionSetBonus(PlayerColor player, PlayerStartingBonus id);
	void optionNextCastle(PlayerColor player, int dir); //dir == -1 or +
	void optionSetCastle(PlayerColor player, FactionID id);

	// Campaigns
	void setCampaignMap(CampaignScenarioID mapId);
	void setCampaignBonus(int bonusId);

	PlayerConnectionID getIdOfFirstUnallocatedPlayer() const;

	void multiplayerWelcomeMessage();
	void startDiscoveryListener();
	void stopDiscoveryListener();
};
