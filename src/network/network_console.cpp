//
//  SuperTuxKart - a fun racing game with go-kart
//  Copyright (C) 2015 Joerg Henrichs
//
//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 3
//  of the License, or (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

#include "karts/abstract_kart.hpp"
#include "karts/kart_properties.hpp"
#include "karts/kart_properties_manager.hpp"
#include "network/network_config.hpp"
#include "network/network_player_profile.hpp"
#include "network/network_string.hpp"
#include "network/remote_kart_info.hpp"
#include "network/server_config.hpp"
#include "network/socket_address.hpp"
#include "network/stk_host.hpp"
#include "network/stk_peer.hpp"
#include "network/protocols/server_lobby.hpp"
#include "utils/time.hpp"
#include "utils/vs.hpp"
#include "modes/world.hpp"
#include "main_loop.hpp"

#include <iostream>
#include <limits>
#include <string>
#include "utils/string_utils.hpp"

#ifndef WIN32
#  include <stdint.h>
#  include <sys/time.h>
#  include <unistd.h>
#endif

namespace NetworkConsole
{
#ifndef WIN32
std::string g_cmd_buffer;
#endif
// ----------------------------------------------------------------------------
void showHelp()
{
    std::cout << "Available command:" << std::endl;
    std::cout << "help, Print this." << std::endl;
    std::cout << "quit, Shut down the server." << std::endl;
    std::cout << "kickall, Kick all players out of STKHost." << std::endl;
    std::cout << "kick #, kick # peer of STKHost." << std::endl;
    std::cout << "kickban #, kick and ban # peer of STKHost." << std::endl;
    std::cout << "unban #, unban # peer of STKHost." << std::endl;
    std::cout << "listpeers, List all peers with host ID and IP." << std::endl;
    std::cout << "listban, List IP ban list of server." << std::endl;
    std::cout << "speedstats, Show upload and download speed." << std::endl;
    std::cout << "setplayer name, Set permission to player." << std::endl;
    std::cout << "setmoderator name, Set permission to moderator." 
        << std::endl;
    std::cout << "setadministrator name, Set permission to administrator." 
        << std::endl;
}   // showHelp

// ----------------------------------------------------------------------------
#ifndef WIN32
bool pollCommand()
{
    struct timeval timeout;
    fd_set rfds;
    int fd;
    char c;

    // stdin file descriptor is 0
    fd = 0;
    timeout.tv_sec = 0;
    timeout.tv_usec = 10000;

    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);

    if (select(fd + 1, &rfds, NULL, NULL, &timeout) <= 0)
        return false;
    if (read(fd, &c, 1) != 1)
        return false;

    if (c == '\n')
        return true;
    g_cmd_buffer += c;
    return false;
}   // pollCommand
#endif

