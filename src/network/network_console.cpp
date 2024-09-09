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

#include "network/network_config.hpp"
#include "network/network_player_profile.hpp"
#include "network/server_config.hpp"
#include "network/socket_address.hpp"
#include "network/stk_host.hpp"
#include "network/stk_peer.hpp"
#include "network/protocols/server_lobby.hpp"
#include "utils/time.hpp"
#include "utils/vs.hpp"
#include "main_loop.hpp"

#include <iostream>
#include <limits>
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
        else if (str == "onlineban")
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
        else
        {
            std::cout << "Unknown command: " << str << std::endl;
        }
    }   // while !stop
    main_loop->requestAbort();
}   // mainLoop

}
