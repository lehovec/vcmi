# Networking

## The big picture

For implementation details see files located at `lib/network` directory.

VCMI uses connection using TCP to communicate with server, even in single-player games. However, even though TCP is stream-based protocol, VCMI uses atomic messages for communication. Each message is a serialized stream of bytes, preceded by 4-byte message size:

```cpp
int32_t messageSize;
byte messagePayload[messageSize];
```

Networking can be used by:

- game client (vcmiclient / VCMI_Client.exe). Actual application that player interacts with directly using UI.
- match server (vcmiserver / VCMI_Server.exe / part of game client). This app controls game logic and coordinates multiplayer games.
- lobby server (vcmilobby). This app provides access to global lobby through which players can play game over Internet.

Following connections can be established during game lifetime:

- game client -> match server: This is main connection for use during gameplay, created once player requests to move from main menu to pregame / match lobby (e.g. after pressing New Game / Load Game)
- game client -> lobby server: This connection is used to access global lobby, for multiplayer over Internet. Created when player logs into a lobby (Multiplayer -> Connect to global service)
- match server -> lobby server: This connection is established when player creates new multiplayer room via lobby. It is used by lobby server to send commands to match server

## Gameplay communication

For gameplay, VCMI serializes data into a binary stream. See [Serialization](Serialization.md) for more information.

### Rejoining a game in progress

A player whose client leaves a multiplayer game keeps their seat; their turns and battles wait for them. A turn timer keeps running for an offline player. A client that connects during gameplay sends `LobbyClientConnected` as usual and receives the seats of offline players with the same names. The `uuid` of the pack starts with a device id that the client keeps in its settings. A seat of the same device is preferred: when several players share a name, a client of another device takes none of their seats. A seat of the same device is taken back also while its old connection is still open, because a connection that dropped without closing stays open until a network timeout. The client receives:

- `LobbyRejoinGame` instead of `LobbyStartGame`. A serialized gamestate does not contain battles, so during a battle it holds the gamestate the day started from and every pack applied since.
- `PackRepeatedOnRejoin` for every question that the player has not answered yet, and for the start of their turn. The client only notifies its interfaces, the gamestate already contains their effects.
- `BattleResumed` for every battle of the player, followed by the unit that waits for orders.

The host client controls the AI players and the neutral side. A server started by a client stops when the host leaves. A dedicated server, started as `vcmiserver` on its own, keeps the game while a human player is still in it:

- When the host leaves, another connected client becomes the host. It receives `LobbyUpdateState`, creates the interfaces of the AI players, and then the same repeated packs as a rejoining client, for the AI players and the neutral side only.
- When no client is connected, the game waits. The first client that rejoins becomes the host.
- The server stops when the last human player won or lost and the host leaves, or when the host sends the `!exit` chat command.
- Started with `--on-turn-start <command>`, a dedicated server runs the command when the turn of a human player starts, also when it is resumed from a save. Its arguments are the player name, the player color, the day since the game start, `online` or `offline`, and the notification topic of the player, e.g. `vcmi-k7q2`. The topic is derived from a secret that the server keeps in `notification-secret.txt` in its configuration directory, from the start time of the game and from the player color, so it stays the same after a restart. A client receives the topics of its players as a system message when the game starts or when it rejoins, and again on the `!notify` chat command. A command without a directory is searched in `PATH`, a relative path is relative to the directory the server was started in.
- Before the turn of a human player starts, a dedicated server saves the game as an autosave, unless a battle is in progress. Started with `--resume`, it loads the newest autosave in the saves directory instead of opening the lobby, and exits if loading fails. Every human player is offline then, and rejoins the game under the same name.

## Global lobby communication

For implementation details see:

- game client: `client/globalLobby/GlobalLobbyClient.h
- match server: `server/GlobalLobbyProcessor.h
- lobby server: `client/globalLobby/GlobalLobbyClient.h

In case of global lobby, message payload uses plaintext json format - utf-8 encoded string:

```cpp
int32_t messageSize;
char jsonString[messageSize];
```

Every message must be a struct (json object) that contains "type" field. Unlike rest of VCMI codebase, this message is validated as strict json, without any extensions, such as comments.

### Communication flow

Notes:

- invalid message, such as corrupted json format or failure to validate message will result in no reply from server
- in addition to specified messages, match server will send `operationFailed` message on failure to apply player request

#### New Account Creation

- client -> lobby: `clientRegister`
- lobby -> client: `accountCreated`

#### Login

- client -> lobby: `clientLogin`
- lobby -> client: `loginSuccess`
- lobby -> client: `chatHistory`
- lobby -> client: `activeAccounts`
- lobby -> client: `activeGameRooms`

#### Chat Message

- client -> lobby: `sendChatMessage`
- lobby -> every client: `chatMessage`

#### New Game Room

- client starts match server instance
- match -> lobby: `serverLogin`
- lobby -> match: `loginSuccess`
- match accepts connection from client
- client -> lobby: `activateGameRoom`
- lobby -> client: `joinRoomSuccess`
- lobby -> every client: `activeAccounts`
- lobby -> every client: `activeGameRooms`

#### Joining a game room

See [#Proxy mode](#proxy-mode)

#### Leaving a game room

- client closes connection to match server
- match -> lobby: `leaveGameRoom`

#### Sending an invite

- client -> lobby: `sendInvite`
- lobby -> target client: `inviteReceived`

Note: there is no dedicated procedure to accept an invite. Instead, invited player will use same flow as when joining public game room

#### Logout

- client closes connection
- lobby -> every client: `activeAccounts`

### Proxy mode

In order to connect players located behind NAT, VCMI lobby can operate in "proxy" mode. In this mode, connection will be act as proxy and will transmit gameplay data from client to a match server, without any data processing on lobby server.

Currently, process to establish connection using proxy mode is:

- Player attempt to join open game room using `joinGameRoom` message
- Lobby server validates requests and on success - notifies match server about new player in lobby using control connection
- Match server receives request, establishes new connection to game lobby, sends `serverProxyLogin` message to lobby server and immediately transfers this connection to VCMIServer class to use as connection for gameplay communication
- Lobby server accepts new connection and then sends reply to client using `joinRoomSuccess` message.
- Game client receives message and establishes own side of proxy connection - connects to lobby, sends `clientProxyLogin` message and transfers to ServerHandler class to use as connection for gameplay communication
- Lobby server accepts new connection and moves it into a proxy mode - all packages that will be received by one side of this connection will be re-sent to another side without any processing.

Once the game is over (or if one side disconnects) lobby server will close another side of the connection and erase proxy connection
