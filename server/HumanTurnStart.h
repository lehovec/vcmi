/*
 * HumanTurnStart.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "../lib/constants/EntityIdentifiers.h"

/// Turn of a human player that started, also when it is resumed from a save
struct HumanTurnStart
{
	PlayerColor player;
	std::string name;
	int day = 0;
	bool online = false; ///< a connected client controls the player
	std::string topic; ///< notification topic of the player, see CGameHandler::notificationTopic
};

/// Notifies players outside of the game, e.g. by a command that the dedicated server runs
using HumanTurnStartListener = std::function<void(const HumanTurnStart &)>;