// ----------------------------------------------------------------------------
void mainLoop(STKHost* host)
{
    VS::setThreadName("NetworkConsole");

#ifndef WIN32
    g_cmd_buffer.clear();
#endif

    showHelp();
    std::string str = "";
    while (!host->requestedShutdown())
    {
#ifndef WIN32
        if (!pollCommand())
            continue;

        std::stringstream ss(g_cmd_buffer);
        if (g_cmd_buffer.empty())
            continue;
        g_cmd_buffer.clear();
#else
        getline(std::cin, str);
        std::stringstream ss(str);
#endif

        //int number = -1;
        std::string str2 = "";
        ss >> str >> str2;
        if (str == "help")
        {
            showHelp();
        }
        else if (str == "quit")
        {
            host->requestShutdown();
        }
        else if (str == "addtime" && str2 != "" &&
            NetworkConfig::get()->isServer())
        {
            auto sl = LobbyProtocol::get<ServerLobby>();
            if (!sl)
                continue;

            int64_t amount_sec = 0;
            amount_sec = std::stol(str2);

            sl->changeTimeout(amount_sec);
            std::cout << "Timeout changed by " << amount_sec << "." << std::endl;
        }
        else if (str == "heavyparty" && str2 != "" &&
            NetworkConfig::get()->isServer())
        {
            auto sl = LobbyProtocol::get<ServerLobby>();
            if (!sl)
                continue;
            const bool state = str2 == "on";

            sl->setKartRestrictionMode(
                state ? KartRestrictionMode::HEAVY : KartRestrictionMode::NONE);

            std::string message("Heavy party is now ");
            if (state)
            {
                message += "ACTIVE. Only heavy karts can be chosen.";
            }
            else
            {
                message += "INACTIVE. All karts can be chosen.";
            }
            sl->sendStringToAllPeers(message);
        }
        else if (str == "mediumparty" && str2 != "" &&
            NetworkConfig::get()->isServer())
        {
            auto sl = LobbyProtocol::get<ServerLobby>();
            if (!sl)
                continue;
            const bool state = str2 == "on";

            sl->setKartRestrictionMode(
                state ? KartRestrictionMode::MEDIUM : KartRestrictionMode::NONE);

            std::string message("Medium party is now ");
            if (state)
            {
                message += "ACTIVE. Only medium karts can be chosen.";
            }
            else
            {
                message += "INACTIVE. All karts can be chosen.";
            }
            sl->sendStringToAllPeers(message);
        }
        else if (str == "lightparty" && str2 != "" &&
            NetworkConfig::get()->isServer())
        {
            auto sl = LobbyProtocol::get<ServerLobby>();
            if (!sl)
                continue;
            const bool state = str2 == "on";

            sl->setKartRestrictionMode(
                state ? KartRestrictionMode::LIGHT : KartRestrictionMode::NONE);

            std::string message("Light party is now ");
            if (state)
            {
                message += "ACTIVE. Only light karts (as primarily) can be chosen.";
            }
            else
            {
                message += "INACTIVE. All karts can be chosen.";
            }
            sl->sendStringToAllPeers(message);
        }
        else if (str == "bowlparty" && str2 != "" &&
            NetworkConfig::get()->isServer())
        {
            auto sl = LobbyProtocol::get<ServerLobby>();
            if (!sl)
                continue;
            const bool state = str2 == "on";

            RaceManager::get()->setPowerupSpecialModifier(
                    state ? Powerup::TSM_BOWLPARTY : Powerup::TSM_NONE);

            std::string message("Bowling party is now ");
            if (state)
            {
                message += "ACTIVE. Bonus boxes only give 3 bowling balls.";
            }
            else
            {
                message += "INACTIVE. All standard items as normal.";
            }
            sl->sendStringToAllPeers(message);
        }
        else if (str == "cakeparty" && str2 != "" &&
            NetworkConfig::get()->isServer())
        {
            auto sl = LobbyProtocol::get<ServerLobby>();
            if (!sl)
                continue;
            const bool state = str2 == "on";

            RaceManager::get()->setPowerupSpecialModifier(
                    state ? Powerup::TSM_CAKEPARTY : Powerup::TSM_NONE);

            std::string message("Cake party is now ");
            if (state)
            {
                message += "ACTIVE. Bonus boxes only give 2 cakes.";
            }
            else
            {
                message += "INACTIVE. All standard items as normal.";
            }
            sl->sendStringToAllPeers(message);
        }
        else if (str == "chaosparty" && str2 != "" &&
            NetworkConfig::get()->isServer())
        {
            auto sl = LobbyProtocol::get<ServerLobby>();
            if (!sl)
                continue;
            const bool state = str2 == "on";


            std::string message("Chaos party is now ");
            if (state)
            {
                RaceManager::get()->applyWorldTimedModifiers(TIERS_TMODIFIER_CHAOSPARTY);
                message += "ACTIVE. Random powerups of 50 are given to everyone each minute.";
            }
            else
            {
                RaceManager::get()->eraseWorldTimedModifiers(TIERS_TMODIFIER_CHAOSPARTY);
                message += "INACTIVE.";
            }
            sl->sendStringToAllPeers(message);
        }
        else if (str == "start" && NetworkConfig::get()->isServer())
        {
            auto sl = LobbyProtocol::get<ServerLobby>();
            if (!sl)
                continue;

            if (!STKHost::get()->getPeerCount())
            {
                std::cout << "No players online." << std::endl;
                continue;
            }

            sl->startSelection();
            std::cout << "Made the game start." << std::endl;
        }
        else if (str == "end" && NetworkConfig::get()->isServer())
        {
            auto sl = LobbyProtocol::get<ServerLobby>();
            if (!sl)
                continue;

            if (!sl->isRacing())
            {
                std::cout << "Game is not active." << std::endl;
                continue;
            }

            World* w = World::getWorld();
            if (!w)
                continue;

            w->scheduleInterruptRace();

            NetworkString* const ns = sl->getNetworkString();
            ns->setSynchronous(true);
            ns->addUInt8(ServerLobby::LE_CHAT);
            ns->encodeString16("The game has been interrupted.");

            STKHost::get()->sendPacketToAllPeersWith([](STKPeer* p)
                {
                    return !p->isWaitingForGame();
                }, ns, true/*reliable*/);
            std::cout << "Made the game end." << std::endl;
        }
        else if (str == "bc" || str == "broadcast")
        {
            auto sl = LobbyProtocol::get<ServerLobby>();
            if (!sl)
            {
                std::cout << "Server lobby is not available." << std::endl;
                continue;
            }
            std::string message;

            // I don't know if that will work...
            std::string cmd = ss.str();
            message = cmd.substr(std::min(str.length() + 1, cmd.length()), cmd.length());
            if (message.empty())
            {
                std::cout << "Cannot broadcast empty message" << std::endl;
                continue;
            }
            sl->sendStringToAllPeers(message);
        }
        else if ((str == "to" || str == "dm") && str2 != "")
        {
            auto sl = LobbyProtocol::get<ServerLobby>();
            if (!sl)
                continue;
            std::string message;

            if (ss.eof())
                continue;
            else
            {
                // I don't know if that will work...
                std::string cmd = ss.str();
                const size_t start = str.length() + str2.length() + 2;
                message = cmd.substr(std::min(start, cmd.length()), cmd.length());
            }
            if (message.empty())
            {
                std::cout << "Cannot send empty direct message" << std::endl;
                continue;
            }

            std::shared_ptr<STKPeer> peer = STKHost::get()->findPeerByName(
                    StringUtils::utf8ToWide(str2), true, true);
            if (!peer)
            {
                std::cout << "Player is not online." << std::endl;
                continue;
            }
            sl->sendStringToPeer(message, peer);

            if (!peer->hasPlayerProfiles())
                std::cout << "Message sent to the peer." << std::endl;
            else
                std::cout << "Message sent to " <<
                    StringUtils::wideToUtf8(peer->getPlayerProfiles()[0]->getName()
                    ) << "." << std::endl;
        }
        else if (str == "kickall")
        {
            auto peers = host->getPeers();
            for (unsigned int i = 0; i < peers.size(); i++)
            {
                peers[i]->kick();
            }
        }
        else if (str == "kick" && str2 != "" &&
            NetworkConfig::get()->isServer())
        {
            std::shared_ptr<STKPeer> peer = STKHost::get()->findPeerByName(StringUtils::utf8ToWide(str2));
            if (peer)
                peer->kick();
            else
                std::cout << "Unknown player: " << str2 << std::endl;
        }
        else if (str == "kickban" && str2 != "" &&
            NetworkConfig::get()->isServer())
        {
            std::shared_ptr<STKPeer> peer = STKHost::get()->findPeerByName(StringUtils::utf8ToWide(str2));
            if (peer)
            {
                peer->kick();
                // ATM use permanently ban
                auto sl = LobbyProtocol::get<ServerLobby>();
                // We don't support banning IPv6 address atm
                if (sl && !peer->getAddress().isIPv6())
                    sl->saveIPBanTable(peer->getAddress());
            }
            else
                std::cout << "Unknown player: " << str2 << std::endl;
        }
        else if (str == "unban" && str2 != "" &&
            NetworkConfig::get()->isServer())
        {
            SocketAddress addr = str2;

            auto sl = LobbyProtocol::get<ServerLobby>();
            if (sl && !addr.isIPv6())
            {
                sl->removeIPBanTable(addr);
                std::cout << "IP address has been unbanned." << std::endl;
            }
        }
        else if (str == "onlineban" && str2 != "" &&
            NetworkConfig::get()->isServer())
        {
            auto sl = LobbyProtocol::get<ServerLobby>();
            if (!sl)
                continue;

            std::string reason;
            int days = -1;
            if (ss.eof())
                reason = "";
            else
            {
                // I don't know if that will work...
                std::string cmd = ss.str();
                reason = cmd.substr(sizeof("onlineban") + str2.length(), cmd.length());
                if (reason.substr(0, 4) == "days")
                {
                    ss.seekg(5);
                    ss >> days;
                }
            }

            // Add the ban
            int res = sl->banPlayer(str2, reason, days);
            if (res < 0 || res == 2)
            {
                std::cout << "Database error." << std::endl;
                continue;
            }
            if (res != 0)
            {
                std::cout << "Player's OID has not been found in the stats." << std::endl;
                continue;
            }

            std::cout << "Player has been banned for " << days << " days because of " <<
                (reason.empty() ? reason : "(unspecified)") << "." << std::endl;
            
        }
        else if (str == "onlineunban")
        {
            auto sl = LobbyProtocol::get<ServerLobby>();
            if (!sl)
                continue;

            // Remove the ban
            int res = sl->unbanPlayer(str2);
            if (res < 0 || res == 2)
            {
                std::cout << "Database error." << std::endl;
                continue;
            }
            if (res != 0)
            {
                std::cout << "Player's OID has not been found in the stats." << std::endl;
                continue;
            }

            std::cout << "Player has been unbanned." << std::endl;
        }
        else if (str == "listpeers")
        {
            auto peers = host->getPeers();
            if (peers.empty())
                std::cout << "No peers exist" << std::endl;
            for (unsigned int i = 0; i < peers.size(); i++)
            {
                std::cout << peers[i]->getHostId() << ": " <<
                    peers[i]->getAddress().toString() <<  " " <<
		    StringUtils::wideToUtf8(peers[i]->getPlayerProfiles()[0]->getName()) << " " <<
                    peers[i]->getUserVersion() << std::endl;
            }
        }
        else if (str == "listban")
        {
            auto sl = LobbyProtocol::get<ServerLobby>();
            if (sl)
                sl->listBanTable();
        }
        else if (str == "speedstats")
        {
            std::cout << "Upload speed (KBps): " <<
                (float)host->getUploadSpeed() / 1024.0f <<
                "   Download speed (KBps): " <<
                (float)host->getDownloadSpeed() / 1024.0f  << std::endl;
        }
        // MODERATION TOOLKIT
        else if (str == "setplayer")
        {
            auto sl = LobbyProtocol::get<ServerLobby>();
            if (!sl)
                continue;

            auto player = STKHost::get()->findPeerByName(
                StringUtils::utf8ToWide(str2)
            );
            if (player)
            {
                player->getPlayerProfiles()[0]->setPermissionLevel(
                        ServerLobby::PERM_PLAYER);
            }
            uint32_t oid = sl->lookupOID(str2);
            if (!oid)
            {
                std::cout << "Player has no recorded online id, changes are temporary." << std::endl;
            }
            else
                sl->writePermissionLevelForOID(oid, ServerLobby::PERM_PLAYER);
            std::cout << "Set " << str2 << " as player (0)." << std::endl;
        }
        else if (str == "setmoderator")
        {
            auto sl = LobbyProtocol::get<ServerLobby>();
            if (!sl)
                continue;

            auto player = STKHost::get()->findPeerByName(
                StringUtils::utf8ToWide(str2)
            );
            if (player)
            {
                player->getPlayerProfiles()[0]->setPermissionLevel(
                        ServerLobby::PERM_MODERATOR);
            }
            uint32_t oid = sl->lookupOID(str2);
            if (!oid)
            {
                std::cout << "Player has no recorded online id, changes are temporary." << std::endl;
            }
            else
                sl->writePermissionLevelForOID(oid, ServerLobby::PERM_MODERATOR);
            std::cout << "Set " << str2 << " as moderator (80)." << std::endl;
        }
        else if (str == "setadministrator")
        {
            auto sl = LobbyProtocol::get<ServerLobby>();
            if (!sl)
                continue;

            auto player = STKHost::get()->findPeerByName(
                StringUtils::utf8ToWide(str2)
            );

            if (player)
            {
                player->getPlayerProfiles()[0]->setPermissionLevel(
                        ServerLobby::PERM_ADMINISTRATOR);
            }
            uint32_t oid = sl->lookupOID(str2);
            if (!oid)
            {
                std::cout << "Player has no recorded online id, changes are temporary." << std::endl;
            }
            else
                sl->writePermissionLevelForOID(oid, ServerLobby::PERM_ADMINISTRATOR);
            std::cout << "Set " << str2 << " as administrator (100)." << std::endl;
        }
        else if (str == "setperm")
        {
            int lvl = 0;
            ss >> lvl;
            if (ss.bad())
            {
                std::cout << "Invalid permission level specified." << std::endl;
                continue;
            }
            auto sl = LobbyProtocol::get<ServerLobby>();
            if (!sl)
                continue;

            auto player = STKHost::get()->findPeerByName(
                StringUtils::utf8ToWide(str2)
            );

            if (player)
            {
                player->getPlayerProfiles()[0]->setPermissionLevel(
                        lvl);
            }
            uint32_t oid = sl->lookupOID(str2);
            if (!oid)
            {
                std::cout << "Player has no recorded online id, changes are temporary." << std::endl;
            }
            else
                sl->writePermissionLevelForOID(oid, lvl);
            std::cout << "Set " << str2 << " to " << lvl << "." << std::endl;
        }
        else if (str == "restrict")
        {
            auto sl = LobbyProtocol::get<ServerLobby>();
            if (!sl)
                continue;
            std::string playername, restrictionname;
            ss >> restrictionname;
            if (ss.bad())
            {
                std::cout << "Specify name of the restriction: nospec, nogame, nochat, nopchat, noteam, handicap, kart, track"
                    << std::endl;
                continue;
            }
            ss >> playername;
            if (ss.bad())
            {
                std::cout << "Specify the name of the player for the restriction to be applied to." << std::endl;
                continue;
            }
            bool state = str2 == "on";
            PlayerRestriction restriction = sl->getRestrictionValue(
                    restrictionname);
            if (restriction == PRF_OK && state)
            {
                std::cout << "Invalid name for restriction: " + restrictionname + ".";
                continue;
            }
            
            auto target = STKHost::get()->findPeerByName(
                    StringUtils::utf8ToWide(playername), true, true);
            uint32_t target_r = sl->loadRestrictionsForUsername(
                    StringUtils::utf8ToWide(playername)
                    );

            if (target && target->hasPlayerProfiles() &&
                    target->getPlayerProfiles()[0]->getOnlineId() != 0)
            {
                auto& targetPlayer = target->getPlayerProfiles()[0];
                if (!state && restriction == PRF_OK)
                    targetPlayer->clearRestrictions();
                else if (state)
                    targetPlayer->addRestriction(restriction);
                else
                    targetPlayer->removeRestriction(restriction);
                sl->writeRestrictionsForUsername(
                        targetPlayer->getName(),
                        targetPlayer->getRestrictions());
            }

            uint32_t online_id = sl->lookupOID(playername);
            if (online_id)
            {
                sl->writeRestrictionsForUsername(
                        StringUtils::utf8ToWide(playername), 
                        state ? target_r | restriction : target_r & ~restriction);
                std::cout << "Set " << restrictionname << " to " << str2 << " for player " << 
                    playername << ". " << std::endl;
                continue;
            }
        }
        else if (str == "setteam")
        {
            auto sl = LobbyProtocol::get<ServerLobby>();
            if (!sl)
                continue;
            // str2 is the team name
            KartTeam team = KART_TEAM_NONE;
            std::string playername;
            ss >> playername;
            if (ss.bad())
            {
                std::cout << "Specify player name." << std::endl;
                continue;
            }
            
            if (str2 == "blue")
                team = KART_TEAM_BLUE;
            else if (str2 == "red")
                team = KART_TEAM_RED;
            else if (str2 != "none")
            {
                std::cout << "Specify either blue, red or none for second argument." << std::endl;
                continue;
            }

            std::shared_ptr<NetworkPlayerProfile> player = nullptr;
            auto peer = STKHost::get()->findPeerByName(
                    StringUtils::utf8ToWide(playername), true, true, &player);
            if (!player || !peer || !peer->hasPlayerProfiles())
            {
                std::cout << "Invalid target player: " << playername << std::endl;
                continue;
            }
            
            sl->forceChangeTeam(player.get(), team);
            sl->updatePlayerList();

            std::cout << "Set player team to " << str2 << " for " << 
                StringUtils::wideToUtf8(player->getName())<< std::endl;
        }
        else if (str == "setkart")
        {
            auto sl = LobbyProtocol::get<ServerLobby>();
            if (!sl)
                continue;

            // str2 is the kart name
            std::string playername;
            ss >> playername;
            if (ss.bad())
            {
                std::cout << "Specify player name." << std::endl;
                continue;
            }

            const KartProperties* kart =
                kart_properties_manager->getKart(str2);
            if ((!kart || kart->isAddon()) && str2 != "off")
            {
                std::cout << "Kart \"" << str2 << "\" does not exist or is not a standard kart." \
                    << std::endl;
                continue;
            }

            std::shared_ptr<NetworkPlayerProfile> t_player = nullptr;
            auto t_peer = STKHost::get()->findPeerByName(
                    StringUtils::utf8ToWide(playername), true, true, &t_player);
            if (!t_player || !t_peer || !t_peer->hasPlayerProfiles())
            {
                std::cout << "Invalid target player: " << playername << std::endl;
                continue;
            }

            if (str2 == "off")
            {
                NetworkString* tmsg = sl->getNetworkString();
                tmsg->setSynchronous(true);
                tmsg->addUInt8(LobbyProtocol::LE_CHAT);
                tmsg->encodeString16(
                        L"You can choose any kart now.");
                t_peer->sendPacket(tmsg, true/*reliable*/);
                delete tmsg;

                t_player->unforceKart();
                std::cout << "No longer forcing a kart for " << playername << "." << std::endl;
                continue;
            }
            else
            {
                std::string targetmsg = "Your kart is " + str2 + " now.";
                NetworkString* tmsg = sl->getNetworkString();
                tmsg->setSynchronous(true);
                tmsg->addUInt8(LobbyProtocol::LE_CHAT);
                tmsg->encodeString16(
                        StringUtils::utf8ToWide(targetmsg));
                t_peer->sendPacket(tmsg, true/*reliable*/);
                delete tmsg;
                t_player->forceKart(str2);
                std::cout << "Made " <<StringUtils::wideToUtf8(t_player->getName()) <<
                    " use kart " << str2 << "." << std::endl;
            }

        }
        else if (str == "sethandicap")
        {
            auto sl = LobbyProtocol::get<ServerLobby>();
            if (!sl)
                continue;
            // str2 is the team name
            HandicapLevel handicap = HANDICAP_NONE;
            std::string playername;
            ss >> playername;
            if (ss.bad())
            {
                std::cout << "Specify player name." << std::endl;
                continue;
            }
            
            if (str2 == "count")
                handicap = HANDICAP_COUNT;
            else if (str2 == "medium")
                handicap = HANDICAP_MEDIUM;
            else if (str2 != "none")
            {
                std::cout << "Specify either count, medium or none for second argument." << std::endl;
                continue;
            }

            std::shared_ptr<NetworkPlayerProfile> player = nullptr;
            auto peer = STKHost::get()->findPeerByName(
                    StringUtils::utf8ToWide(playername), true, true, &player);
            if (!player || !peer || !peer->hasPlayerProfiles())
            {
                std::cout << "Invalid target player: " << playername << std::endl;
                continue;
            }
            
            player->setHandicap(handicap);
            sl->updatePlayerList();

            std::cout << "Set player handicap to " << str2 << " for " << 
                StringUtils::wideToUtf8(player->getName())<< std::endl;
        }
        // CHEATS
        else if (str == "hackitem" || str == "hki")
        {
            auto sl = LobbyProtocol::get<ServerLobby>();
            if (!sl)
                continue;

            if (!sl->isRacing())
            {
                std::cout << "The game is not running." << std::endl;
                continue;
            }
            if (str2.empty())
            {
                std::cout << "Specify powerup type." << std::endl;
                continue;
            }

            World* w = World::getWorld();
            if (!w)
            {
                std::cout << "World is not available right now." << std::endl;
                continue;
            }
            AbstractKart* target;
            if (str2.empty())
            {
                std::cout << "Specify quantity." << std::endl;
                continue;
            }
            unsigned int quantity;
            ss >> quantity;
            if (ss.bad())
            {
                std::cout << "Wrong quantity." << std::endl;
                continue;
            }

            std::string player_name;
            ss >> player_name;
            if (ss.bad())
            {
                std::cout << "Specify player name." << std::endl;
                continue;
            }

            // 2 arguments: item quantity: give to yourself
            // 3 arguments: item quantity player: give to player
            std::shared_ptr<STKPeer> target_peer;
            target_peer = STKHost::get()->findPeerByName(
                        StringUtils::utf8ToWide(player_name),
                        true, true
                        );

            if (!target_peer)
            {
                std::cout << "Player is not online." << std::endl;
                continue;
            }
            const std::set<unsigned int>& k_ids
                = target_peer->getAvailableKartIDs();
            if (target_peer->isWaitingForGame() || k_ids.empty())
            {
                std::cout << "Player is not in the game or has no available karts."
                    << std::endl;
                continue;
            }
            else if (k_ids.size() > 1)
            {
                Log::warn("NetworkConsole", "hackitem: Player %s has multiple kart IDs.", 
                        StringUtils::wideToUtf8(target_peer->getPlayerProfiles()[0]->getName()).c_str());
                continue;
            }
            unsigned int a = *k_ids.begin();
            target = w->getKart(a);
            PowerupManager::PowerupType type =
                PowerupManager::getPowerupFromName(str2);

            if (type == PowerupManager::POWERUP_NOTHING)
                quantity = 0;

            // set the powerup
            target->setPowerup(PowerupManager::POWERUP_NOTHING, 0);
            target->setPowerup(type, quantity);
            std::string msgtarget = "Your powerup has been changed.";
            sl->sendStringToPeer(msgtarget, target_peer);
            std::string msg = StringUtils::insertValues(
                "Changed powerup for player %s.",
                StringUtils::wideToUtf8(
                    target_peer->getPlayerProfiles()[0]->getName()).c_str());
            std::cout << msg << std::endl;

        }
        else if (str == "hacknitro" || str == "hkn")
        {
            auto sl = LobbyProtocol::get<ServerLobby>();
            if (!sl)
                continue;

            if (!sl->isRacing())
            {
                std::cout << "The game is not running." << std::endl;
                continue;
            }

            World* w = World::getWorld();
            if (!w)
            {
                std::cout << "World is not available right now." << std::endl;
                continue;
            }
            AbstractKart* target;
            float quantity = std::stof(str2);

            std::string player_name;
            ss >> player_name;
            if (ss.bad())
            {
                std::cout << "Specify player name." << std::endl;
                continue;
            }

            // 2 arguments: item quantity: give to yourself
            // 3 arguments: item quantity player: give to player
            std::shared_ptr<STKPeer> target_peer;
            target_peer = STKHost::get()->findPeerByName(
                        StringUtils::utf8ToWide(player_name),
                        true, true
                        );

            if (!target_peer)
            {
                std::cout << "Player is not online." << std::endl;
                continue;
            }
            const std::set<unsigned int>& k_ids
                = target_peer->getAvailableKartIDs();
            if (target_peer->isWaitingForGame() || k_ids.empty())
            {
                std::cout << "Player is not in the game or has no available karts."
                    << std::endl;
                continue;
            }
            else if (k_ids.size() > 1)
            {
                Log::warn("NetworkConsole", "hackitem: Player %s has multiple kart IDs.", 
                        StringUtils::wideToUtf8(target_peer->getPlayerProfiles()[0]->getName()).c_str());
                continue;
            }
            unsigned int a = *k_ids.begin();
            target = w->getKart(a);

            // set the nitro
            target->setEnergy(.0f);
            target->setEnergy(quantity);
            std::string msgtarget = "Your nitro has been changed.";
            sl->sendStringToPeer(msgtarget, target_peer);
            std::string msg = StringUtils::insertValues(
                "Changed nitro for player %s.",
                StringUtils::wideToUtf8(
                    target_peer->getPlayerProfiles()[0]->getName()).c_str());
            std::cout << msg << std::endl;

        }
        else
        {
            std::cout << "Unknown command: " << str << std::endl;
        }
    }   // while !stop
    main_loop->requestAbort();
}   // mainLoop

}
