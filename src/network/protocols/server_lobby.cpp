//
//  SuperTuxKart - a fun racing game with go-kart
//  Copyright (C) 2013-2015 SuperTuxKart-Team
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

#include "network/protocols/server_lobby.hpp"

#include "addons/addon.hpp"
#include "config/user_config.hpp"
#include "irrMath.h"
#include "irrString.h"
#include "items/network_item_manager.hpp"
#include "items/powerup_manager.hpp"
#include "karts/abstract_kart.hpp"
#include "karts/controller/player_controller.hpp"
#include "karts/kart_properties.hpp"
#include "karts/kart_properties_manager.hpp"
#include "karts/official_karts.hpp"
#include "modes/capture_the_flag.hpp"
#include "modes/linear_world.hpp"
#include "modes/soccer_world.hpp"
#include "network/crypto.hpp"
#include "network/event.hpp"
#include "network/game_setup.hpp"
#include "network/network.hpp"
#include "network/network_config.hpp"
#include "network/network_player_profile.hpp"
#include "network/peer_vote.hpp"
#include "network/protocol_manager.hpp"
#include "network/protocols/connect_to_peer.hpp"
#include "network/protocols/game_protocol.hpp"
#include "network/protocols/game_events_protocol.hpp"
#include "network/protocols/global_log.hpp"
#include "network/race_event_manager.hpp"
#include "network/remote_kart_info.hpp"
#include "network/server_config.hpp"
#include "network/socket_address.hpp"
#include "network/server.hpp"
#include "network/stk_host.hpp"
#include "network/stk_ipv6.hpp"
#include "network/stk_peer.hpp"
#include "online/online_profile.hpp"
#include "online/request_manager.hpp"
#include "online/xml_request.hpp"
#include "tracks/check_manager.hpp"
#include "tracks/track.hpp"
#include "tracks/track_manager.hpp"
#include "utils/log.hpp"
#include "utils/random_generator.hpp"
#include "utils/string_utils.hpp"
#include "utils/time.hpp"
//#include "utils/translation.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <ctime>
#include <cwchar>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

int ServerLobby::m_fixed_laps = -1;
// ========================================================================
class SubmitRankingRequest : public Online::XMLRequest
{
public:
    SubmitRankingRequest(uint32_t online_id, double scores,
                         double max_scores, unsigned num_races,
                         double raw_scores, double rating_deviation,
                         uint64_t disconnects,
                         const std::string& country_code)
        : XMLRequest(Online::RequestManager::HTTP_MAX_PRIORITY)
    {
        addParameter("id", online_id);
        addParameter("scores", scores);
        addParameter("max-scores", max_scores);
        addParameter("num-races-done", num_races);
        addParameter("raw-scores", raw_scores);
        addParameter("rating-deviation", rating_deviation);
        addParameter("disconnects", disconnects);
        addParameter("country-code", country_code);
    }
    virtual void afterOperation()
    {
        Online::XMLRequest::afterOperation();
        const XMLNode* result = getXMLData();
        std::string rec_success;
        if (!(result->get("success", &rec_success) &&
            rec_success == "yes"))
        {
            Log::error("ServerLobby", "Failed to submit scores.");
        }
    }
};   // UpdatePlayerRankingRequest
// ========================================================================

// We use max priority for all server requests to avoid downloading of addons
// icons blocking the poll request in all-in-one graphical client server

#ifdef ENABLE_SQLITE3

// ----------------------------------------------------------------------------
static void upperIPv6SQL(sqlite3_context* context, int argc,
                         sqlite3_value** argv)
{
    if (argc != 1)
    {
        sqlite3_result_int64(context, 0);
        return;
    }

    char* ipv6 = (char*)sqlite3_value_text(argv[0]);
    if (ipv6 == NULL)
    {
        sqlite3_result_int64(context, 0);
        return;
    }
    sqlite3_result_int64(context, upperIPv6(ipv6));
}

// ----------------------------------------------------------------------------
void insideIPv6CIDRSQL(sqlite3_context* context, int argc,
                       sqlite3_value** argv)
{
    if (argc != 2)
    {
        sqlite3_result_int(context, 0);
        return;
    }

    char* ipv6_cidr = (char*)sqlite3_value_text(argv[0]);
    char* ipv6_in = (char*)sqlite3_value_text(argv[1]);
    if (ipv6_cidr == NULL || ipv6_in == NULL)
    {
        sqlite3_result_int(context, 0);
        return;
    }
    sqlite3_result_int(context, insideIPv6CIDR(ipv6_cidr, ipv6_in));
}   // insideIPv6CIDRSQL

// ----------------------------------------------------------------------------
/*
Copy below code so it can be use as loadable extension to be used in sqlite3
command interface (together with andIPv6 and insideIPv6CIDR from stk_ipv6)

#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1
// ----------------------------------------------------------------------------
sqlite3_extension_init(sqlite3* db, char** pzErrMsg,
                       const sqlite3_api_routines* pApi)
{
    SQLITE_EXTENSION_INIT2(pApi)
    sqlite3_create_function(db, "insideIPv6CIDR", 2, SQLITE_UTF8, NULL,
        insideIPv6CIDRSQL, NULL, NULL);
    sqlite3_create_function(db, "upperIPv6", 1, SQLITE_UTF8,  0, upperIPv6SQL,
        0, 0);
    return 0;
}   // sqlite3_extension_init
*/

#endif

/** This is the central game setup protocol running in the server. It is
 *  mostly a finite state machine. Note that all nodes in ellipses and light
 *  grey background are actual states; nodes in boxes and white background 
 *  are functions triggered from a state or triggering potentially a state
 *  change.
 \dot
 digraph interaction {
 node [shape=box]; "Server Constructor"; "playerTrackVote"; "connectionRequested"; 
                   "signalRaceStartToClients"; "startedRaceOnClient"; "loadWorld";
 node [shape=ellipse,style=filled,color=lightgrey];

 "Server Constructor" -> "INIT_WAN" [label="If WAN game"]
 "Server Constructor" -> "WAITING_FOR_START_GAME" [label="If LAN game"]
 "INIT_WAN" -> "GETTING_PUBLIC_ADDRESS" [label="GetPublicAddress protocol callback"]
 "GETTING_PUBLIC_ADDRESS" -> "WAITING_FOR_START_GAME" [label="Register server"]
 "WAITING_FOR_START_GAME" -> "connectionRequested" [label="Client connection request"]
 "connectionRequested" -> "WAITING_FOR_START_GAME"
 "WAITING_FOR_START_GAME" -> "SELECTING" [label="Start race from authorised client"]
 "SELECTING" -> "SELECTING" [label="Client selects kart, #laps, ..."]
 "SELECTING" -> "playerTrackVote" [label="Client selected track"]
 "playerTrackVote" -> "SELECTING" [label="Not all clients have selected"]
 "playerTrackVote" -> "LOAD_WORLD" [label="All clients have selected; signal load_world to clients"]
 "LOAD_WORLD" -> "loadWorld"
 "loadWorld" -> "WAIT_FOR_WORLD_LOADED" 
 "WAIT_FOR_WORLD_LOADED" -> "WAIT_FOR_WORLD_LOADED" [label="Client or server loaded world"]
 "WAIT_FOR_WORLD_LOADED" -> "signalRaceStartToClients" [label="All clients and server ready"]
 "signalRaceStartToClients" -> "WAIT_FOR_RACE_STARTED"
 "WAIT_FOR_RACE_STARTED" ->  "startedRaceOnClient" [label="Client has started race"]
 "startedRaceOnClient" -> "WAIT_FOR_RACE_STARTED" [label="Not all clients have started"]
 "startedRaceOnClient" -> "DELAY_SERVER" [label="All clients have started"]
 "DELAY_SERVER" -> "DELAY_SERVER" [label="Not done waiting"]
 "DELAY_SERVER" -> "RACING" [label="Server starts race now"]
 }
 \enddot


 *  It starts with detecting the public ip address and port of this
 *  host (GetPublicAddress).
 */
ServerLobby::ServerLobby() : LobbyProtocol()
{
    m_client_server_host_id.store(0);
    m_lobby_players.store(0);
    m_current_ai_count.store(0);
    std::vector<int> all_t =
        track_manager->getTracksInGroup("standard");
    std::vector<int> all_arenas =
        track_manager->getArenasInGroup("standard", false);
    std::vector<int> all_soccers =
        track_manager->getArenasInGroup("standard", true);
    all_t.insert(all_t.end(), all_arenas.begin(), all_arenas.end());
    all_t.insert(all_t.end(), all_soccers.begin(), all_soccers.end());

    m_official_kts.first = OfficialKarts::getOfficialKarts();
    for (int track : all_t)
    {
        Track* t = track_manager->getTrack(track);
        if (!t->isAddon())
            m_official_kts.second.insert(t->getIdent());
    }
    updateAddons();

    m_rs_state.store(RS_NONE);
    m_last_success_poll_time.store(StkTime::getMonoTimeMs() + 30000);
    m_last_unsuccess_poll_time = StkTime::getMonoTimeMs();
    m_server_owner_id.store(-1);
    m_registered_for_once_only = false;
    setHandleDisconnections(true);
    m_state = SET_PUBLIC_ADDRESS;
    m_save_server_config = true;
    if (ServerConfig::m_ranked)
    {
        Log::info("ServerLobby", "This server will submit ranking scores to "
            "the STK addons server. Don't bother hosting one without the "
            "corresponding permissions, as they would be rejected.");
    }
    m_result_ns = getNetworkString();
    m_result_ns->setSynchronous(true);
    m_items_complete_state = new BareNetworkString();
    m_server_id_online.store(0);
    m_max_players         = ServerConfig::m_server_max_players;
    m_max_players_in_game = ServerConfig::m_max_players_in_game;
    m_powerupper_active   = false;
    m_difficulty.store(ServerConfig::m_server_difficulty);
    m_game_mode.store(ServerConfig::m_server_mode);
    m_default_vote = new PeerVote();
    m_player_reports_table_exists = false;
    m_allow_powerupper = ServerConfig::m_allow_powerupper;
    m_last_wanrefresh_cmd_time = 0UL;
    m_last_wanrefresh_res = nullptr;
    m_last_wanrefresh_requester.reset();
    initDatabase();
}   // ServerLobby

//-----------------------------------------------------------------------------
/** Destructor.
 */
ServerLobby::~ServerLobby()
{
    if (m_server_id_online.load() != 0)
    {
        // For child process the request manager will keep on running
        unregisterServer(m_process_type == PT_MAIN ? true : false/*now*/);
    }
    delete m_result_ns;
    delete m_items_complete_state;
    if (m_save_server_config)
        ServerConfig::writeServerConfigToDisk();
    delete m_default_vote;
    destroyDatabase();
}   // ~ServerLobby

//-----------------------------------------------------------------------------
void ServerLobby::initDatabase()
{
#ifdef ENABLE_SQLITE3
    m_last_poll_db_time = StkTime::getMonoTimeMs();
    m_db = NULL;
    m_ip_ban_table_exists = false;
    m_ipv6_ban_table_exists = false;
    m_online_id_ban_table_exists = false;
    m_ip_geolocation_table_exists = false;
    m_ipv6_geolocation_table_exists = false;
    m_permissions_table_exists = false;
    m_restrictions_table_exists = false;
    if (!ServerConfig::m_sql_management)
        return;
    const std::string& path = ServerConfig::getConfigDirectory() + "/" +
        ServerConfig::m_database_file.c_str();
    int ret = sqlite3_open_v2(path.c_str(), &m_db,
        SQLITE_OPEN_SHAREDCACHE | SQLITE_OPEN_FULLMUTEX |
        SQLITE_OPEN_READWRITE, NULL);
    if (ret != SQLITE_OK)
    {
        Log::error("ServerLobby", "Cannot open database: %s.",
            sqlite3_errmsg(m_db));
        sqlite3_close(m_db);
        m_db = NULL;
        return;
    }
    sqlite3_busy_handler(m_db, [](void* data, int retry)
        {
            int retry_count = ServerConfig::m_database_timeout / 100;
            if (retry < retry_count)
            {
                sqlite3_sleep(100);
                // Return non-zero to let caller retry again
                return 1;
            }
            // Return zero to let caller return SQLITE_BUSY immediately
            return 0;
        }, NULL);
    sqlite3_create_function(m_db, "insideIPv6CIDR", 2, SQLITE_UTF8, NULL,
        &insideIPv6CIDRSQL, NULL, NULL);
    sqlite3_create_function(m_db, "upperIPv6", 1, SQLITE_UTF8, NULL,
        &upperIPv6SQL, NULL, NULL);
    checkTableExists(ServerConfig::m_ip_ban_table, m_ip_ban_table_exists);
    checkTableExists(ServerConfig::m_ipv6_ban_table, m_ipv6_ban_table_exists);
    checkTableExists(ServerConfig::m_online_id_ban_table,
        m_online_id_ban_table_exists);
    checkTableExists(ServerConfig::m_player_reports_table,
        m_player_reports_table_exists);
    checkTableExists(ServerConfig::m_ip_geolocation_table,
        m_ip_geolocation_table_exists);
    checkTableExists(ServerConfig::m_ipv6_geolocation_table,
        m_ipv6_geolocation_table_exists);
    checkTableExists(ServerConfig::m_permissions_table,
        m_permissions_table_exists);
    checkTableExists(ServerConfig::m_restrictions_table,
        m_restrictions_table_exists);
#endif
}   // initDatabase

//-----------------------------------------------------------------------------
void ServerLobby::initServerStatsTable()
{
#ifdef ENABLE_SQLITE3
    if (!ServerConfig::m_sql_management || !m_db)
        return;
    std::string table_name = std::string("v") +
        StringUtils::toString(ServerConfig::m_server_db_version) + "_" +
        ServerConfig::m_server_uid + "_stats";

    std::ostringstream oss;
    oss << "CREATE TABLE IF NOT EXISTS " << table_name << " (\n"
        "    host_id INTEGER UNSIGNED NOT NULL PRIMARY KEY, -- Unique host id in STKHost of each connection session for a STKPeer\n"
        "    ip INTEGER UNSIGNED NOT NULL, -- IP decimal of host\n";
    if (ServerConfig::m_ipv6_connection)
        oss << "    ipv6 TEXT NOT NULL DEFAULT '', -- IPv6 (if exists) in string of host\n";
    oss << "    port INTEGER UNSIGNED NOT NULL, -- Port of host\n"
        "    online_id INTEGER UNSIGNED NOT NULL, -- Online if of the host (0 for offline account)\n"
        "    username TEXT NOT NULL, -- First player name in the host (if the host has splitscreen player)\n"
        "    player_num INTEGER UNSIGNED NOT NULL, -- Number of player(s) from the host, more than 1 if it has splitscreen player\n"
        "    country_code TEXT NULL DEFAULT NULL, -- 2-letter country code of the host\n"
        "    version TEXT NOT NULL, -- SuperTuxKart version of the host\n"
        "    os TEXT NOT NULL, -- Operating system of the host\n"
        "    connected_time TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, -- Time when connected\n"
        "    disconnected_time TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, -- Time when disconnected (saved when disconnected)\n"
        "    ping INTEGER UNSIGNED NOT NULL DEFAULT 0, -- Ping of the host\n"
        "    packet_loss INTEGER NOT NULL DEFAULT 0 -- Mean packet loss count from ENet (saved when disconnected)\n"
        ") WITHOUT ROWID;";
    std::string query = oss.str();
    sqlite3_stmt* stmt = NULL;
    int ret = sqlite3_prepare_v2(m_db, query.c_str(), -1, &stmt, 0);
    if (ret == SQLITE_OK)
    {
        ret = sqlite3_step(stmt);
        ret = sqlite3_finalize(stmt);
        if (ret == SQLITE_OK)
            m_server_stats_table = table_name;
        else
        {
            Log::error("ServerLobby",
                "Error finalize database for query %s: %s",
                query.c_str(), sqlite3_errmsg(m_db));
        }
    }
    else
    {
        Log::error("ServerLobby", "Error preparing database for query %s: %s",
            query.c_str(), sqlite3_errmsg(m_db));
    }
    if (m_server_stats_table.empty())
        return;

    // Extra default table _countries:
    // Server owner need to initialise this table himself, check NETWORKING.md
    std::string country_table_name = std::string("v") + StringUtils::toString(
        ServerConfig::m_server_db_version) + "_countries";
    query = StringUtils::insertValues(
        "CREATE TABLE IF NOT EXISTS %s (\n"
        "    country_code TEXT NOT NULL PRIMARY KEY UNIQUE, -- Unique 2-letter country code\n"
        "    country_flag TEXT NOT NULL, -- Unicode country flag representation of 2-letter country code\n"
        "    country_name TEXT NOT NULL -- Readable name of this country\n"
        ") WITHOUT ROWID;", country_table_name.c_str());
    easySQLQuery(query);

    // Default views:
    // _full_stats
    // Full stats with ip in human readable format and time played of each
    // players in minutes
    std::string full_stats_view_name = std::string("v") +
        StringUtils::toString(ServerConfig::m_server_db_version) + "_" +
        ServerConfig::m_server_uid + "_full_stats";
    oss.str("");
    oss << "CREATE VIEW IF NOT EXISTS " << full_stats_view_name << " AS\n"
        << "    SELECT host_id, ip,\n"
        << "    ((ip >> 24) & 255) ||'.'|| ((ip >> 16) & 255) ||'.'|| ((ip >>  8) & 255) ||'.'|| ((ip ) & 255) AS ip_readable,\n";
    if (ServerConfig::m_ipv6_connection)
        oss << "    ipv6,";
    oss << "    port, online_id, username, player_num,\n"
        << "    " << m_server_stats_table << ".country_code AS country_code, country_flag, country_name, version, os,\n"
        << "    ROUND((STRFTIME(\"%s\", disconnected_time) - STRFTIME(\"%s\", connected_time)) / 60.0, 2) AS time_played,\n"
        << "    connected_time, disconnected_time, ping, packet_loss FROM " << m_server_stats_table << "\n"
        << "    LEFT JOIN " << country_table_name << " ON "
        <<      country_table_name << ".country_code = " << m_server_stats_table << ".country_code\n"
        << "    ORDER BY connected_time DESC;";
    query = oss.str();
    easySQLQuery(query);

    // _current_players
    // Current players in server with ip in human readable format and time
    // played of each players in minutes
    std::string current_players_view_name = std::string("v") +
        StringUtils::toString(ServerConfig::m_server_db_version) + "_" +
        ServerConfig::m_server_uid + "_current_players";
    oss.str("");
    oss.clear();
    oss << "CREATE VIEW IF NOT EXISTS " << current_players_view_name << " AS\n"
        << "    SELECT host_id, ip,\n"
        << "    ((ip >> 24) & 255) ||'.'|| ((ip >> 16) & 255) ||'.'|| ((ip >>  8) & 255) ||'.'|| ((ip ) & 255) AS ip_readable,\n";
    if (ServerConfig::m_ipv6_connection)
        oss << "    ipv6,";
    oss << "    port, online_id, username, player_num,\n"
        << "    " << m_server_stats_table << ".country_code AS country_code, country_flag, country_name, version, os,\n"
        << "    ROUND((STRFTIME(\"%s\", 'now') - STRFTIME(\"%s\", connected_time)) / 60.0, 2) AS time_played,\n"
        << "    connected_time, ping FROM " << m_server_stats_table << "\n"
        << "    LEFT JOIN " << country_table_name << " ON "
        <<      country_table_name << ".country_code = " << m_server_stats_table << ".country_code\n"
        << "    WHERE connected_time = disconnected_time;";
    query = oss.str();
    easySQLQuery(query);

    // _player_stats
    // All players with online id and username with their time played stats
    // in this server since creation of this database
    // If sqlite supports window functions (since 3.25), it will include last session player info (ip, country, ping...)
    std::string player_stats_view_name = std::string("v") +
        StringUtils::toString(ServerConfig::m_server_db_version) + "_" +
        ServerConfig::m_server_uid + "_player_stats";
    oss.str("");
    oss.clear();
    if (sqlite3_libversion_number() < 3025000)
    {
        oss << "CREATE VIEW IF NOT EXISTS " << player_stats_view_name << " AS\n"
            << "    SELECT online_id, username, COUNT(online_id) AS num_connections,\n"
            << "    MIN(connected_time) AS first_connected_time,\n"
            << "    MAX(connected_time) AS last_connected_time,\n"
            << "    ROUND(SUM((STRFTIME(\"%s\", disconnected_time) - STRFTIME(\"%s\", connected_time)) / 60.0), 2) AS total_time_played,\n"
            << "    ROUND(AVG((STRFTIME(\"%s\", disconnected_time) - STRFTIME(\"%s\", connected_time)) / 60.0), 2) AS average_time_played,\n"
            << "    ROUND(MIN((STRFTIME(\"%s\", disconnected_time) - STRFTIME(\"%s\", connected_time)) / 60.0), 2) AS min_time_played,\n"
            << "    ROUND(MAX((STRFTIME(\"%s\", disconnected_time) - STRFTIME(\"%s\", connected_time)) / 60.0), 2) AS max_time_played\n"
            << "    FROM " << m_server_stats_table << "\n"
            << "    WHERE online_id != 0 GROUP BY online_id ORDER BY num_connections DESC;";
    }
    else
    {
        oss << "CREATE VIEW IF NOT EXISTS " << player_stats_view_name << " AS\n"
            << "    SELECT a.online_id, a.username, a.ip, a.ip_readable,\n";
        if (ServerConfig::m_ipv6_connection)
            oss << "    a.ipv6,";
        oss << "    a.port, a.player_num,\n"
            << "    a.country_code, a.country_flag, a.country_name, a.version, a.os, a.ping, a.packet_loss,\n"
            << "    b.num_connections, b.first_connected_time, b.first_disconnected_time,\n"
            << "    a.connected_time AS last_connected_time, a.disconnected_time AS last_disconnected_time,\n"
            << "    a.time_played AS last_time_played, b.total_time_played, b.average_time_played,\n"
            << "    b.min_time_played, b.max_time_played\n"
            << "    FROM\n"
            << "    (\n"
            << "        SELECT *,\n"
            << "        ROW_NUMBER() OVER\n"
            << "        (\n"
            << "            PARTITION BY online_id\n"
            << "            ORDER BY connected_time DESC\n"
            << "        ) RowNum\n"
            << "        FROM " << full_stats_view_name << " where online_id != 0\n"
            << "    ) as a\n"
            << "    JOIN\n"
            << "    (\n"
            << "        SELECT online_id, COUNT(online_id) AS num_connections,\n"
            << "        MIN(connected_time) AS first_connected_time,\n"
            << "        MIN(disconnected_time) AS first_disconnected_time,\n"
            << "        ROUND(SUM((STRFTIME(\"%s\", disconnected_time) - STRFTIME(\"%s\", connected_time)) / 60.0), 2) AS total_time_played,\n"
            << "        ROUND(AVG((STRFTIME(\"%s\", disconnected_time) - STRFTIME(\"%s\", connected_time)) / 60.0), 2) AS average_time_played,\n"
            << "        ROUND(MIN((STRFTIME(\"%s\", disconnected_time) - STRFTIME(\"%s\", connected_time)) / 60.0), 2) AS min_time_played,\n"
            << "        ROUND(MAX((STRFTIME(\"%s\", disconnected_time) - STRFTIME(\"%s\", connected_time)) / 60.0), 2) AS max_time_played\n"
            << "        FROM " << m_server_stats_table << " WHERE online_id != 0 GROUP BY online_id\n"
            << "    ) AS b\n"
            << "    ON b.online_id = a.online_id\n"
            << "    WHERE RowNum = 1 ORDER BY num_connections DESC;\n";
    }
    query = oss.str();
    easySQLQuery(query);

    uint32_t last_host_id = 0;
    query = StringUtils::insertValues("SELECT MAX(host_id) FROM %s;",
        m_server_stats_table.c_str());
    ret = sqlite3_prepare_v2(m_db, query.c_str(), -1, &stmt, 0);
    if (ret == SQLITE_OK)
    {
        ret = sqlite3_step(stmt);
        if (ret == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL)
        {
            last_host_id = (unsigned)sqlite3_column_int64(stmt, 0);
            Log::info("ServerLobby", "%u was last server session max host id.",
                last_host_id);
        }
        ret = sqlite3_finalize(stmt);
        if (ret != SQLITE_OK)
        {
            Log::error("ServerLobby",
                "Error finalize database for query %s: %s",
                query.c_str(), sqlite3_errmsg(m_db));
            m_server_stats_table = "";
        }
    }
    else
    {
        Log::error("ServerLobby", "Error preparing database for query %s: %s",
            query.c_str(), sqlite3_errmsg(m_db));
        m_server_stats_table = "";
    }
    STKHost::get()->setNextHostId(last_host_id);

    // Update disconnected time (if stk crashed it will not be written)
    query = StringUtils::insertValues(
        "UPDATE %s SET disconnected_time = datetime('now') "
        "WHERE connected_time = disconnected_time;",
        m_server_stats_table.c_str());
    easySQLQuery(query);
#endif
}   // initServerStatsTable

//-----------------------------------------------------------------------------
void ServerLobby::destroyDatabase()
{
#ifdef ENABLE_SQLITE3
    auto peers = STKHost::get()->getPeers();
    for (auto& peer : peers)
        writeDisconnectInfoTable(peer.get());
    if (m_db != NULL)
        sqlite3_close(m_db);
#endif
}   // destroyDatabase

//-----------------------------------------------------------------------------
void ServerLobby::writeDisconnectInfoTable(STKPeer* peer)
{
#ifdef ENABLE_SQLITE3
    if (m_server_stats_table.empty())
        return;
    std::string query = StringUtils::insertValues(
        "UPDATE %s SET disconnected_time = datetime('now'), "
        "ping = %d, packet_loss = %d "
        "WHERE host_id = %u;", m_server_stats_table.c_str(),
        peer->getAveragePing(), peer->getPacketLoss(),
        peer->getHostId());
    easySQLQuery(query);
#endif
}   // writeDisconnectInfoTable

//-----------------------------------------------------------------------------
void ServerLobby::updateAddons()
{
    m_addon_kts.first.clear();
    m_addon_kts.second.clear();
    m_addon_arenas.clear();
    m_addon_soccers.clear();

    std::set<std::string> total_addons;
    for (unsigned i = 0; i < kart_properties_manager->getNumberOfKarts(); i++)
    {
        const KartProperties* kp =
            kart_properties_manager->getKartById(i);
        if (kp->isAddon())
            total_addons.insert(kp->getIdent());
    }
    for (unsigned i = 0; i < track_manager->getNumberOfTracks(); i++)
    {
        const Track* track = track_manager->getTrack(i);
        if (track->isAddon())
            total_addons.insert(track->getIdent());
    }

    for (auto& addon : total_addons)
    {
        const KartProperties* kp = kart_properties_manager->getKart(addon);
        if (kp && kp->isAddon())
        {
            m_addon_kts.first.insert(kp->getIdent());
            continue;
        }
        Track* t = track_manager->getTrack(addon);
        if (!t || !t->isAddon() || t->isInternal())
            continue;
        if (t->isArena())
            m_addon_arenas.insert(t->getIdent());
        else if (t->isSoccer())
            m_addon_soccers.insert(t->getIdent());
        else
            m_addon_kts.second.insert(t->getIdent());
    }

    std::vector<std::string> all_k;
    for (unsigned i = 0; i < kart_properties_manager->getNumberOfKarts(); i++)
    {
        const KartProperties* kp = kart_properties_manager->getKartById(i);
        if (kp->isAddon())
            all_k.push_back(kp->getIdent());
    }
    std::set<std::string> oks = OfficialKarts::getOfficialKarts();
    if (all_k.size() >= 65536 - (unsigned)oks.size())
        all_k.resize(65535 - (unsigned)oks.size());
    for (const std::string& k : oks)
        all_k.push_back(k);
    if (ServerConfig::m_live_players)
        m_available_kts.first = m_official_kts.first;
    else
        m_available_kts.first = { all_k.begin(), all_k.end() };
}   // updateAddons

//-----------------------------------------------------------------------------
/** Called whenever server is reset or game mode is changed.
 */
void ServerLobby::updateTracksForMode()
{
    auto all_t = track_manager->getAllTrackIdentifiers();
    if (all_t.size() >= 65536)
        all_t.resize(65535);
    m_available_kts.second = { all_t.begin(), all_t.end() };
    RaceManager::MinorRaceModeType m =
        ServerConfig::getLocalGameMode(m_game_mode.load()).first;
    switch (m)
    {
        case RaceManager::MINOR_MODE_NORMAL_RACE:
        case RaceManager::MINOR_MODE_TIME_TRIAL:
        case RaceManager::MINOR_MODE_FOLLOW_LEADER:
        {
            auto it = m_available_kts.second.begin();
            while (it != m_available_kts.second.end())
            {
                Track* t =  track_manager->getTrack(*it);
                if (t->isArena() || t->isSoccer() || t->isInternal())
                {
                    it = m_available_kts.second.erase(it);
                }
                else
                    it++;
            }
            break;
        }
        case RaceManager::MINOR_MODE_FREE_FOR_ALL:
        case RaceManager::MINOR_MODE_CAPTURE_THE_FLAG:
        {
            auto it = m_available_kts.second.begin();
            while (it != m_available_kts.second.end())
            {
                Track* t =  track_manager->getTrack(*it);
                if (RaceManager::get()->getMinorMode() ==
                    RaceManager::MINOR_MODE_CAPTURE_THE_FLAG)
                {
                    if (!t->isCTF() || t->isInternal())
                    {
                        it = m_available_kts.second.erase(it);
                    }
                    else
                        it++;
                }
                else
                {
                    if (!t->isArena() ||  t->isInternal())
                    {
                        it = m_available_kts.second.erase(it);
                    }
                    else
                        it++;
                }
            }
            break;
        }
        case RaceManager::MINOR_MODE_SOCCER:
        {
            auto it = m_available_kts.second.begin();
            while (it != m_available_kts.second.end())
            {
                Track* t =  track_manager->getTrack(*it);
                if (!t->isSoccer() || t->isInternal())
                {
                    it = m_available_kts.second.erase(it);
                }
                else
                    it++;
            }
            break;
        }
        default:
            assert(false);
            break;
    }

}   // updateTracksForMode

//-----------------------------------------------------------------------------
void ServerLobby::setup()
{
    LobbyProtocol::setup();
    m_battle_hit_capture_limit = 0;
    m_battle_time_limit = 0.0f;
    m_item_seed = 0;
    m_winner_peer_id = 0;
    m_client_starting_time = 0;
    m_ai_count = 0;
    auto players = STKHost::get()->getPlayersForNewGame();
    if (m_game_setup->isGrandPrix() && !m_game_setup->isGrandPrixStarted())
    {
        for (auto player : players)
            player->resetGrandPrixData();
    }
    if (!m_game_setup->isGrandPrix() || !m_game_setup->isGrandPrixStarted())
    {
        for (auto player : players)
            player->setKartName("");
    }
    if (auto ai = m_ai_peer.lock())
    {
        for (auto player : ai->getPlayerProfiles())
            player->setKartName("");
    }
    for (auto ai : m_ai_profiles)
        ai->setKartName("");

    StateManager::get()->resetActivePlayers();
    // We use maximum 16bit unsigned limit
    auto all_k = kart_properties_manager->getAllAvailableKarts();
    if (all_k.size() >= 65536)
        all_k.resize(65535);
    if (ServerConfig::m_live_players)
        m_available_kts.first = m_official_kts.first;
    else
        m_available_kts.first = { all_k.begin(), all_k.end() };
    NetworkConfig::get()->setTuxHitboxAddon(ServerConfig::m_live_players);
    updateTracksForMode();

    m_server_has_loaded_world.store(false);

    // Initialise the data structures to detect if all clients and 
    // the server are ready:
    resetPeersReady();
    setPoleEnabled(false);
    m_timeout.store(std::numeric_limits<int64_t>::max());
    m_server_started_at = m_server_delay = 0;
    Log::info("ServerLobby", "Resetting the server to its initial state.");
}   // setup

//-----------------------------------------------------------------------------
bool ServerLobby::notifyEvent(Event* event)
{
    assert(m_game_setup); // assert that the setup exists
    if (event->getType() != EVENT_TYPE_MESSAGE)
        return true;

    NetworkString &data = event->data();
    assert(data.size()); // message not empty
    uint8_t message_type;
    message_type = data.getUInt8();
    Log::info("ServerLobby", "Synchronous message of type %d received.",
              message_type);
    switch (message_type)
    {
    case LE_RACE_FINISHED_ACK: playerFinishedResult(event);   break;
    case LE_LIVE_JOIN:         liveJoinRequest(event);        break;
    case LE_CLIENT_LOADED_WORLD: finishedLoadingLiveJoinClient(event); break;
    case LE_KART_INFO: handleKartInfo(event); break;
    case LE_CLIENT_BACK_LOBBY: clientInGameWantsToBackLobby(event); break;
    default: Log::error("ServerLobby", "Unknown message of type %d - ignored.",
                        message_type);
             break;
    }   // switch message_type
    return true;
}   // notifyEvent

//-----------------------------------------------------------------------------
void ServerLobby::handleChat(Event* event)
{
    if (!checkDataSize(event, 1) || !ServerConfig::m_chat) return;

    // Update so that the peer is not kicked
    event->getPeer()->updateLastActivity();
    const bool sender_in_game = event->getPeer()->isWaitingForGame();

    int64_t last_message = event->getPeer()->getLastMessage();
    int64_t elapsed_time = (int64_t)StkTime::getMonoTimeMs() - last_message;

    // Read ServerConfig for formula and details
    if (ServerConfig::m_chat_consecutive_interval > 0 &&
        elapsed_time < ServerConfig::m_chat_consecutive_interval * 1000)
        event->getPeer()->updateConsecutiveMessages(true);
    else
        event->getPeer()->updateConsecutiveMessages(false);

    if (ServerConfig::m_chat_consecutive_interval > 0 &&
        event->getPeer()->getConsecutiveMessages() >
        ServerConfig::m_chat_consecutive_interval / 2)
    {
        NetworkString* chat = getNetworkString();
        chat->setSynchronous(true);
        core::stringw warn = "Spam detected";
        chat->addUInt8(LE_CHAT).encodeString16(warn);
        event->getPeer()->sendPacket(chat, true/*reliable*/);
        delete chat;
        return;
    }

    core::stringw message;
    core::stringw sender_name;
    NetworkPlayerProfile* sender_profile = nullptr;

    event->data().decodeString16(&message, 360/*max_len*/);

    KartTeam target_team = KART_TEAM_NONE;

    // WTF????
    if (event->data().size() > 0)
        target_team = (KartTeam)event->data().getUInt8();

    // determine and verify the name of the sender
    STKPeer* sender = event->getPeer();
    int msg_at = message.find(L": ");
    if (msg_at != -1)
    {
        sender_name = message.subString(0, msg_at);
    }
    else if (!sender->hasPlayerProfiles())
        // Peer cannot send messages without player profiles
        return;
    else
    {
        sender_name = sender->getPlayerProfiles()[0]->getName();
        message = sender_name + L": " + message;
    }

    if (message.size() > 0)
    {
        // Red or blue square emoji
        if (target_team == KART_TEAM_RED)
            message = StringUtils::utf32ToWide({0x1f7e5, 0x20}) + message;
        else if (target_team == KART_TEAM_BLUE)
            message = StringUtils::utf32ToWide({0x1f7e6, 0x20}) + message;

        //teamchat
        //auto can_receive = m_message_receivers[sender];
        bool team_speak = m_team_speakers.find(sender) != m_team_speakers.end();

        // make a function of it for god sake, or at least a macro
        team_speak &= (
            RaceManager::get()->getMinorMode() == RaceManager::MINOR_MODE_SOCCER ||
            RaceManager::get()->getMinorMode() == RaceManager::MINOR_MODE_CAPTURE_THE_FLAG
            );
        std::set<KartTeam> teams;
        for (auto& profile : sender->getPlayerProfiles())
        {
            if (!sender_profile && sender_name == profile->getName())
            {
                sender_profile = profile.get();
            }
            teams.insert(profile->getTeam());
        }

        // check if the sender_profile is authorised to send the message
        if (!sender_profile || sender_profile->hasRestriction(PRF_NOCHAT) ||
                sender_profile->getPermissionLevel() <= PERM_NONE)
        {
            NetworkString* const response = getNetworkString();
            response->setSynchronous(true);
            response->addUInt8(LE_CHAT).encodeString16(L"You are not allowed to send chat messages.");
            sender->sendPacket(response, true/*reliable*/);
            delete response;
            return;
        }

        NetworkString* chat = getNetworkString();
        chat->setSynchronous(true);
        chat->addUInt8(LE_CHAT).encodeString16(message);
        const bool game_started = m_state.load() != WAITING_FOR_START_GAME;

        STKHost::get()->sendPacketToAllPeersWith(
            [game_started, sender_in_game, target_team, sender_name, team_speak, teams, this]
            (STKPeer* p)
            {
                if (game_started)
                {
                    // separates the chat between lobby peers, and ingame players/spectators
                    if (p->isWaitingForGame() != sender_in_game)
                        return false;
#if 0
                    if (p->isWaitingForGame() && !sender_in_game)
                        return false;
                    if (!p->isWaitingForGame() && sender_in_game)
                        return false;
#endif
                    // when targeted towards one team, send it.
                    if (target_team != KART_TEAM_NONE)
                    {
                        if (p->isSpectator())
                            return false;
                        for (auto& player : p->getPlayerProfiles())
                        {
                            if (player->getTeam() == target_team
#if 0
                                || player->getPermissionLevel() >= PERM_MODERATOR
#endif
                                )
                                return true;
                        }
                        return false;
                    }
                }
                // /teamchat restrictions
                if (team_speak)
                {
                    for (auto& profile : p->getPlayerProfiles())
                        if (teams.count(profile->getTeam()) > 0
#if 0
                                || player->getPermissionLevel() >= PERM_MODERATOR
#endif
                            )
                            return true;
                    return false;
                }
                for (auto& peer : m_peers_muted_players)
                {
                    if (auto peer_sp = peer.first.lock())
                    {
                        if (peer_sp.get() == p &&
                            peer.second.find(sender_name) != peer.second.end())
                            return false;
                    }
                }
#if 0
                // incase of no /to
                if (can_receive.empty())
                    return true;
                for (auto& profile : p->getPlayerProfiles())
                {
                    if (can_receive.find(profile->getName()) !=
                        can_receive.end())
                    {
                        return true;
                    }
                }
#endif
                return true;
            }, chat);
            event->getPeer()->updateLastMessage();
        delete chat;
    }
}   // handleChat

//-----------------------------------------------------------------------------
//FIXME: add "force" argument that avoids the check for manual team choosing
void ServerLobby::changeTeam(Event* event)
{
    if (!ServerConfig::m_team_choosing ||
        !RaceManager::get()->teamEnabled())
        return;
    if (!checkDataSize(event, 1)) return;
    NetworkString& data = event->data();
    uint8_t local_id = data.getUInt8();
    auto& player = event->getPeer()->getPlayerProfiles().at(local_id);

    // check if player can change teams
    if (player->hasRestriction(PRF_NOTEAM))
    {
        Log::info("ServerLobby",
                "Player %s tried to change teams without permission.",
                StringUtils::wideToUtf8(player->getName()).c_str());

        NetworkString* const response = getNetworkString();
        response->setSynchronous(true);
        response->addUInt8(LE_CHAT).encodeString16(
                L"You are not allowed to change teams.");
        event->getPeer()->sendPacket(response, true/*reliable*/);
        delete response;
        return;
    }

    auto red_blue = STKHost::get()->getAllPlayersTeamInfo();
    const auto team = player->getTeam();

    // reset pole voting if any
    bool has_pole = false;
    auto peer = event->getPeer();
    auto b = m_blue_pole_votes.find(peer);
    auto r = m_red_pole_votes.find(peer);
    if (b != m_blue_pole_votes.cend())
    {
        m_blue_pole_votes.erase(b);
        has_pole = true;
    }
    if (r != m_red_pole_votes.cend())
    {
        m_red_pole_votes.erase(r);
        has_pole = true;
    }

    if (has_pole)
    {
        core::stringw text = L"Your voting has been reset since you've changed your team."
            L" Please vote again:\n";
        // TODO:
        NetworkString* msg = getNetworkString();
        msg->setSynchronous(true);
        msg->addUInt8(LE_CHAT);
        text += formatTeammateList(
                STKHost::get()->getPlayerProfilesOfTeam(
                    team == KART_TEAM_BLUE ? KART_TEAM_RED : KART_TEAM_BLUE
                    ));
        msg->encodeString16(text);
        peer->sendPacket(msg, true/*reliable*/);
        delete msg;
    }

    // At most 7 players on each team (for live join)
    if (player->getTeam() == KART_TEAM_BLUE)
    {
        if (red_blue.first >= 7)
            return;
        player->setTeam(KART_TEAM_RED);
    }
    else
    {
        if (red_blue.second >= 7)
            return;
        player->setTeam(KART_TEAM_BLUE);
    }
    updatePlayerList();
}   // changeTeam
void ServerLobby::forceChangeTeam(NetworkPlayerProfile* const player, const KartTeam team)
{
    // reset pole voting if any
    bool has_pole = false;
    auto peer = player->getPeer().get();
    auto b = m_blue_pole_votes.find(peer);
    auto r = m_red_pole_votes.find(peer);

    player->setTeam(team);

    if (b != m_blue_pole_votes.cend())
    {
        m_blue_pole_votes.erase(b);
        has_pole = true;
    }
    if (r != m_red_pole_votes.cend())
    {
        m_red_pole_votes.erase(r);
        has_pole = true;
    }

    if (has_pole)
    {
        core::stringw text = L"Your voting has been reset since your team has been changed."
            L" Please vote again:\n";
        // TODO:
        NetworkString* msg = getNetworkString();
        msg->setSynchronous(true);
        msg->addUInt8(LE_CHAT);
        text += formatTeammateList(
                STKHost::get()->getPlayerProfilesOfTeam(
                    team
                    ));
        msg->encodeString16(text);
        peer->sendPacket(msg, true/*reliable*/);
        delete msg;
    }

    updatePlayerList();

}  // forceChangeTeam

//-----------------------------------------------------------------------------
void ServerLobby::kickHost(Event* event)
{
    if (m_server_owner.lock() != event->getPeerSP())
        return;
    if (!checkDataSize(event, 4)) return;
    NetworkString& data = event->data();
    uint32_t host_id = data.getUInt32();
    std::shared_ptr<STKPeer> peer = STKHost::get()->findPeerByHostId(host_id);
    // Ignore kicking ai peer if ai handling is on
    if (peer && (!ServerConfig::m_ai_handling || !peer->isAIPeer()))
        peer->kick();
}   // kickHost

//-----------------------------------------------------------------------------
bool ServerLobby::notifyEventAsynchronous(Event* event)
{
    assert(m_game_setup); // assert that the setup exists
    if (event->getType() == EVENT_TYPE_MESSAGE)
    {
        NetworkString &data = event->data();
        assert(data.size()); // message not empty
        uint8_t message_type;
        message_type = data.getUInt8();
        Log::info("ServerLobby", "Message of type %d received.",
                  message_type);
        switch(message_type)
        {
	// le paquet envoyé au serveur
        case LE_CONNECTION_REQUESTED: connectionRequested(event); break;
        case LE_KART_SELECTION: kartSelectionRequested(event);    break;
        case LE_CLIENT_LOADED_WORLD: finishedLoadingWorldClient(event); break;
        case LE_VOTE: handlePlayerVote(event);                    break;
        case LE_KICK_HOST: kickHost(event);                       break;
        case LE_CHANGE_TEAM: changeTeam(event);                   break;
        case LE_REQUEST_BEGIN: startSelection(event);             break;
        case LE_CHAT: handleChat(event);                          break;
        case LE_CONFIG_SERVER: handleServerConfiguration(event);  break;
        case LE_CHANGE_HANDICAP: changeHandicap(event);           break;
        case LE_CLIENT_BACK_LOBBY:
            clientSelectingAssetsWantsToBackLobby(event);         break;
        case LE_REPORT_PLAYER: writePlayerReport(event);          break;
        case LE_ASSETS_UPDATE:
            handleAssets(event->data(), event->getPeer());        break;
        case LE_COMMAND:
            handleServerCommand(event, event->getPeerSP());       break;
        default:                                                  break;
        }   // switch
    } // if (event->getType() == EVENT_TYPE_MESSAGE)
    else if (event->getType() == EVENT_TYPE_DISCONNECTED)
    {
        clientDisconnected(event);
    } // if (event->getType() == EVENT_TYPE_DISCONNECTED)
    return true;
}   // notifyEventAsynchronous

//-----------------------------------------------------------------------------
#ifdef ENABLE_SQLITE3
/* Every 1 minute STK will poll database:
 * 1. Set disconnected time to now for non-exists host.
 * 2. Clear expired player reports if necessary
 * 3. Kick active peer from ban list
 */
void ServerLobby::pollDatabase()
{
    if (!ServerConfig::m_sql_management || !m_db)
        return;

    if (StkTime::getMonoTimeMs() < m_last_poll_db_time + 60000)
        return;

    m_last_poll_db_time = StkTime::getMonoTimeMs();

    if (m_ip_ban_table_exists)
    {
        std::string query =
            "SELECT ip_start, ip_end, reason, description FROM ";
        query += ServerConfig::m_ip_ban_table;
        query += " WHERE datetime('now') > datetime(starting_time) AND "
            "(expired_days is NULL OR datetime"
            "(starting_time, '+'||expired_days||' days') > datetime('now'));";
        auto peers = STKHost::get()->getPeers();
        sqlite3_exec(m_db, query.c_str(),
            [](void* ptr, int count, char** data, char** columns)
            {
                std::vector<std::shared_ptr<STKPeer> >* peers_ptr =
                    (std::vector<std::shared_ptr<STKPeer> >*)ptr;
                for (std::shared_ptr<STKPeer>& p : *peers_ptr)
                {
                    // IPv4 ban list atm
                    if (p->isAIPeer() || p->getAddress().isIPv6())
                        continue;

                    uint32_t ip_start = 0;
                    uint32_t ip_end = 0;
                    if (!StringUtils::fromString(data[0], ip_start))
                        continue;
                    if (!StringUtils::fromString(data[1], ip_end))
                        continue;
                    uint32_t peer_addr = p->getAddress().getIP();
                    if (ip_start <= peer_addr && ip_end >= peer_addr)
                    {
                        Log::info("ServerLobby",
                            "Kick %s, reason: %s, description: %s",
                            p->getAddress().toString().c_str(),
                            data[2], data[3]);
                        p->kick();
                    }
                }
                return 0;
            }, &peers, NULL);
    }

    if (m_ipv6_ban_table_exists)
    {
        std::string query =
            "SELECT ipv6_cidr, reason, description FROM ";
        query += ServerConfig::m_ipv6_ban_table;
        query += " WHERE datetime('now') > datetime(starting_time) AND "
            "(expired_days is NULL OR datetime"
            "(starting_time, '+'||expired_days||' days') > datetime('now'));";
        auto peers = STKHost::get()->getPeers();
        sqlite3_exec(m_db, query.c_str(),
            [](void* ptr, int count, char** data, char** columns)
            {
                std::vector<std::shared_ptr<STKPeer> >* peers_ptr =
                    (std::vector<std::shared_ptr<STKPeer> >*)ptr;
                for (std::shared_ptr<STKPeer>& p : *peers_ptr)
                {
                    std::string ipv6;
                    if (p->getAddress().isIPv6())
                        ipv6 = p->getAddress().toString(false);
                    // IPv6 ban list atm
                    if (p->isAIPeer() || ipv6.empty())
                        continue;

                    char* ipv6_cidr = data[0];
                    if (insideIPv6CIDR(ipv6_cidr, ipv6.c_str()) == 1)
                    {
                        Log::info("ServerLobby",
                            "Kick %s, reason: %s, description: %s",
                            ipv6.c_str(), data[1], data[2]);
                        p->kick();
                    }
                }
                return 0;
            }, &peers, NULL);
    }

    if (m_online_id_ban_table_exists)
    {
        std::string query = "SELECT online_id, reason, description FROM ";
        query += ServerConfig::m_online_id_ban_table;
        query += " WHERE datetime('now') > datetime(starting_time) AND "
            "(expired_days is NULL OR datetime"
            "(starting_time, '+'||expired_days||' days') > datetime('now'));";
        auto peers = STKHost::get()->getPeers();
        sqlite3_exec(m_db, query.c_str(),
            [](void* ptr, int count, char** data, char** columns)
            {
                std::vector<std::shared_ptr<STKPeer> >* peers_ptr =
                    (std::vector<std::shared_ptr<STKPeer> >*)ptr;
                for (std::shared_ptr<STKPeer>& p : *peers_ptr)
                {
                    if (p->isAIPeer()
                        || p->getPlayerProfiles().empty())
                        continue;

                    uint32_t online_id = 0;
                    if (!StringUtils::fromString(data[0], online_id))
                        continue;
                    if (online_id == p->getPlayerProfiles()[0]->getOnlineId())
                    {
                        Log::info("ServerLobby",
                            "Kick %s, reason: %s, description: %s",
                            p->getAddress().toString().c_str(),
                            data[1], data[2]);
                        p->kick();
                    }
                }
                return 0;
            }, &peers, NULL);
    }

    if (m_player_reports_table_exists &&
        ServerConfig::m_player_reports_expired_days != 0.0f)
    {
        std::string query = StringUtils::insertValues(
            "DELETE FROM %s "
            "WHERE datetime"
            "(reported_time, '+%f days') < datetime('now');",
            ServerConfig::m_player_reports_table.c_str(),
            ServerConfig::m_player_reports_expired_days);
        easySQLQuery(query);
    }
    if (m_server_stats_table.empty())
        return;

    std::string query;
    auto peers = STKHost::get()->getPeers();
    std::vector<uint32_t> exist_hosts;
    if (!peers.empty())
    {
        for (auto& peer : peers)
        {
            if (!peer->isValidated())
                continue;
            exist_hosts.push_back(peer->getHostId());
        }
    }
    if (peers.empty() || exist_hosts.empty())
    {
        query = StringUtils::insertValues(
            "UPDATE %s SET disconnected_time = datetime('now') "
            "WHERE connected_time = disconnected_time;",
            m_server_stats_table.c_str());
    }
    else
    {
        std::ostringstream oss;
        oss << "UPDATE " << m_server_stats_table
            << "    SET disconnected_time = datetime('now')"
            << "    WHERE connected_time = disconnected_time AND"
            << "    host_id NOT IN (";
        for (unsigned i = 0; i < exist_hosts.size(); i++)
        {
            oss << exist_hosts[i];
            if (i != (exist_hosts.size() - 1))
                oss << ",";
        }
        oss << ");";
        query = oss.str();
    }
    easySQLQuery(query);
}   // pollDatabase

//-----------------------------------------------------------------------------
/** Run simple query with write lock waiting and optional function, this
 *  function has no callback for the return (if any) by the query.
 *  Return true if no error occurs
 */
bool ServerLobby::easySQLQuery(const std::string& query,
                   std::function<void(sqlite3_stmt* stmt)> bind_function) const
{
    if (!m_db)
        return false;
    sqlite3_stmt* stmt = NULL;
    int ret = sqlite3_prepare_v2(m_db, query.c_str(), -1, &stmt, 0);
    if (ret == SQLITE_OK)
    {
        if (bind_function)
            bind_function(stmt);
        ret = sqlite3_step(stmt);
        ret = sqlite3_finalize(stmt);
        if (ret != SQLITE_OK)
        {
            Log::error("ServerLobby",
                "Error finalize database for easy query %s: %s",
                query.c_str(), sqlite3_errmsg(m_db));
            return false;
        }
    }
    else
    {
        Log::error("ServerLobby",
            "Error preparing database for easy query %s: %s",
            query.c_str(), sqlite3_errmsg(m_db));
        return false;
    }
    return true;
}   // easySQLQuery

//-----------------------------------------------------------------------------
/* Write true to result if table name exists in database. */
void ServerLobby::checkTableExists(const std::string& table, bool& result)
{
    if (!m_db)
        return;
    sqlite3_stmt* stmt = NULL;
    if (!table.empty())
    {
        std::string query = StringUtils::insertValues(
            "SELECT count(type) FROM sqlite_master "
            "WHERE type='table' AND name='%s';", table.c_str());

        int ret = sqlite3_prepare_v2(m_db, query.c_str(), -1, &stmt, 0);
        if (ret == SQLITE_OK)
        {
            ret = sqlite3_step(stmt);
            if (ret == SQLITE_ROW)
            {
                int number = sqlite3_column_int(stmt, 0);
                if (number == 1)
                {
                    Log::info("ServerLobby", "Table named %s will used.",
                        table.c_str());
                    result = true;
                }
            }
            ret = sqlite3_finalize(stmt);
            if (ret != SQLITE_OK)
            {
                Log::error("ServerLobby",
                    "Error finalize database for query %s: %s",
                    query.c_str(), sqlite3_errmsg(m_db));
            }
        }
    }
    if (!result && !table.empty())
    {
        Log::warn("ServerLobby", "Table named %s not found in database.",
            table.c_str());
    }
}   // checkTableExists

//-----------------------------------------------------------------------------
std::string ServerLobby::ip2Country(const SocketAddress& addr) const
{
    if (!m_db || !m_ip_geolocation_table_exists || addr.isLAN())
        return "";

    std::string cc_code;
    std::string query = StringUtils::insertValues(
        "SELECT country_code FROM %s "
        "WHERE `ip_start` <= %d AND `ip_end` >= %d "
        "ORDER BY `ip_start` DESC LIMIT 1;",
        ServerConfig::m_ip_geolocation_table.c_str(), addr.getIP(),
        addr.getIP());

    sqlite3_stmt* stmt = NULL;
    int ret = sqlite3_prepare_v2(m_db, query.c_str(), -1, &stmt, 0);
    if (ret == SQLITE_OK)
    {
        ret = sqlite3_step(stmt);
        if (ret == SQLITE_ROW)
        {
            const char* country_code = (char*)sqlite3_column_text(stmt, 0);
            cc_code = country_code;
        }
        ret = sqlite3_finalize(stmt);
        if (ret != SQLITE_OK)
        {
            Log::error("ServerLobby",
                "Error finalize database for query %s: %s",
                query.c_str(), sqlite3_errmsg(m_db));
        }
    }
    else
    {
        Log::error("ServerLobby", "Error preparing database for query %s: %s",
            query.c_str(), sqlite3_errmsg(m_db));
        return "";
    }
    return cc_code;
}   // ip2Country

//-----------------------------------------------------------------------------
std::string ServerLobby::ipv62Country(const SocketAddress& addr) const
{
    if (!m_db || !m_ipv6_geolocation_table_exists)
        return "";

    std::string cc_code;
    const std::string& ipv6 = addr.toString(false/*show_port*/);
    std::string query = StringUtils::insertValues(
        "SELECT country_code FROM %s "
        "WHERE `ip_start` <= upperIPv6(\"%s\") AND `ip_end` >= upperIPv6(\"%s\") "
        "ORDER BY `ip_start` DESC LIMIT 1;",
        ServerConfig::m_ipv6_geolocation_table.c_str(), ipv6.c_str(),
        ipv6.c_str());

    sqlite3_stmt* stmt = NULL;
    int ret = sqlite3_prepare_v2(m_db, query.c_str(), -1, &stmt, 0);
    if (ret == SQLITE_OK)
    {
        ret = sqlite3_step(stmt);
        if (ret == SQLITE_ROW)
        {
            const char* country_code = (char*)sqlite3_column_text(stmt, 0);
            cc_code = country_code;
        }
        ret = sqlite3_finalize(stmt);
        if (ret != SQLITE_OK)
        {
            Log::error("ServerLobby",
                "Error finalize database for query %s: %s",
                query.c_str(), sqlite3_errmsg(m_db));
        }
    }
    else
    {
        Log::error("ServerLobby", "Error preparing database for query %s: %s",
            query.c_str(), sqlite3_errmsg(m_db));
        return "";
    }
    return cc_code;
}   // ipv62Country

#endif

//-----------------------------------------------------------------------------
void ServerLobby::writePlayerReport(Event* event)
{
#ifdef ENABLE_SQLITE3
    if (!m_db || !m_player_reports_table_exists)
        return;
    STKPeer* reporter = event->getPeer();
    if (!reporter->hasPlayerProfiles())
        return;
    auto reporter_npp = reporter->getPlayerProfiles()[0];

    uint32_t reporting_host_id = event->data().getUInt32();
    core::stringw info;
    event->data().decodeString16(&info);
    if (info.empty())
        return;

    auto reporting_peer = STKHost::get()->findPeerByHostId(reporting_host_id);
    if (!reporting_peer || !reporting_peer->hasPlayerProfiles())
        return;
    auto reporting_npp = reporting_peer->getPlayerProfiles()[0];

    std::string query;
    if (ServerConfig::m_ipv6_connection)
    {
        query = StringUtils::insertValues(
            "INSERT INTO %s "
            "(server_uid, reporter_ip, reporter_ipv6, reporter_online_id, reporter_username, "
            "info, reporting_ip, reporting_ipv6, reporting_online_id, reporting_username) "
            "VALUES (?, %u, \"%s\", %u, ?, ?, %u, \"%s\", %u, ?);",
            ServerConfig::m_player_reports_table.c_str(),
            !reporter->getAddress().isIPv6() ? reporter->getAddress().getIP() : 0,
            reporter->getAddress().isIPv6() ? reporter->getAddress().toString(false) : "",
            reporter_npp->getOnlineId(),
            !reporting_peer->getAddress().isIPv6() ? reporting_peer->getAddress().getIP() : 0,
            reporting_peer->getAddress().isIPv6() ? reporting_peer->getAddress().toString(false) : "",
            reporting_npp->getOnlineId());
    }
    else
    {
        query = StringUtils::insertValues(
            "INSERT INTO %s "
            "(server_uid, reporter_ip, reporter_online_id, reporter_username, "
            "info, reporting_ip, reporting_online_id, reporting_username) "
            "VALUES (?, %u, %u, ?, ?, %u, %u, ?);",
            ServerConfig::m_player_reports_table.c_str(),
            reporter->getAddress().getIP(), reporter_npp->getOnlineId(),
            reporting_peer->getAddress().getIP(), reporting_npp->getOnlineId());
    }
    bool written = easySQLQuery(query,
        [reporter_npp, reporting_npp, info](sqlite3_stmt* stmt)
        {
            // SQLITE_TRANSIENT to copy string
            if (sqlite3_bind_text(stmt, 1, ServerConfig::m_server_uid.c_str(),
                -1, SQLITE_TRANSIENT) != SQLITE_OK)
            {
                Log::error("easySQLQuery", "Failed to bind %s.",
                    ServerConfig::m_server_uid.c_str());
            }
            if (sqlite3_bind_text(stmt, 2,
                StringUtils::wideToUtf8(reporter_npp->getName()).c_str(),
                -1, SQLITE_TRANSIENT) != SQLITE_OK)
            {
                Log::error("easySQLQuery", "Failed to bind %s.",
                    StringUtils::wideToUtf8(reporter_npp->getName()).c_str());
            }
            if (sqlite3_bind_text(stmt, 3,
                StringUtils::wideToUtf8(info).c_str(),
                -1, SQLITE_TRANSIENT) != SQLITE_OK)
            {
                Log::error("easySQLQuery", "Failed to bind %s.",
                    StringUtils::wideToUtf8(info).c_str());
            }
            if (sqlite3_bind_text(stmt, 4,
                StringUtils::wideToUtf8(reporting_npp->getName()).c_str(),
                -1, SQLITE_TRANSIENT) != SQLITE_OK)
            {
                Log::error("easySQLQuery", "Failed to bind %s.",
                    StringUtils::wideToUtf8(reporting_npp->getName()).c_str());
            }
        });
    if (written)
    {
        NetworkString* success = getNetworkString();
        success->setSynchronous(true);
        success->addUInt8(LE_REPORT_PLAYER).addUInt8(1)
            .encodeString(reporting_npp->getName());
        event->getPeer()->sendPacket(success, true/*reliable*/);
        delete success;
    }
#endif
}   // writePlayerReport

void ServerLobby::sendWANListToPeer(std::shared_ptr<STKPeer> peer)
{
    core::stringw responseMsg = L"[Currently played servers]\n";
    std::lock_guard<std::mutex> wr_lock(m_wanrefresh_lock);
    std::shared_ptr<STKPeer> requester = m_last_wanrefresh_requester.lock();
    // send a message to whoever requested the message
    NetworkString* response = getNetworkString();
    response->setSynchronous(true);
    response->addUInt8(LE_CHAT);

    for (std::shared_ptr<Server> serverPtr : m_last_wanrefresh_res->m_servers)
    {
        auto players = serverPtr->getPlayers();
        if (players.empty() || serverPtr->isPasswordProtected())
            continue;

        RaceManager::MinorRaceModeType m =
            ServerConfig::getLocalGameMode(serverPtr->getServerMode()).first;

        responseMsg += StringUtils::getCountryFlag(serverPtr->getCountryCode());
        responseMsg += serverPtr->getName();
        responseMsg += L" (";
        responseMsg += players.size();
        responseMsg += L"/";
        responseMsg += serverPtr->getMaxPlayers();
        responseMsg += L"), ";
        responseMsg += StringUtils::utf8ToWide(
                RaceManager::getIdentOf(m));
        responseMsg += L", ";
        if (serverPtr->isGameStarted())
        {
            Track* track = serverPtr->getCurrentTrack();
            if (track)
                responseMsg += StringUtils::utf8ToWide(track->getIdent());
            else
                responseMsg += L"(unknown track)";
        }
        else {
            responseMsg += L"(waiting for game)";
        }
        responseMsg += L"\n";
        
        //player list
        for (auto player : players)
        {
            responseMsg += std::get<1>(player);
            responseMsg += L" | ";
        }
        responseMsg += L"\n\n";
    }

    response->encodeString16(responseMsg);
    peer->sendPacket(response);
    delete response;
}
//-----------------------------------------------------------------------------
/** Find out the public IP server or poll STK server asynchronously. */
void ServerLobby::asynchronousUpdate()
{
    if (m_rs_state.load() == RS_ASYNC_RESET)
    {
        resetVotingTime();
        resetServer();
        m_rs_state.store(RS_NONE);
    }

    for (auto it = m_peers_muted_players.begin();
        it != m_peers_muted_players.end();)
    {
        if (it->first.expired())
            it = m_peers_muted_players.erase(it);
        else
            it++;
    }

#ifdef ENABLE_SQLITE3
    pollDatabase();
#endif

    // Check if server owner has left
    updateServerOwner();

    if (ServerConfig::m_ranked && m_state.load() == WAITING_FOR_START_GAME)
        clearDisconnectedRankedPlayer();

    if (allowJoinedPlayersWaiting() || (m_game_setup->isGrandPrix() &&
        m_state.load() == WAITING_FOR_START_GAME))
    {
        // Only poll the STK server if server has been registered.
        if (m_server_id_online.load() != 0 &&
            m_state.load() != REGISTER_SELF_ADDRESS)
            checkIncomingConnectionRequests();
        handlePendingConnection();
    }

    if (m_server_id_online.load() != 0 &&
        allowJoinedPlayersWaiting() &&
        StkTime::getMonoTimeMs() > m_last_unsuccess_poll_time &&
        StkTime::getMonoTimeMs() > m_last_success_poll_time.load() + 30000)
    {
        Log::warn("ServerLobby", "Trying auto server recovery.");
        // For auto server recovery wait 3 seconds for next try
        m_last_unsuccess_poll_time = StkTime::getMonoTimeMs() + 3000;
        registerServer(false/*first_time*/);
    }

    // Respond to asynchronous events
    if (m_last_wanrefresh_res && m_last_wanrefresh_res->m_servers.size() &&
            m_last_wanrefresh_res->m_list_updated.load() &&
            !m_last_wanrefresh_requester.expired())
    {
        sendWANListToPeer(m_last_wanrefresh_requester.lock());
        m_last_wanrefresh_res->m_list_updated.store(false);
    }

    switch (m_state.load())
    {
    case SET_PUBLIC_ADDRESS:
    {
        // In case of LAN we don't need our public address or register with the
        // STK server, so we can directly go to the accepting clients state.
        if (NetworkConfig::get()->isLAN())
        {
            m_state = WAITING_FOR_START_GAME;
            updatePlayerList();
            STKHost::get()->startListening();
            return;
        }
        auto ip_type = NetworkConfig::get()->getIPType();
        // Set the IPv6 address first for possible IPv6 only server
        if (isIPv6Socket() && ip_type >= NetworkConfig::IP_V6)
        {
            STKHost::get()->setPublicAddress(AF_INET6);
        }
        if (ip_type == NetworkConfig::IP_V4 ||
            ip_type == NetworkConfig::IP_DUAL_STACK)
        {
            STKHost::get()->setPublicAddress(AF_INET);
        }
        if (STKHost::get()->getPublicAddress().isUnset() &&
            STKHost::get()->getPublicIPv6Address().empty())
        {
            m_state = ERROR_LEAVE;
        }
        else
        {
            STKHost::get()->startListening();
            m_state = REGISTER_SELF_ADDRESS;
        }
        break;
    }
    case REGISTER_SELF_ADDRESS:
    {
        if (m_game_setup->isGrandPrixStarted() || m_registered_for_once_only)
        {
            m_state = WAITING_FOR_START_GAME;
            updatePlayerList();
            break;
        }
        // Register this server with the STK server. This will block
        // this thread, because there is no need for the protocol manager
        // to react to any requests before the server is registered.
        if (m_server_registering.expired() && m_server_id_online.load() == 0)
            registerServer(true/*first_time*/);

        if (m_server_registering.expired())
        {
            // Finished registering server
            if (m_server_id_online.load() != 0)
            {
                // For non grand prix server we only need to register to stk
                // addons once
                if (allowJoinedPlayersWaiting())
                    m_registered_for_once_only = true;
                m_state = WAITING_FOR_START_GAME;
                updatePlayerList();
            }
        }
        break;
    }
    case WAITING_FOR_START_GAME:
    {
        if (ServerConfig::m_owner_less)
        {
            unsigned players = 0;
            STKHost::get()->updatePlayers(&players);

            if (((int)players >= ServerConfig::m_min_start_game_players ||
                m_game_setup->isGrandPrixStarted()) &&
                m_timeout.load() == std::numeric_limits<int64_t>::max())
            {
                m_timeout.store((int64_t)StkTime::getMonoTimeMs() +
                    (int64_t)
                    (ServerConfig::m_start_game_counter * 1000.0f));
            }
            else if ((int)players < ServerConfig::m_min_start_game_players &&
                !m_game_setup->isGrandPrixStarted())
            {
                resetPeersReady();
                if (m_timeout.load() != std::numeric_limits<int64_t>::max())
                    updatePlayerList();
                m_timeout.store(std::numeric_limits<int64_t>::max());
            }

	    const char all_ready_play = checkPeersCanPlayAndReady(true/*ignore_ai_peer*/);
            if (((m_timeout.load() < (int64_t)StkTime::getMonoTimeMs()) &&
	 	  (all_ready_play&2)) ||
                ((all_ready_play==3) &&
                (int)players >= ServerConfig::m_min_start_game_players))
            {
                resetPeersReady();
                startSelection();
                return;
            }
        }
        // You can implement anything of interesting when there is a crowned player
        break;
    }
    case ERROR_LEAVE:
    {
        requestTerminate();
        m_state = EXITING;
        STKHost::get()->requestShutdown();
        break;
    }
    case WAIT_FOR_WORLD_LOADED:
    {
        // For WAIT_FOR_WORLD_LOADED and SELECTING make sure there are enough
        // players to start next game, otherwise exiting and let main thread
        // reset
        if (m_end_voting_period.load() == 0)
            return;

        unsigned player_in_game = 0;
        STKHost::get()->updatePlayers(&player_in_game);
        // Reset lobby will be done in main thread
        if ((player_in_game == 1 && ServerConfig::m_ranked) ||
            player_in_game == 0)
        {
            resetVotingTime();
            return;
        }

        // m_server_has_loaded_world is set by main thread with atomic write
        if (m_server_has_loaded_world.load() == false)
            return;
        if (!checkPeersReady(
            ServerConfig::m_ai_handling && m_ai_count == 0/*ignore_ai_peer*/))
            return;
        // Reset for next state usage
        resetPeersReady();
        configPeersStartTime();
        break;
    }
    case SELECTING:
    {
        if (m_end_voting_period.load() == 0)
            return;
        unsigned player_in_game = 0;
        STKHost::get()->updatePlayers(&player_in_game);
        if ((player_in_game == 1 && ServerConfig::m_ranked) ||
            player_in_game == 0)
        {
            resetVotingTime();
            return;
        }

        PeerVote winner_vote;
        m_winner_peer_id = std::numeric_limits<uint32_t>::max();
        bool go_on_race = false;
        if (ServerConfig::m_track_voting)
            go_on_race = handleAllVotes(&winner_vote, &m_winner_peer_id);
        else if (m_game_setup->isGrandPrixStarted() || isVotingOver())
        {
            winner_vote = *m_default_vote;
            go_on_race = true;
        }
        if (go_on_race)
        {
            // pole
            if (isPoleEnabled() && (!m_red_pole_votes.empty() || !m_blue_pole_votes.empty()))
            {
                auto pole = decidePoles();

                //RaceManager::get()->setBluePole(pole.first);
                //RaceManager::get()->setRedPole(pole.second);

                if (pole.first && pole.first->getTeam() == KART_TEAM_BLUE)
                {
                    STKHost::get()->setForcedSecondPlayer(pole.first);
                    announcePoleFor(pole.first, KART_TEAM_BLUE);
                    Log::info("ServerLobby", "Pole player for team blue is %s.",
                            StringUtils::wideToUtf8(pole.first->getName()).c_str());
                }
                else
                    Log::info("ServerLobby", "No pole player for first pos.");

                if (pole.second && pole.second->getTeam() == KART_TEAM_RED)
                {
                    STKHost::get()->setForcedFirstPlayer(pole.second);
                    announcePoleFor(pole.second, KART_TEAM_RED);
                    Log::info("ServerLobby", "Pole player for team red is %s.",
                            StringUtils::wideToUtf8(pole.second->getName()).c_str());
                }
                else
                    Log::info("ServerLobby", "No pole player for second pos.");
            }
            *m_default_vote = winner_vote;
            m_item_seed = (uint32_t)StkTime::getTimeSinceEpoch();
            ItemManager::updateRandomSeed(m_item_seed);
            m_game_setup->setRace(winner_vote);
            bool has_always_on_spectators = false;
            auto players = STKHost::get()
                ->getPlayersForNewGame(&has_always_on_spectators);

            if (players.size() > 0)
            {
                auto player1 = players[0];
            }
            if (players.size() > 1)
            {
                auto player2 = players[1];
            }


            auto ai_instance = m_ai_peer.lock();
            if (supportsAI())
            {
                if (ai_instance)
                {
                    auto ai_profiles = ai_instance->getPlayerProfiles();
                    if (m_ai_count > 0)
                    {
                        ai_profiles.resize(m_ai_count);
                        players.insert(players.end(), ai_profiles.begin(),
                            ai_profiles.end());
                    }
                }
                else if (!m_ai_profiles.empty())
                {
                    players.insert(players.end(), m_ai_profiles.begin(),
                        m_ai_profiles.end());
                }
            }
            m_game_setup->sortPlayersForGrandPrix(players);
            m_game_setup->sortPlayersForGame(players, 0/*ignoreLeading*/, !isPoleEnabled()/*shuffle*/);

            if (players.size() > 0)
            {
                auto player1 = players[0];
            }
            if (players.size() > 1)
            {
                auto player2 = players[1];
            }

            // Add placeholder players for live join
            addLiveJoinPlaceholder(players);

            for (unsigned i = 0; i < players.size(); i++)
            {
                std::shared_ptr<NetworkPlayerProfile>& player = players[i];
                std::shared_ptr<STKPeer> peer = player->getPeer();
                if (peer)
                    peer->clearAvailableKartIDs();
            }
            for (unsigned i = 0; i < players.size(); i++)
            {
                std::shared_ptr<NetworkPlayerProfile>& player = players[i];
                std::shared_ptr<STKPeer> peer = player->getPeer();
                if (peer)
                    peer->addAvailableKartID(i);
            }
            getHitCaptureLimit();

            // If player chose random / hasn't chose any kart
            for (unsigned i = 0; i < players.size(); i++)
            {
                if (players[i]->getKartName().empty())
                {
                    RandomGenerator rg;
                    std::set<std::string>::iterator it =
                        m_available_kts.first.begin();
                    std::advance(it,
                        rg.get((int)m_available_kts.first.size()));
                    players[i]->setKartName(*it);
                }
            }

            if (players.size() > 0)
            {
                auto player1 = players[0];
            }
            if (players.size() > 1)
            {
                auto player2 = players[1];
            }

            NetworkString* load_world_message = getLoadWorldMessage(players,
                false/*live_join*/);
            m_game_setup->setHitCaptureTime(m_battle_hit_capture_limit,
                m_battle_time_limit);
            uint16_t flag_return_time = (uint16_t)stk_config->time2Ticks(
                ServerConfig::m_flag_return_timeout);
            RaceManager::get()->setFlagReturnTicks(flag_return_time);
            uint16_t flag_deactivated_time = (uint16_t)stk_config->time2Ticks(
                ServerConfig::m_flag_deactivated_time);
            RaceManager::get()->setFlagDeactivatedTicks(flag_deactivated_time);
            configRemoteKart(players, 0);
           
            std::string log_msg;
            if(ServerConfig::m_soccer_log)
            {
                log_msg = "Addon: " + winner_vote.m_track_name;
                GlobalLog::writeLog(log_msg + "\n", GlobalLogTypes::POS_LOG);
                Log::info("AddonLog",winner_vote.m_track_name.c_str());
            }

            // Reset for next state usage
            resetPeersReady();
            m_state = LOAD_WORLD;
            sendMessageToPeers(load_world_message);
            // updatePlayerList so the in lobby players (if any) can see always
            // spectators join the game
            if (has_always_on_spectators)
                updatePlayerList();
            delete load_world_message;
        }
        break;
    }
    default:
        break;
    }

}   // asynchronousUpdate

//-----------------------------------------------------------------------------
void ServerLobby::encodePlayers(BareNetworkString* bns,
        std::vector<std::shared_ptr<NetworkPlayerProfile> >& players) const
{
    bns->addUInt8((uint8_t)players.size());
    for (unsigned i = 0; i < players.size(); i++)
    {
        std::shared_ptr<NetworkPlayerProfile>& player = players[i];
        bns->encodeString(player->getName())
            .addUInt32(player->getHostId())
            .addFloat(player->getDefaultKartColor())
            .addUInt32(player->getOnlineId())
            .addUInt8(player->getHandicap())
            .addUInt8(player->getLocalPlayerId())
            .addUInt8(
            RaceManager::get()->teamEnabled() ? player->getTeam() : KART_TEAM_NONE)
            .encodeString(player->getCountryCode());
        bns->encodeString(player->getKartName());
    }
}   // encodePlayers

//-----------------------------------------------------------------------------
NetworkString* ServerLobby::getLoadWorldMessage(
    std::vector<std::shared_ptr<NetworkPlayerProfile> >& players,
    bool live_join) const
{
    NetworkString* load_world_message = getNetworkString();
    load_world_message->setSynchronous(true);
    load_world_message->addUInt8(LE_LOAD_WORLD);
    load_world_message->addUInt32(m_winner_peer_id);
    m_default_vote->encode(load_world_message);
    load_world_message->addUInt8(live_join ? 1 : 0);
    encodePlayers(load_world_message, players);
    load_world_message->addUInt32(m_item_seed);
    if (RaceManager::get()->isBattleMode())
    {
        load_world_message->addUInt32(m_battle_hit_capture_limit)
            .addFloat(m_battle_time_limit);
        uint16_t flag_return_time = (uint16_t)stk_config->time2Ticks(
            ServerConfig::m_flag_return_timeout);
        load_world_message->addUInt16(flag_return_time);
        uint16_t flag_deactivated_time = (uint16_t)stk_config->time2Ticks(
            ServerConfig::m_flag_deactivated_time);
        load_world_message->addUInt16(flag_deactivated_time);
    }
    for (unsigned i = 0; i < players.size(); i++)
        players[i]->getKartData().encode(load_world_message);
    return load_world_message;
}   // getLoadWorldMessage

//-----------------------------------------------------------------------------
/** Returns true if server can be live joined or spectating
 */
bool ServerLobby::canLiveJoinNow() const
{
    bool live_join = ServerConfig::m_live_players && worldIsActive();
    if (!live_join)
        return false;
    if (RaceManager::get()->modeHasLaps())
    {
        // No spectate when fastest kart is nearly finish, because if there
        // is endcontroller the spectating remote may not be knowing this
        // on time
        LinearWorld* w = dynamic_cast<LinearWorld*>(World::getWorld());
        if (!w)
            return false;
        AbstractKart* fastest_kart = NULL;
        for (unsigned i = 0; i < w->getNumKarts(); i++)
        {
            fastest_kart = w->getKartAtPosition(i + 1);
            if (fastest_kart && !fastest_kart->isEliminated())
                break;
        }
        if (!fastest_kart)
            return false;
        float progress = w->getOverallDistance(
            fastest_kart->getWorldKartId()) /
            (Track::getCurrentTrack()->getTrackLength() *
            (float)RaceManager::get()->getNumLaps());
        if (progress > 0.9f)
            return false;
    }
    return live_join;
}   // canLiveJoinNow

//-----------------------------------------------------------------------------
/** Returns true if world is active for clients to live join, spectate or
 *  going back to lobby live
 */
bool ServerLobby::worldIsActive() const
{
    return World::getWorld() && RaceEventManager::get()->isRunning() &&
        !RaceEventManager::get()->isRaceOver() &&
        World::getWorld()->getPhase() == WorldStatus::RACE_PHASE;
}   // worldIsActive

//-----------------------------------------------------------------------------
/** \ref STKPeer peer will be reset back to the lobby with reason
 *  \ref BackLobbyReason blr
 */
void ServerLobby::rejectLiveJoin(STKPeer* peer, BackLobbyReason blr)
{
    NetworkString* reset = getNetworkString(2);
    reset->setSynchronous(true);
    reset->addUInt8(LE_BACK_LOBBY).addUInt8(blr);
    peer->sendPacket(reset, /*reliable*/true);
    delete reset;
    updatePlayerList();
    NetworkString* server_info = getNetworkString();
    server_info->setSynchronous(true);
    server_info->addUInt8(LE_SERVER_INFO);
    m_game_setup->addServerInfo(server_info);
    peer->sendPacket(server_info, /*reliable*/true);
    delete server_info;

    // everywhere where addServerInfo is used, sendRandomInstalladdonLine be used too.
    sendRandomInstalladdonLine(peer);
    sendCurrentModifiers(peer);
}   // rejectLiveJoin

//-----------------------------------------------------------------------------
/** This message is like kartSelectionRequested, but it will send the peer
 *  load world message if he can join the current started game.
 */
void ServerLobby::liveJoinRequest(Event* event)
{
    STKPeer* peer = event->getPeer();
    const NetworkString& data = event->data();

    if (!canLiveJoinNow())
    {
        rejectLiveJoin(peer, BLR_NO_GAME_FOR_LIVE_JOIN);
        return;
    }
    bool spectator = data.getUInt8() == 1;
    if (RaceManager::get()->modeHasLaps() && !spectator)
    {
        // No live join for linear race
        rejectLiveJoin(peer, BLR_NO_GAME_FOR_LIVE_JOIN);
        return;
    }

    for (auto player : peer->getPlayerProfiles())
    {
        if (spectator && (player->hasRestriction(PRF_NOSPEC) ||
                    (player->getPermissionLevel() < PERM_SPECTATOR)))
        {
            NetworkString* const response = getNetworkString();
            response->setSynchronous(true);
            response->addUInt8(LE_CHAT).encodeString16(
                L"You are not allowed to spectate.");
            rejectLiveJoin(peer, BLR_NO_PLACE_FOR_LIVE_JOIN);
            peer->sendPacket(response, true/*reliable*/);
            delete response;
            return;
        }
        else if (!spectator && (player->hasRestriction(PRF_NOGAME) || (
                        player->getPermissionLevel() < PERM_PLAYER)))
        {
            NetworkString* const response = getNetworkString();
            response->setSynchronous(true);
            response->addUInt8(LE_CHAT).encodeString16(
                L"You are not allowed to participate in the game.");
            rejectLiveJoin(peer, BLR_NO_PLACE_FOR_LIVE_JOIN);
            peer->sendPacket(response, true/*reliable*/);
            delete response;
            return;
        }
    }

    peer->clearAvailableKartIDs();
    if (!spectator)
    {
        auto spectators_by_limit = getSpectatorsByLimit();
        setPlayerKarts(data, peer);

        std::vector<int> used_id;
        for (unsigned i = 0; i < peer->getPlayerProfiles().size(); i++)
        {
            int id = getReservedId(peer->getPlayerProfiles()[i], i);
            if (id == -1)
                break;
            used_id.push_back(id);
        }
        if ((used_id.size() != peer->getPlayerProfiles().size()) ||
            (spectators_by_limit.find(event->getPeerSP()) != spectators_by_limit.end()))
        {
            for (unsigned i = 0; i < peer->getPlayerProfiles().size(); i++)
                peer->getPlayerProfiles()[i]->setKartName("");
            for (unsigned i = 0; i < used_id.size(); i++)
            {
                RemoteKartInfo& rki = RaceManager::get()->getKartInfo(used_id[i]);
                rki.makeReserved();
            }
            Log::info("ServerLobby", "Too many players (%d) try to live join",
                (int)peer->getPlayerProfiles().size());
            rejectLiveJoin(peer, BLR_NO_PLACE_FOR_LIVE_JOIN);
            return;
        }

        for (int id : used_id)
        {
            Log::info("ServerLobby", "%s live joining with reserved kart id %d.",
                peer->getAddress().toString().c_str(), id);
            peer->addAvailableKartID(id);
        }
    }
    else
    {
        Log::info("ServerLobby", "%s spectating now.",
            peer->getAddress().toString().c_str());
    }

    std::vector<std::shared_ptr<NetworkPlayerProfile> > players =
        getLivePlayers();
    NetworkString* load_world_message = getLoadWorldMessage(players,
        true/*live_join*/);
    peer->sendPacket(load_world_message, true/*reliable*/);
    delete load_world_message;
    peer->updateLastActivity();
}   // liveJoinRequest

//-----------------------------------------------------------------------------
/** Get a list of current ingame players for live join or spectate.
 */
std::vector<std::shared_ptr<NetworkPlayerProfile> >
                                            ServerLobby::getLivePlayers() const
{
    std::vector<std::shared_ptr<NetworkPlayerProfile> > players;
    for (unsigned i = 0; i < RaceManager::get()->getNumPlayers(); i++)
    {
        const RemoteKartInfo& rki = RaceManager::get()->getKartInfo(i);
        std::shared_ptr<NetworkPlayerProfile> player =
            rki.getNetworkPlayerProfile().lock();
        if (!player)
        {
            if (RaceManager::get()->modeHasLaps())
            {
                player = std::make_shared<NetworkPlayerProfile>(
                    nullptr, rki.getPlayerName(),
                    std::numeric_limits<uint32_t>::max(),
                    rki.getDefaultKartColor(),
                    rki.getOnlineId(), rki.getHandicap(),
                    rki.getLocalPlayerId(), KART_TEAM_NONE,
                    rki.getCountryCode());
                player->setKartName(rki.getKartName());
            }
            else
            {
                player = NetworkPlayerProfile::getReservedProfile(
                    RaceManager::get()->getMinorMode() ==
                    RaceManager::MINOR_MODE_FREE_FOR_ALL ?
                    KART_TEAM_NONE : rki.getKartTeam());
            }
        }
        players.push_back(player);
    }
    return players;
}   // getLivePlayers

//-----------------------------------------------------------------------------
/** Decide where to put the live join player depends on his team and game mode.
 */
int ServerLobby::getReservedId(std::shared_ptr<NetworkPlayerProfile>& p,
                               unsigned local_id) const
{
    const bool is_ffa =
        RaceManager::get()->getMinorMode() == RaceManager::MINOR_MODE_FREE_FOR_ALL;
    int red_count = 0;
    int blue_count = 0;
    for (unsigned i = 0; i < RaceManager::get()->getNumPlayers(); i++)
    {
        RemoteKartInfo& rki = RaceManager::get()->getKartInfo(i);
        if (rki.isReserved())
            continue;
        bool disconnected = rki.disconnected();
        if (RaceManager::get()->getKartInfo(i).getKartTeam() == KART_TEAM_RED &&
            !disconnected)
            red_count++;
        else if (RaceManager::get()->getKartInfo(i).getKartTeam() ==
            KART_TEAM_BLUE && !disconnected)
            blue_count++;
    }
    KartTeam target_team = red_count > blue_count ? KART_TEAM_BLUE :
        KART_TEAM_RED;

    for (unsigned i = 0; i < RaceManager::get()->getNumPlayers(); i++)
    {
        RemoteKartInfo& rki = RaceManager::get()->getKartInfo(i);
        std::shared_ptr<NetworkPlayerProfile> player =
            rki.getNetworkPlayerProfile().lock();
        if (!player)
        {
            if (is_ffa)
            {
                rki.copyFrom(p, local_id);
                return i;
            }
            if (ServerConfig::m_team_choosing)
            {
                if ((p->getTeam() == KART_TEAM_RED &&
                    rki.getKartTeam() == KART_TEAM_RED) ||
                    (p->getTeam() == KART_TEAM_BLUE &&
                    rki.getKartTeam() == KART_TEAM_BLUE))
                {
                    rki.copyFrom(p, local_id);
                    return i;
                }
            }
            else
            {
                if (rki.getKartTeam() == target_team)
                {
                    p->setTeam(target_team);
                    rki.copyFrom(p, local_id);
                    return i;
                }
            }
        }
    }
    return -1;
}   // getReservedId

//-----------------------------------------------------------------------------
/** Finally put the kart in the world and inform client the current world
 *  status, (including current confirmed item state, kart scores...)
 */
void ServerLobby::finishedLoadingLiveJoinClient(Event* event)
{
    std::shared_ptr<STKPeer> peer = event->getPeerSP();
    if (!canLiveJoinNow())
    {
        rejectLiveJoin(peer.get(), BLR_NO_GAME_FOR_LIVE_JOIN);
        return;
    }
    bool live_joined_in_time = true;
    for (const int id : peer->getAvailableKartIDs())
    {
        const RemoteKartInfo& rki = RaceManager::get()->getKartInfo(id);
        if (rki.isReserved())
        {
            live_joined_in_time = false;
            break;
        }
    }
    if (!live_joined_in_time)
    {
        Log::warn("ServerLobby", "%s can't live-join in time.",
            peer->getAddress().toString().c_str());
        rejectLiveJoin(peer.get(), BLR_NO_GAME_FOR_LIVE_JOIN);
        return;
    }
    World* w = World::getWorld();
    assert(w);

    uint64_t live_join_start_time = STKHost::get()->getNetworkTimer();

    // Instead of using getTicksSinceStart we caculate the current world ticks
    // only from network timer, because if the server hangs in between the
    // world ticks may not be up to date
    // 2000 is the time for ready set, remove 3 ticks after for minor
    // correction (make it more looks like getTicksSinceStart if server has no
    // hang
    int cur_world_ticks = stk_config->time2Ticks(
        (live_join_start_time - m_server_started_at - 2000) / 1000.f) - 3;
    // Give 3 seconds for all peers to get new kart info
    m_last_live_join_util_ticks =
        cur_world_ticks + stk_config->time2Ticks(3.0f);
    live_join_start_time -= m_server_delay;
    live_join_start_time += 3000;

    bool spectator = false;
    std::string msg;
    for (const int id : peer->getAvailableKartIDs())
    {
        World::getWorld()->addReservedKart(id);
        const RemoteKartInfo& rki = RaceManager::get()->getKartInfo(id);
        addLiveJoiningKart(id, rki, m_last_live_join_util_ticks);
        Log::info("ServerLobby", "%s succeeded live-joining with kart id %d.",
            peer->getAddress().toString().c_str(), id);
        if(ServerConfig::m_soccer_log)
	{
            GlobalLog::addIngamePlayer(id, StringUtils::wideToUtf8(rki.getPlayerName()), rki.getOnlineId() == 0);

            World* w = World::getWorld();
            if (w)
	    {
                SoccerWorld *sw = dynamic_cast<SoccerWorld*>(w);
	        std::string time = std::to_string(sw->getTime());
		auto kart_team = sw->getKartTeam(id);
		std::string team =  kart_team==KART_TEAM_RED ? "red" : "blue";
	        msg =  StringUtils::wideToUtf8(rki.getPlayerName()) + " joined the " + team + " team at "+ time + "\n";
                GlobalLog::writeLog(msg, GlobalLogTypes::POS_LOG);
	    }
	}
    }
    if (peer->getAvailableKartIDs().empty())
    {
        Log::info("ServerLobby", "%s spectating succeeded.",
            peer->getAddress().toString().c_str());
        spectator = true;
    }

    const uint8_t cc = (uint8_t)Track::getCurrentTrack()->getCheckManager()->getCheckStructureCount();
    NetworkString* ns = getNetworkString(10);
    ns->setSynchronous(true);
    ns->addUInt8(LE_LIVE_JOIN_ACK).addUInt64(m_client_starting_time)
        .addUInt8(cc).addUInt64(live_join_start_time)
        .addUInt32(m_last_live_join_util_ticks);

    NetworkItemManager* nim = dynamic_cast<NetworkItemManager*>
        (Track::getCurrentTrack()->getItemManager());
    assert(nim);
    nim->saveCompleteState(ns);
    nim->addLiveJoinPeer(peer);

    w->saveCompleteState(ns, peer.get());
    if (RaceManager::get()->supportsLiveJoining())
    {
        // Only needed in non-racing mode as no need players can added after
        // starting of race
        std::vector<std::shared_ptr<NetworkPlayerProfile> > players =
            getLivePlayers();
        encodePlayers(ns, players);
        for (unsigned i = 0; i < players.size(); i++)
            players[i]->getKartData().encode(ns);
    }

    m_peers_ready[peer] = false;
    peer->setWaitingForGame(false);
    peer->setSpectator(spectator);

    peer->sendPacket(ns, true/*reliable*/);
    delete ns;
    updatePlayerList();
    peer->updateLastActivity();
}   // finishedLoadingLiveJoinClient

//-----------------------------------------------------------------------------
/** Simple finite state machine.  Once this
 *  is known, register the server and its address with the stk server so that
 *  client can find it.
 */
void ServerLobby::update(int ticks)
{
    World* w = World::getWorld();
    bool world_started = m_state.load() >= WAIT_FOR_WORLD_LOADED &&
        m_state.load() <= RACING && m_server_has_loaded_world.load();
    bool all_players_in_world_disconnected = (w != NULL && world_started);
    int sec = ServerConfig::m_kick_idle_player_seconds;
    if (world_started)
    {
        for (unsigned i = 0; i < RaceManager::get()->getNumPlayers(); i++)
        {
            RemoteKartInfo& rki = RaceManager::get()->getKartInfo(i);
            std::shared_ptr<NetworkPlayerProfile> player =
                rki.getNetworkPlayerProfile().lock();
            if (player)
            {
                if (w)
                    all_players_in_world_disconnected = false;
            }
            else
                continue;
            auto peer = player->getPeer();
            if (!peer)
                continue;

            if (peer->idleForSeconds() > 60 && w &&
                w->getKart(i)->isEliminated())
            {
                // Remove loading world too long (60 seconds) live join peer
                Log::info("ServerLobby", "%s hasn't live-joined within"
                    " 60 seconds, remove it.",
                    peer->getAddress().toString().c_str());
                rki.makeReserved();
                continue;
            }
            if (!peer->isAIPeer() &&
                sec > 0 && peer->idleForSeconds() > sec &&
                !peer->isDisconnected() && NetworkConfig::get()->isWAN())
            {
                if (w && w->getKart(i)->hasFinishedRace())
                    continue;
                // Don't kick in game GUI server host so he can idle in game
                if (m_process_type == PT_CHILD &&
                    peer->getHostId() == m_client_server_host_id.load())
                    continue;
                Log::info("ServerLobby", "%s %s has been idle for more than"
                    " %d seconds, kick.",
                    peer->getAddress().toString().c_str(),
                    StringUtils::wideToUtf8(rki.getPlayerName()).c_str(), sec);
                peer->kick();
            }
        }
    }
    if (w)
        setGameStartedProgress(w->getGameStartedProgress());
    else
        resetGameStartedProgress();

    if (w && w->getPhase() == World::RACE_PHASE)
    {
        storePlayingTrack(RaceManager::get()->getTrackName());
    }
    else
        storePlayingTrack("");

    // Reset server to initial state if no more connected players
    if (m_rs_state.load() == RS_WAITING)
    {
        if ((RaceEventManager::get() &&
            !RaceEventManager::get()->protocolStopped()) ||
            !GameProtocol::emptyInstance())
            return;

        exitGameState();
        m_rs_state.store(RS_ASYNC_RESET);
    }

    STKHost::get()->updatePlayers();
    if (m_rs_state.load() == RS_NONE &&
        (m_state.load() > WAITING_FOR_START_GAME ||
        m_game_setup->isGrandPrixStarted()) &&
        (STKHost::get()->getPlayersInGame() == 0 ||
        all_players_in_world_disconnected))
    {
        if (RaceEventManager::get() &&
            RaceEventManager::get()->isRunning())
        {
            // Send a notification to all players who may have start live join
            // or spectate to go back to lobby
            NetworkString* back_to_lobby = getNetworkString(2);
            back_to_lobby->setSynchronous(true);
            back_to_lobby->addUInt8(LE_BACK_LOBBY).addUInt8(BLR_NONE);
            sendMessageToPeersInServer(back_to_lobby, /*reliable*/true);
            delete back_to_lobby;

            RaceEventManager::get()->stop();
            RaceEventManager::get()->getProtocol()->requestTerminate();
            GameProtocol::lock()->requestTerminate();
        }
        else if (auto ai = m_ai_peer.lock())
        {
            // Reset AI peer for empty server, which will delete world
            NetworkString* back_to_lobby = getNetworkString(2);
            back_to_lobby->setSynchronous(true);
            back_to_lobby->addUInt8(LE_BACK_LOBBY).addUInt8(BLR_NONE);
            ai->sendPacket(back_to_lobby, /*reliable*/true);
            delete back_to_lobby;
        }

        resetVotingTime();
        m_game_setup->stopGrandPrix();
        m_rs_state.store(RS_WAITING);
        return;
    }

    if (m_rs_state.load() != RS_NONE)
        return;

    // Reset for ranked server if in kart / track selection has only 1 player
    if (ServerConfig::m_ranked &&
        m_state.load() == SELECTING &&
        STKHost::get()->getPlayersInGame() == 1)
    {
        NetworkString* back_lobby = getNetworkString(2);
        back_lobby->setSynchronous(true);
        back_lobby->addUInt8(LE_BACK_LOBBY)
            .addUInt8(BLR_ONE_PLAYER_IN_RANKED_MATCH);
        sendMessageToPeers(back_lobby, /*reliable*/true);
        delete back_lobby;
        resetVotingTime();
        m_game_setup->stopGrandPrix();
        m_rs_state.store(RS_ASYNC_RESET);
    }

    handlePlayerDisconnection();

    std::string time;
    std::string time_msg;

    switch (m_state.load())
    {
    case SET_PUBLIC_ADDRESS:
    case REGISTER_SELF_ADDRESS:
    case WAITING_FOR_START_GAME:
    case WAIT_FOR_WORLD_LOADED:
    case WAIT_FOR_RACE_STARTED:
    {
        // Waiting for asynchronousUpdate
        break;
    }
    case SELECTING:
        // The function playerTrackVote will trigger the next state
        // once all track votes have been received.
        break;
    case LOAD_WORLD:
        Log::info("ServerLobbyRoom", "Starting the race loading.");
        // This will create the world instance, i.e. load track and karts
        loadWorld();
        m_state = WAIT_FOR_WORLD_LOADED;
        break;
    case RACING:
        if (World::getWorld() && RaceEventManager::get() &&
            RaceEventManager::get()->isRunning())
        {
            checkRaceFinished();
        }
        break;
    case WAIT_FOR_RACE_STOPPED:
        if (!RaceEventManager::get()->protocolStopped() ||
            !GameProtocol::emptyInstance())
            return;

        if (ServerConfig::m_soccer_log)
	{
            
            World* w = World::getWorld();
            if (w)
	    {
                SoccerWorld *sw = dynamic_cast<SoccerWorld*>(w);
	        time = std::to_string(sw->getTime());
            }
	    time_msg = "The game ended after " + time + " seconds.\n";
            GlobalLog::writeLog(time_msg, GlobalLogTypes::POS_LOG);
	}
		    
        // This will go back to lobby in server (and exit the current race)
        exitGameState();
        // Reset for next state usage
        resetPeersReady();
        // Set the delay before the server forces all clients to exit the race
        // result screen and go back to the lobby
        m_timeout.store((int64_t)StkTime::getMonoTimeMs() + 15000);
        m_state = RESULT_DISPLAY;
        sendMessageToPeers(m_result_ns, /*reliable*/ true);
        Log::info("ServerLobby", "End of game message sent");
        if(ServerConfig::m_soccer_log) GlobalLog::writeLog("GAME_END\n", GlobalLogTypes::POS_LOG);
        GlobalLog::closeLog(GlobalLogTypes::POS_LOG);
        break;
    case RESULT_DISPLAY:
        if (checkPeersReady(true/*ignore_ai_peer*/) ||
            (int64_t)StkTime::getMonoTimeMs() > m_timeout.load())
        {
            // Send a notification to all clients to exit
            // the race result screen
            NetworkString* back_to_lobby = getNetworkString(2);
            back_to_lobby->setSynchronous(true);
            back_to_lobby->addUInt8(LE_BACK_LOBBY).addUInt8(BLR_NONE);
            sendMessageToPeersInServer(back_to_lobby, /*reliable*/true);
            delete back_to_lobby;
            m_rs_state.store(RS_ASYNC_RESET);
        }
        break;
    case ERROR_LEAVE:
    case EXITING:
        break;
    }
}   // update

//-----------------------------------------------------------------------------
/** Register this server (i.e. its public address) with the STK server
 *  so that clients can find it. It blocks till a response from the
 *  stk server is received (this function is executed from the 
 *  ProtocolManager thread). The information about this client is added
 *  to the table 'server'.
 */
void ServerLobby::registerServer(bool first_time)
{
    // ========================================================================
    class RegisterServerRequest : public Online::XMLRequest
    {
    private:
        std::weak_ptr<ServerLobby> m_server_lobby;
        bool m_first_time;
    protected:
        virtual void afterOperation()
        {
            Online::XMLRequest::afterOperation();
            const XMLNode* result = getXMLData();
            std::string rec_success;
            auto sl = m_server_lobby.lock();
            if (!sl)
                return;

            if (result->get("success", &rec_success) &&
                rec_success == "yes")
            {
                const XMLNode* server = result->getNode("server");
                assert(server);
                const XMLNode* server_info = server->getNode("server-info");
                assert(server_info);
                unsigned server_id_online = 0;
                server_info->get("id", &server_id_online);
                assert(server_id_online != 0);
                bool is_official = false;
                server_info->get("official", &is_official);
                if (!is_official && ServerConfig::m_ranked)
                {
                    Log::fatal("ServerLobby", "You don't have permission to "
                        "host a ranked server.");
                }
                Log::info("ServerLobby",
                    "Server %d is now online.", server_id_online);
                sl->m_server_id_online.store(server_id_online);
                sl->m_last_success_poll_time.store(StkTime::getMonoTimeMs());
                return;
            }
            Log::error("ServerLobby", "%s",
                StringUtils::wideToUtf8(getInfo()).c_str());
            // Exit now if failed to register to stk addons for first time
            if (m_first_time)
                sl->m_state.store(ERROR_LEAVE);
        }
    public:
        RegisterServerRequest(std::shared_ptr<ServerLobby> sl, bool first_time)
        : XMLRequest(Online::RequestManager::HTTP_MAX_PRIORITY),
        m_server_lobby(sl), m_first_time(first_time) {}
    };   // RegisterServerRequest

    auto request = std::make_shared<RegisterServerRequest>(
        std::dynamic_pointer_cast<ServerLobby>(shared_from_this()), first_time);
    NetworkConfig::get()->setServerDetails(request, "create");
    const SocketAddress& addr = STKHost::get()->getPublicAddress();
    request->addParameter("address",      addr.getIP()        );
    request->addParameter("port",         addr.getPort()      );
    request->addParameter("private_port",
                                    STKHost::get()->getPrivatePort()      );
    request->addParameter("name", m_game_setup->getServerNameUtf8());
    request->addParameter("max_players", ServerConfig::m_server_max_players);
    int difficulty = m_difficulty.load();
    request->addParameter("difficulty", difficulty);
    int game_mode = m_game_mode.load();
    request->addParameter("game_mode", game_mode);
    const std::string& pw = ServerConfig::m_private_server_password;
    request->addParameter("password", (unsigned)(!pw.empty()));
    request->addParameter("version", (unsigned)ServerConfig::m_server_version);

    bool ipv6_only = addr.isUnset();
    if (!ipv6_only)
    {
        Log::info("ServerLobby", "Public IPv4 server address %s",
            addr.toString().c_str());
    }
    if (!STKHost::get()->getPublicIPv6Address().empty())
    {
        request->addParameter("address_ipv6",
            STKHost::get()->getPublicIPv6Address());
        Log::info("ServerLobby", "Public IPv6 server address %s",
            STKHost::get()->getPublicIPv6Address().c_str());
    }
    request->queue();
    m_server_registering = request;
}   // registerServer

//-----------------------------------------------------------------------------
/** Unregister this server (i.e. its public address) with the STK server,
 *  currently when karts enter kart selection screen it will be done or quit
 *  stk.
 */
void ServerLobby::unregisterServer(bool now, std::weak_ptr<ServerLobby> sl)
{
    // ========================================================================
    class UnRegisterServerRequest : public Online::XMLRequest
    {
    private:
        std::weak_ptr<ServerLobby> m_server_lobby;
    protected:
        virtual void afterOperation()
        {
            Online::XMLRequest::afterOperation();
            const XMLNode* result = getXMLData();
            std::string rec_success;

            if (result->get("success", &rec_success) &&
                rec_success == "yes")
            {
                // Clear the server online for next register
                // For grand prix server
                if (auto sl = m_server_lobby.lock())
                    sl->m_server_id_online.store(0);
                return;
            }
            Log::error("ServerLobby", "%s",
                StringUtils::wideToUtf8(getInfo()).c_str());
        }
    public:
        UnRegisterServerRequest(std::weak_ptr<ServerLobby> sl)
        : XMLRequest(Online::RequestManager::HTTP_MAX_PRIORITY),
        m_server_lobby(sl) {}
    };   // UnRegisterServerRequest
    auto request = std::make_shared<UnRegisterServerRequest>(sl);
    NetworkConfig::get()->setServerDetails(request, "stop");

    const SocketAddress& addr = STKHost::get()->getPublicAddress();
    request->addParameter("address", addr.getIP());
    request->addParameter("port", addr.getPort());
    bool ipv6_only = addr.isUnset();
    if (!ipv6_only)
    {
        Log::info("ServerLobby", "Unregister server address %s",
            addr.toString().c_str());
    }
    else
    {
        Log::info("ServerLobby", "Unregister server address %s",
            STKHost::get()->getValidPublicAddress().c_str());
    }

    // No need to check for result as server will be auto-cleared anyway
    // when no polling is done
    if (now)
    {
        request->executeNow();
    }
    else
        request->queue();

}   // unregisterServer

void ServerLobby::insertKartsIntoNotType(std::set<std::string>& set, const char* type) const
{
    // m_available_kts.first is not an addon list
    for (const std::string& kt : m_available_kts.first ) {
        const KartProperties* const props = kart_properties_manager->getKart(kt);
        const std::string& _type = props->getKartType();
        if (_type != type)
        {
            //Log::verbose("ServerLobby", "Kart %s has been ruled out, type %s != %s (requred)", kt.c_str(), _type.c_str(), type);
            set.insert(kt);
        }
    }
}
std::set<std::string> ServerLobby::getOtherKartsThan(const std::string& name) const
{
    std::set<std::string> set;
    for (const std::string& kt : m_available_kts.first ) {
        const KartProperties* const props = kart_properties_manager->getKart(kt);
        const core::stringw& _name = props->getName();
        if (_name != StringUtils::utf8ToWide(name))
        {
            set.insert(kt);
        }
    }
    return set;
}

const char* ServerLobby::kartRestrictedTypeName(const enum KartRestrictionMode mode) const
{
    switch (mode) {
        case LIGHT:
            return "light";
        case MEDIUM:
            return "medium";
        case HEAVY:
            return "heavy";
        case NONE:
            break;
    }
    return "";
}

void ServerLobby::setKartRestrictionMode(const enum KartRestrictionMode mode)
{
    m_kart_restriction = mode;
}
//-----------------------------------------------------------------------------
/** Instructs all clients to start the kart selection. If event is NULL,
 *  the command comes from the owner less server.
 */
void ServerLobby::startSelection(const Event *event)
{
    if (event != NULL)
    {
        if (m_state != WAITING_FOR_START_GAME)
        {
            Log::warn("ServerLobby",
                "Received startSelection while being in state %d.",
                m_state.load());
            return;
        }

        // check if player can play
        for (auto& player : event->getPeer()->getPlayerProfiles())
        {
            if (player->hasRestriction(PRF_NOGAME) ||
                    player->getPermissionLevel() < PERM_PLAYER)
            {
                NetworkString* const response = getNetworkString();
                response->setSynchronous(true);
                response->addUInt8(LE_CHAT).encodeString16(
                        L"You are not allowed to play the game.");
                event->getPeer()->sendPacket(response, true/*reliable*/);
                delete response;
                return;
            }
        }

        if (ServerConfig::m_owner_less)
        {
            // toggle ready
            m_peers_ready.at(event->getPeerSP()) =
                !m_peers_ready.at(event->getPeerSP());
            updatePlayerList();
            return;
        }
        if (event->getPeerSP() != m_server_owner.lock())
        {
            Log::warn("ServerLobby",
                "Client %d is not authorised to start selection.",
                event->getPeer()->getHostId());
            return;
        }
    }

#if 0
    if (!ServerConfig::m_owner_less && ServerConfig::m_team_choosing &&
        RaceManager::get()->teamEnabled())
    {
	set_powerup_multiplier(1); // ?????
        auto red_blue = STKHost::get()->getAllPlayersTeamInfo();
        if ((red_blue.first == 0 || red_blue.second == 0) &&
            red_blue.first + red_blue.second != 1)
        {
            Log::warn("ServerLobby", "Bad team choosing.");
            if (event)
            {
                NetworkString* bt = getNetworkString();
                bt->setSynchronous(true);
                bt->addUInt8(LE_BAD_TEAM);
                event->getPeer()->sendPacket(bt, true/*reliable*/);
                delete bt;
            }
            return;
        }
    }
#endif

    // Remove karts / tracks from server that are not supported on all clients
    std::set<std::string> karts_erase, tracks_erase;
    switch (m_kart_restriction) {
        case NONE:
            break;
        case LIGHT:
            insertKartsIntoNotType(karts_erase, "light");
            break;
        case MEDIUM:
            insertKartsIntoNotType(karts_erase, "medium");
            break;
        case HEAVY:
            insertKartsIntoNotType(karts_erase, "heavy");
            break;
    }
    auto peers = STKHost::get()->getPeers();
    std::set<STKPeer*> always_spectate_peers;
    bool has_peer_plays_game = false;
    for (auto peer : peers)
    {
        if (!peer->isValidated() || peer->isWaitingForGame())
            continue;

        peer->eraseServerKarts(m_available_kts.first, karts_erase);
        peer->eraseServerTracks(m_available_kts.second, tracks_erase);
        if (peer->alwaysSpectate())
            always_spectate_peers.insert(peer.get());
        else if (!peer->isAIPeer() && peer->hasPlayerProfiles()
                && peer->getPlayerProfiles()[0]->notRestrictedBy(PRF_NOGAME)
                && peer->getPlayerProfiles()[0]->getPermissionLevel()
                        >= PERM_PLAYER)
            has_peer_plays_game = true;
        else {
            for (auto& player : peer->getPlayerProfiles())
            {
                if (player->getPermissionLevel() < PERM_SPECTATOR)
                {
                    peer->setAlwaysSpectate(ASM_FULL);
                    peer->setWaitingForGame(true);
                    always_spectate_peers.insert(peer.get());
                }
                if (player->hasRestriction(PRF_NOGAME) ||
                    player->getPermissionLevel() <= PERM_SPECTATOR)
                {
                    always_spectate_peers.insert(peer.get());
                    break;
                }
            }
        }
    }

    // Disable always spectate peers if no players join the game
    if (!has_peer_plays_game)
    {
        for (STKPeer* peer : always_spectate_peers)
        {
            bool can_play = true;
            for (auto& player : peer->getPlayerProfiles())
            {
                if (player->hasRestriction(PRF_NOGAME) ||
                    player->getPermissionLevel() <= PERM_SPECTATOR)
                {
                    can_play = false;
                    break;
                }
            }
            if (can_play)
                peer->setAlwaysSpectate(ASM_NONE);
        }
        always_spectate_peers.clear();
    }
    else
    {
        // We make those always spectate peer waiting for game so it won't
        // be able to vote, this will be reset in STKHost::getPlayersForNewGame
        // This will also allow a correct number of in game players for max
        // arena players handling
        for (STKPeer* peer : always_spectate_peers)
            peer->setWaitingForGame(true);
    }

    unsigned max_player = 0;
    STKHost::get()->updatePlayers(&max_player);
    
    if (ServerConfig::m_soccer_log) GlobalLog::writeLog("GAME_START\n", GlobalLogTypes::POS_LOG);

    // Set late coming player to spectate if too many players
    auto spectators_by_limit = getSpectatorsByLimit();
    if (spectators_by_limit.size() == peers.size())
    {
        Log::error("ServerLobby", "Too many players and cannot set "
            "spectate for late coming players!");
        return;
    }
    for(auto &peer : spectators_by_limit)
    {
        peer->setAlwaysSpectate(ASM_FULL);
        peer->setWaitingForGame(true);
        always_spectate_peers.insert(peer.get());
    }

    for (const std::string& kart_erase : karts_erase)
    {
        m_available_kts.first.erase(kart_erase);
    }
    for (const std::string& track_erase : tracks_erase)
    {
        m_available_kts.second.erase(track_erase);
    }

    max_player = 0;
    STKHost::get()->updatePlayers(&max_player);
    if (auto ai = m_ai_peer.lock())
    {
        if (supportsAI())
        {
            unsigned total_ai_available =
                (unsigned)ai->getPlayerProfiles().size();
            m_ai_count = max_player > total_ai_available ?
                0 : total_ai_available - max_player + 1;
            // Disable ai peer for this game
            if (m_ai_count == 0)
                ai->setValidated(false);
            else
                ai->setValidated(true);
        }
        else
        {
            ai->setValidated(false);
            m_ai_count = 0;
        }
    }
    else
        m_ai_count = 0;

    if (RaceManager::get()->getMinorMode() == RaceManager::MINOR_MODE_FREE_FOR_ALL)
    {
        auto it = m_available_kts.second.begin();
        while (it != m_available_kts.second.end())
        {
            Track* t =  track_manager->getTrack(*it);
            if (t->getMaxArenaPlayers() < max_player)
            {
                it = m_available_kts.second.erase(it);
            }
            else
                it++;
        }
    }

    if (m_available_kts.second.empty())
    {
        Log::error("ServerLobby", "No tracks for playing!");
        return;
    }

    RandomGenerator rg;
    std::set<std::string>::iterator it = m_available_kts.second.begin();
    std::advance(it, rg.get((int)m_available_kts.second.size()));
    m_default_vote->m_track_name = *it;
    switch (RaceManager::get()->getMinorMode())
    {
        case RaceManager::MINOR_MODE_NORMAL_RACE:
        case RaceManager::MINOR_MODE_TIME_TRIAL:
        case RaceManager::MINOR_MODE_FOLLOW_LEADER:
        {
            Track* t = track_manager->getTrack(*it);
            assert(t);
            m_default_vote->m_num_laps = t->getDefaultNumberOfLaps();
            if (ServerConfig::m_auto_game_time_ratio > 0.0f)
            {
                m_default_vote->m_num_laps =
                    (uint8_t)(fmaxf(1.0f, (float)t->getDefaultNumberOfLaps() *
                    ServerConfig::m_auto_game_time_ratio));
            }
            else if (m_fixed_laps != -1)
                m_default_vote->m_num_laps = m_fixed_laps;
            m_default_vote->m_reverse = rg.get(2) == 0;
            break;
        }
        case RaceManager::MINOR_MODE_FREE_FOR_ALL:
        {
            m_default_vote->m_num_laps = 0;
            m_default_vote->m_reverse = rg.get(2) == 0;
            break;
        }
        case RaceManager::MINOR_MODE_CAPTURE_THE_FLAG:
        {
            m_default_vote->m_num_laps = 0;
            m_default_vote->m_reverse = 0;
            break;
        }
        case RaceManager::MINOR_MODE_SOCCER:
        {
            if (m_game_setup->isSoccerGoalTarget())
            {
                m_default_vote->m_num_laps =
                    (uint8_t)(UserConfigParams::m_num_goals);
                if (m_default_vote->m_num_laps > 10)
                    m_default_vote->m_num_laps = (uint8_t)5;
            }
            else
            {
                m_default_vote->m_num_laps =
                    (uint8_t)(UserConfigParams::m_soccer_time_limit);
                if (m_default_vote->m_num_laps > 15)
                    m_default_vote->m_num_laps = (uint8_t)7;
            }
            m_default_vote->m_reverse = rg.get(2) == 0;
            break;
        }
        default:
            assert(false);
            break;
    }

    if (!allowJoinedPlayersWaiting())
    {
        ProtocolManager::lock()->findAndTerminate(PROTOCOL_CONNECTION);
        if (m_server_id_online.load() != 0)
        {
            unregisterServer(false/*now*/,
                std::dynamic_pointer_cast<ServerLobby>(shared_from_this()));
        }
    }

    startVotingPeriod(ServerConfig::m_voting_timeout);
    const auto& all_k = m_available_kts.first;
    const auto& all_t = m_available_kts.second;
#if 0
#endif

    for (auto peer : peers)
    {
        auto profiles = peer->getPlayerProfiles();
        bool hasEnforcedKart = 
            peer->hasPlayerProfiles() && profiles.size() == 1 
            && !profiles[0]->getForcedKart().empty();
        // INSERT YOUR SETKART HERE
        NetworkString *ns = getNetworkString(1);
        // Start selection - must be synchronous since the receiver pushes
        // a new screen, which must be done from the main thread.
        ns->setSynchronous(true);
        ns->addUInt8(LE_START_SELECTION)
           .addFloat(ServerConfig::m_voting_timeout)
           .addUInt8(m_game_setup->isGrandPrixStarted() ? 1 : 0)
           .addUInt8((ServerConfig::m_auto_game_time_ratio > 0.0f ||
            m_fixed_laps != -1) ? 1 : 0)
           .addUInt8(ServerConfig::m_track_voting ? 1 : 0);

        if (hasEnforcedKart)
            ns->addUInt16(1);
        else
            ns->addUInt16((uint16_t)all_k.size());
        ns->addUInt16((uint16_t)all_t.size());

        if (hasEnforcedKart)
        {
            Log::verbose("ServerLobby", "forced kart: %s", profiles[0]->getForcedKart().c_str());
            ns->encodeString(profiles[0]->getForcedKart());
        }
        else
            for (const std::string& kart : all_k)
            {
                Log::verbose("ServerLobby", "not forced kart: %s", kart.c_str());
                ns->encodeString(kart);
            }
        for (const std::string& track : all_t)
        {
            ns->encodeString(track);
        }

        peer->sendPacket(ns, true/*reliable*/);
        delete ns;
    }
    //sendMessageToPeers(ns, /*reliable*/true);
#if 0
    STKHost::get()->sendPacketToAllPeersWith([this](STKPeer* peer)
    {
        if (peer->hasPlayerProfiles() && peer->getPlayerProfiles().size() == 1
                && !peer->getPlayerProfiles()[0]->getForcedKart().empty())
        {
            //AAATODO
            Log::verbose("ServerLobby", "setKart is used: %s", 
                    peer->getPlayerProfiles()[0]->getForcedKart().c_str());
            auto karts = getOtherKartsThan(
                    peer->getPlayerProfiles()[0]->getForcedKart());
            peer->eraseServerKarts(m_available_kts.first,
                karts);
        }
        return true;
    }, ns);
#endif

    m_state = SELECTING;
    if (!always_spectate_peers.empty())
    {
        NetworkString* back_lobby = getNetworkString(2);
        back_lobby->setSynchronous(true);
        back_lobby->addUInt8(LE_BACK_LOBBY).addUInt8(BLR_SPECTATING_NEXT_GAME);
        STKHost::get()->sendPacketToAllPeersWith(
            [always_spectate_peers](STKPeer* peer) {
            return always_spectate_peers.find(peer) !=
            always_spectate_peers.end(); }, back_lobby, /*reliable*/true);
        delete back_lobby;
        updatePlayerList();
    }

    if (!allowJoinedPlayersWaiting())
    {
        // Drop all pending players and keys if doesn't allow joinning-waiting
        for (auto& p : m_pending_connection)
        {
            if (auto peer = p.first.lock())
                peer->disconnect();
        }
        m_pending_connection.clear();
        std::unique_lock<std::mutex> ul(m_keys_mutex);
        m_keys.clear();
        ul.unlock();
    }

    // Will be changed after the first vote received
    m_timeout.store(std::numeric_limits<int64_t>::max());
}   // startSelection

//-----------------------------------------------------------------------------
/** Query the STK server for connection requests. For each connection request
 *  start a ConnectToPeer protocol.
 */
void ServerLobby::checkIncomingConnectionRequests()
{
    // First poll every 5 seconds. Return if no polling needs to be done.
    const uint64_t POLL_INTERVAL = 5000;
    static uint64_t last_poll_time = 0;
    if (StkTime::getMonoTimeMs() < last_poll_time + POLL_INTERVAL ||
        StkTime::getMonoTimeMs() > m_last_success_poll_time.load() + 30000)
        return;

    // Keep the port open, it can be sent to anywhere as we will send to the
    // correct peer later in ConnectToPeer.
    if (ServerConfig::m_firewalled_server)
    {
        BareNetworkString data;
        data.addUInt8(0);
        const SocketAddress* stun_v4 = STKHost::get()->getStunIPv4Address();
        const SocketAddress* stun_v6 = STKHost::get()->getStunIPv6Address();
        if (stun_v4)
            STKHost::get()->sendRawPacket(data, *stun_v4);
        if (stun_v6)
            STKHost::get()->sendRawPacket(data, *stun_v6);
    }

    // Now poll the stk server
    last_poll_time = StkTime::getMonoTimeMs();

    // ========================================================================
    class PollServerRequest : public Online::XMLRequest
    {
    private:
        std::weak_ptr<ServerLobby> m_server_lobby;
        std::weak_ptr<ProtocolManager> m_protocol_manager;
    protected:
        virtual void afterOperation()
        {
            Online::XMLRequest::afterOperation();
            const XMLNode* result = getXMLData();
            std::string success;

            if (!result->get("success", &success) || success != "yes")
            {
                Log::error("ServerLobby", "Poll server request failed: %s",
                    StringUtils::wideToUtf8(getInfo()).c_str());
                return;
            }

            // Now start a ConnectToPeer protocol for each connection request
            const XMLNode * users_xml = result->getNode("users");
            std::map<uint32_t, KeyData> keys;
            auto sl = m_server_lobby.lock();
            if (!sl)
                return;
            sl->m_last_success_poll_time.store(StkTime::getMonoTimeMs());
            if (sl->m_state.load() != WAITING_FOR_START_GAME &&
                !sl->allowJoinedPlayersWaiting())
            {
                sl->replaceKeys(keys);
                return;
            }

            sl->removeExpiredPeerConnection();
            for (unsigned int i = 0; i < users_xml->getNumNodes(); i++)
            {
                uint32_t addr, id;
                uint16_t port;
                std::string ipv6;
                users_xml->getNode(i)->get("ip", &addr);
                users_xml->getNode(i)->get("ipv6", &ipv6);
                users_xml->getNode(i)->get("port", &port);
                users_xml->getNode(i)->get("id", &id);
                users_xml->getNode(i)->get("aes-key", &keys[id].m_aes_key);
                users_xml->getNode(i)->get("aes-iv", &keys[id].m_aes_iv);
                users_xml->getNode(i)->get("username", &keys[id].m_name);
                users_xml->getNode(i)->get("country-code",
                    &keys[id].m_country_code);
                keys[id].m_tried = false;
                if (ServerConfig::m_firewalled_server)
                {
                    SocketAddress peer_addr(addr, port);
                    if (!ipv6.empty())
                        peer_addr.init(ipv6, port);
                    peer_addr.convertForIPv6Socket(isIPv6Socket());
                    std::string peer_addr_str = peer_addr.toString();
                    if (sl->m_pending_peer_connection.find(peer_addr_str) !=
                        sl->m_pending_peer_connection.end())
                    {
                        continue;
                    }
                    auto ctp = std::make_shared<ConnectToPeer>(peer_addr);
                    if (auto pm = m_protocol_manager.lock())
                        pm->requestStart(ctp);
                    sl->addPeerConnection(peer_addr_str);
                }
            }
            sl->replaceKeys(keys);
        }
    public:
        PollServerRequest(std::shared_ptr<ServerLobby> sl,
                          std::shared_ptr<ProtocolManager> pm)
        : XMLRequest(Online::RequestManager::HTTP_MAX_PRIORITY),
        m_server_lobby(sl), m_protocol_manager(pm)
        {
            m_disable_sending_log = true;
        }
    };   // PollServerRequest
    // ========================================================================

    auto request = std::make_shared<PollServerRequest>(
        std::dynamic_pointer_cast<ServerLobby>(shared_from_this()),
        ProtocolManager::lock());
    NetworkConfig::get()->setServerDetails(request,
        "poll-connection-requests");
    const SocketAddress& addr = STKHost::get()->getPublicAddress();
    request->addParameter("address", addr.getIP()  );
    request->addParameter("port",    addr.getPort());
    request->addParameter("current-players", getLobbyPlayers());
    request->addParameter("current-ai", m_current_ai_count.load());
    request->addParameter("game-started",
        m_state.load() == WAITING_FOR_START_GAME ? 0 : 1);
    std::string current_track = getPlayingTrackIdent();
    if (!current_track.empty())
        request->addParameter("current-track", getPlayingTrackIdent());
    request->queue();

}   // checkIncomingConnectionRequests

//-----------------------------------------------------------------------------
/** Checks if the race is finished, and if so informs the clients and switches
 *  to state RESULT_DISPLAY, during which the race result gui is shown and all
 *  clients can click on 'continue'.
 */
void ServerLobby::checkRaceFinished()
{
    assert(RaceEventManager::get()->isRunning());
    assert(World::getWorld());
    if (!RaceEventManager::get()->isRaceOver()) return;

    Log::info("ServerLobby", "The game is considered finished.");
    // notify the network world that it is stopped
    RaceEventManager::get()->stop();

    // stop race protocols before going back to lobby (end race)
    RaceEventManager::get()->getProtocol()->requestTerminate();
    GameProtocol::lock()->requestTerminate();

    // Save race result before delete the world
    m_result_ns->clear();
    m_result_ns->addUInt8(LE_RACE_FINISHED);
    if (m_game_setup->isGrandPrix())
    {
        // fastest lap
        int fastest_lap =
            static_cast<LinearWorld*>(World::getWorld())->getFastestLapTicks();
        m_result_ns->addUInt32(fastest_lap);
        m_result_ns->encodeString(static_cast<LinearWorld*>(World::getWorld())
            ->getFastestLapKartName());

        // all gp tracks
        m_result_ns->addUInt8((uint8_t)m_game_setup->getTotalGrandPrixTracks())
            .addUInt8((uint8_t)m_game_setup->getAllTracks().size());
        for (const std::string& gp_track : m_game_setup->getAllTracks())
            m_result_ns->encodeString(gp_track);

        // each kart score and total time
        m_result_ns->addUInt8((uint8_t)RaceManager::get()->getNumPlayers());
        for (unsigned i = 0; i < RaceManager::get()->getNumPlayers(); i++)
        {
            int last_score = RaceManager::get()->getKartScore(i);
            int cur_score = last_score;
            float overall_time = RaceManager::get()->getOverallTime(i);
            if (auto player =
                RaceManager::get()->getKartInfo(i).getNetworkPlayerProfile().lock())
            {
                last_score = player->getScore();
                cur_score += last_score;
                overall_time = overall_time + player->getOverallTime();
                player->setScore(cur_score);
                player->setOverallTime(overall_time);
            }
            m_result_ns->addUInt32(last_score).addUInt32(cur_score)
                .addFloat(overall_time);            
        }
    }
    else if (RaceManager::get()->modeHasLaps())
    {
        int fastest_lap =
            static_cast<LinearWorld*>(World::getWorld())->getFastestLapTicks();
        m_result_ns->addUInt32(fastest_lap);
        m_result_ns->encodeString(static_cast<LinearWorld*>(World::getWorld())
            ->getFastestLapKartName());
    }

    uint8_t ranking_changes_indication = 0;
    if (ServerConfig::m_ranked && RaceManager::get()->modeHasLaps())
        ranking_changes_indication = 1;
    m_result_ns->addUInt8(ranking_changes_indication);

    if (ServerConfig::m_ranked)
    {
        computeNewRankings();
        submitRankingsToAddons();
    }
    m_state.store(WAIT_FOR_RACE_STOPPED);

    // Remove kart restriction after the game is over
    setKartRestrictionMode(NONE);
    // Remove pole mode after the game is over
    setPoleEnabled(false);
}   // checkRaceFinished

//-----------------------------------------------------------------------------
/** Compute the new player's rankings used in ranked servers
 */
void ServerLobby::computeNewRankings()
{
    // TODO : go over the variables and look
    //        for things that can be simplified away.
    //        e.g. can new/prev be simplified ?

    // No ranking for battle mode
    if (!RaceManager::get()->modeHasLaps())
        return;

    std::vector<double> raw_scores_change;
    std::vector<double> new_raw_scores;
    std::vector<double> prev_raw_scores;
    std::vector<double> prev_scores;
    std::vector<double> new_rating_deviations;
    std::vector<double> prev_rating_deviations;
    std::vector<uint64_t> prev_disconnects; //bitflag
    std::vector<int>    disconnects;

    World* w = World::getWorld();
    assert(w);

    unsigned player_count = RaceManager::get()->getNumPlayers();
    m_result_ns->addUInt8((uint8_t)player_count);

    // If all players quitted the race, we assume something went wrong
    // and skip entirely rating and statistics updates.
    for (unsigned i = 0; i < player_count; i++)
    {
        if (!w->getKart(i)->isEliminated())
            break;
        if ((i + 1) == player_count)
            return;
    }

    // Initialize data vectors
    for (unsigned i = 0; i < player_count; i++)
    {
        const uint32_t id = RaceManager::get()->getKartInfo(i).getOnlineId();
        double prev_raw_score = m_raw_scores.at(id);
        new_raw_scores.push_back(prev_raw_score);
        prev_raw_scores.push_back(prev_raw_score);

        prev_scores.push_back(m_scores.at(id));

        double prev_deviation = m_rating_deviations.at(id);
        new_rating_deviations.push_back(prev_deviation);
        prev_rating_deviations.push_back(prev_deviation);

        prev_disconnects.push_back(m_num_ranked_disconnects.at(id));
    }
 
    // Update some variables
    for (unsigned i = 0; i < player_count; i++)
    {
        const uint32_t id = RaceManager::get()->getKartInfo(i).getOnlineId();

        //First, update the number of ranked races
        m_num_ranked_races.at(id)++;

        // Update the number of disconnects
        // We store the last 64 results as bit flags in a 64-bit int.
        // This way, shifting flushes the oldest result.
        m_num_ranked_disconnects.at(id) <<= 1;

        if (w->getKart(i)->isEliminated())
            m_num_ranked_disconnects.at(id)++;

        // std::popcount is C++20 only
        std::bitset<64> b(m_num_ranked_disconnects.at(id));
        disconnects.push_back(b.count());
    }

    // In this loop, considering the race as a set
    // of head to head minimatches, we compute :
    // I - Point changes for each ordered player pair.
    //     In a (p1, p2) pair, only p1's rating is changed.
    //     However, the loop will also go over (p2, p1).
    //     Point changes can be assymetric.
    // II - Rating deviation changes
    for (unsigned i = 0; i < player_count; i++)
    {
        raw_scores_change.push_back(0.0);

        double player1_raw_scores = new_raw_scores[i];
        if (w->getKart(i)->getHandicap())
            player1_raw_scores -= HANDICAP_OFFSET;

        // If the player has quitted before the race end,
        // the time value will be incorrect, but it will not be used
        double player1_time  = RaceManager::get()->getKartRaceTime(i);
        double player1_rd = prev_rating_deviations[i];

        // On a disconnect, increase RD once,
        // no matter how many opponents
        if (w->getKart(i)->isEliminated() && disconnects[i] >= 3)
            new_rating_deviations[i] =  prev_rating_deviations[i]
                                      + BASE_RD_PER_DISCONNECT
                                      + VAR_RD_PER_DISCONNECT * (disconnects[i] - 3);

        // Loop over all opponents
        for (unsigned j = 0; j < player_count; j++)
        {
            // Don't compare a player with himself
            if (i == j)
                continue;

            // No change between two quitting players
            if (   w->getKart(i)->isEliminated()
                && w->getKart(j)->isEliminated())
                continue;

            double diff, result, expected_result, ranking_importance, max_time;
            diff = result = expected_result = ranking_importance = max_time = 0.0;

            double player2_raw_scores = new_raw_scores[j];
            if (w->getKart(j)->getHandicap())
                player2_raw_scores -= HANDICAP_OFFSET;

            double player2_time = RaceManager::get()->getKartRaceTime(j);
            double player2_rd = prev_rating_deviations[j];

            // Each result can be viewed as new data helping to refine our previous
            // estimates. But first, we need to assess how reliable this new data is
            // compared to existing estimates.

            bool handicap_used = w->getKart(i)->getHandicap() || w->getKart(j)->getHandicap();
            double accuracy = computeDataAccuracy(player1_rd, player2_rd, player1_raw_scores, player2_raw_scores, player_count, handicap_used);

            // Now that we've computed the reliability value,
            // we can proceed with computing the points gained or lost

            // Compute the result and race ranking importance

            double mode_factor = getModeFactor();

            if (w->getKart(i)->isEliminated())
            {
                // Recurring disconnects are punished through
                // increased RD and higher RD floor,
                // not through higher raw score loss
                result = 0.0;
                player1_time = player2_time * 1.2; // for getTimeSpread
            }
            else if (w->getKart(j)->isEliminated())
            {
                result = 1.0;
                player2_time = player1_time * 1.2;
            }
            else
            {
                result = computeH2HResult(player1_time, player2_time);
            }

            max_time = std::min(MAX_SCALING_TIME, std::max(player1_time, player2_time));

            ranking_importance = accuracy * mode_factor * scalingValueForTime(max_time);

            // Compute the expected result using an ELO-like function
            diff = player2_raw_scores - player1_raw_scores;

            expected_result = 1.0/ (1.0 + std::pow(10.0,
                diff / (  BASE_RANKING_POINTS / 2.0
                        * getModeSpread()
                        * getTimeSpread(std::min(player1_time, player2_time)))));

            // Compute the ranking change
            raw_scores_change[i] += ranking_importance * (result - expected_result);

            // We now update the rating deviation. The change
            // depends on the current RD, on the result's accuracy,
            // on how expected the result was (upsets can increase RD)

            // If there was a disconnect in this race, RD was handled once already
            if (!w->getKart(i)->isEliminated()) {
                // First the RD reduction based on accuracy and current RD
                double rd_change_factor = accuracy * 0.0016;
                double rd_change = (-1) * prev_rating_deviations[i] * rd_change_factor;

                // If the unexpected result happened, we add a RD increase
                // TODO : more reliable would be accumulating an expected_result/result
                // differential over time, weighted through relative RDs.
                // If that differential goes high, then increase RD while decaying
                // the differential. Some work needed to ensure sensible maths.
                double upset = std::abs(result - expected_result);
                if (upset > 0.5)
                {
                    // Renormalize so expected result 50% is 1.0 and expected result 100% is 0.0
                    upset = 2.0 - 2 * upset;
                    upset = std::max(0.02, upset);

                    // If upsets happen at the rate predicted by expected score,
                    // this won't prevent the rating deviation from going down.
                    // However, if upsets are at least twice more frequent than expected, RD will go up.
                    rd_change += MIN_RATING_DEVIATION * rd_change_factor / upset;
                }

                // Minimum RD will be handled after all iterative RD change have been done,
                // so as to avoid the order in which player pairs are computed changing results.
                new_rating_deviations[i] += rd_change;
            }
        }
    }

    // Don't merge it in the main loop as new_scores value are used there
    for (unsigned i = 0; i < player_count; i++)
    {
        new_raw_scores[i] += raw_scores_change[i];
        const uint32_t id = RaceManager::get()->getKartInfo(i).getOnlineId();
        m_raw_scores.at(id) = new_raw_scores[i];

        // Ensure RD doesn't go below the RD floor.
        // The minimum RD is increased in case of repeated disconnects
        double disconnects_floor = 0;
        if (disconnects[i] >= 3)
        {
            int n = disconnects[i] - 3;
            disconnects_floor =   (disconnects[i]-2) * BASE_RD_PER_DISCONNECT
                                + VAR_RD_PER_DISCONNECT * (n * (n+1)) / 2;
        }
        new_rating_deviations[i] = std::max(new_rating_deviations[i], MIN_RATING_DEVIATION + disconnects_floor);
        m_rating_deviations.at(id) = new_rating_deviations[i];

        // Update the main public rating. At min RD, it is equal to the raw score.
        m_scores.at(id) = m_raw_scores.at(id) - 3*new_rating_deviations[i] + 3*MIN_RATING_DEVIATION;
        if (m_scores.at(id) > m_max_scores.at(id))
            m_max_scores.at(id) = m_scores.at(id);
    }

    // Used to display rating change at the end of a race
    for (unsigned i = 0; i < player_count; i++)
    {
        const uint32_t id = RaceManager::get()->getKartInfo(i).getOnlineId();
        double change = m_scores.at(id) - prev_scores[i];
        m_result_ns->addFloat((float)change);
    }
}   // computeNewRankings


//-----------------------------------------------------------------------------
/** Returns the mode race importance factor,
 *  used to make ranking move slower in more random modes.
 */
double ServerLobby::getModeFactor()
{
    if (RaceManager::get()->isTimeTrialMode())
        return 1.0;
    return 0.75;
}   // getModeFactor

//-----------------------------------------------------------------------------
/** Returns the mode spread factor, used so that a similar difference in
 *  skill will result in a similar ranking difference in more random modes.
 */
double ServerLobby::getModeSpread()
{
    if (RaceManager::get()->isTimeTrialMode())
        return 1.0;

    //TODO: the value used here for normal races is a wild guess.
    // When hard data to the spread tendencies of time-trial
    // and normal mode becomes available, update this to make
    // the spreads more comparable
    return 1.25;
}   // getModeSpread

//-----------------------------------------------------------------------------
/** Returns the time spread factor.
 *  Short races are more random, so the expected result changes depending
 *  on race duration.
 */
double ServerLobby::getTimeSpread(double time)
{
    return sqrt(120.0 / time);
}   // getTimeSpread

//-----------------------------------------------------------------------------
/** Compute the scaling value of a given time
 *  This is linear to race duration, getTimeSpread takes care
 *  of expecting a more random result in shorter races.
 */
double ServerLobby::scalingValueForTime(double time)
{
    return time * BASE_POINTS_PER_SECOND;
}   // scalingValueForTime

//-----------------------------------------------------------------------------
/** Computes the score of a head-to-head minimatch.
 *  If time difference > 2,5% ; the result is 1 (complete win of player 1)
 *  or 0 (complete loss of player 1)
 *  Otherwise, it is averaged between 0 and 1.
 */
double ServerLobby::computeH2HResult(double player1_time, double player2_time)
{
    double max_time = std::max(player1_time, player2_time);
    double min_time = std::min(player1_time, player2_time);

    double result = (max_time - min_time) / (min_time / 20.0);
    result = std::min(1.0, 0.5 + result);

    if (player2_time <= player1_time)
        result = 1.0 - result;

    return result;
}   // computeH2HResult

//-----------------------------------------------------------------------------
/** Computes a relative factor indicating how much informative value
 *  the new race result gives us.
 *
 *  For a player with a high own rating deviation, the current rating is unreliable
 *  so any new data holds more importance. This is crucial to allow reasonably
 *  fast rating convergence of new players, provided they play accurately rated opponents.
 *
 *  When the opponent has a high rating deviation, the expected scores are likely off.
 *  Therefore, the information from such a result is much less valuable.
 *
 *  We also reduce rating changes when the player ratings are very different, even
 *  after considering the uncertainties from rating deviation.
 *  This is multi-purpose :
 *   - With a very high rating difference, random race events (very poor luck, disconnects)
 *     are very likely to be the cause of any upset, so the rate of legitimate upsets is
 *     unreliable. No rating method is safe.
 *   - Attempting to "farm" much lower rated players against which a practical 100% winrate
 *     may be reached (outside of random events) becomes very ineffective. Instead,
 *     to gain rating points, the player has incentive to play well-rated opponents.
 *   - The primary goal is to ensure that two players of equal rating would be about
 *     evenly matched in head-to-head. If two strong players each beat a much weaker third
 *     player, very little information is gained on how a direct head-to-head between the
 *     strong players would go.
 *  For the purposes of this rating computation, we assume that the informational value
 *  of a race is roughly proportional to the likelihood of the weaker player winning.
 *  We cap the effect so that losing to a much weaker player still costs rating points.
 *
 *  In a race with many players, a single event can have a significant impact on the
 *  results of all the H2H. To avoid races with high players count having too strong
 *  rating swings, we apply a modifier scaling down accuracy.
 *
 *  Finally, while handicap is allowed in ranked races and a rating offset is applied
 *  to keep expected results realistic (without incentivizing playing handicap-only),
 *  the results of such races are much less reliable.
 */
double ServerLobby::computeDataAccuracy(double player1_rd, double player2_rd, double player1_scores, double player2_scores, int player_count, bool handicap_used)
{
    double accuracy = player1_rd / (sqrt(player2_rd) * sqrt(MIN_RATING_DEVIATION));

    double strong_lowerbound = (player1_scores > player2_scores) ? player1_scores - 3 * player1_rd
                                                                 : player2_scores - 3 * player2_rd;
    double weak_upperbound   = (player1_scores > player2_scores) ? player2_scores + 3 * player2_rd
                                                                 : player1_scores + 3 * player1_rd;

    if (weak_upperbound < strong_lowerbound)
    {
        double diff = strong_lowerbound - weak_upperbound;
        diff = diff / (BASE_RANKING_POINTS / 2.0);

        // The expected result is that of the weaker player and is between 0 and 0.5
        double expected_result = 1.0/ (1.0 + std::pow(10.0, diff));
        expected_result = std::max(0.2, sqrt(2*expected_result));

        accuracy *= expected_result;
    }

    // Reduce the importance of single h2h in a race with many players.
    // The overall impact of a race with more players is still always bigger.
    double player_count_modifier = 2.0 / sqrt((double) player_count);
    accuracy *= player_count_modifier;

    // Races with handicap are unreliable for ranking
    if (handicap_used)
        accuracy *= 0.25;

    return accuracy;
}

//-----------------------------------------------------------------------------
/** Called when a client disconnects.
 *  \param event The disconnect event.
 */
void ServerLobby::clientDisconnected(Event* event)
{
    auto players_on_peer = event->getPeer()->getPlayerProfiles();
    if (players_on_peer.empty())
        return;

    NetworkString* msg = getNetworkString(2);
    const bool waiting_peer_disconnected =
        event->getPeer()->isWaitingForGame();
    msg->setSynchronous(true);
    msg->addUInt8(LE_PLAYER_DISCONNECTED);
    msg->addUInt8((uint8_t)players_on_peer.size())
        .addUInt32(event->getPeer()->getHostId());
    for (auto p : players_on_peer)
    {
        std::string name = StringUtils::wideToUtf8(p->getName());
        msg->encodeString(name);
        Log::info("ServerLobby", "%s disconnected", name.c_str());
    }

    std::string msg2;
    std::string player_name;
    if(ServerConfig::m_soccer_log)
    {
        World* w = World::getWorld();
        if (w)
	{
            SoccerWorld *sw = dynamic_cast<SoccerWorld*>(w);
	    std::string time = std::to_string(sw->getTime());
            for (const auto id : event->getPeer()->getAvailableKartIDs())
	    {
                player_name = GlobalLog::getPlayerName(id);
		msg2 =  player_name + " left the game at " + time + ". \n";
		GlobalLog::writeLog(msg2, GlobalLogTypes::POS_LOG);
                GlobalLog::removeIngamePlayer(id);
	    }
	}
    }
    

    // Don't show waiting peer disconnect message to in game player
    STKHost::get()->sendPacketToAllPeersWith([waiting_peer_disconnected]
        (STKPeer* p)
        {
            if (!p->isValidated())
                return false;
            if (!p->isWaitingForGame() && waiting_peer_disconnected)
                return false;
            return true;
        }, msg);
    updatePlayerList();
    delete msg;

    writeDisconnectInfoTable(event->getPeer());

    // On last player
    if (!STKHost::get()->getPeerCount())
    {
        setKartRestrictionMode(NONE);
        setPoleEnabled(false);
    }
    else 
    {
        auto peer = event->getPeer();
        auto b = m_blue_pole_votes.find(peer);
        auto r = m_red_pole_votes.find(peer);
        if (b != m_blue_pole_votes.cend())
            m_blue_pole_votes.erase(b);
        if (r != m_red_pole_votes.cend())
            m_red_pole_votes.erase(r);
        RaceManager::get()->resetPoleProfile(event->getPeer());
    }
}   // clientDisconnected

//-----------------------------------------------------------------------------
void ServerLobby::clearDisconnectedRankedPlayer()
{
    for (auto it = m_ranked_players.begin(); it != m_ranked_players.end();)
    {
        if (it->second.expired())
        {
            const uint32_t id = it->first;
            m_scores.erase(id);
            m_max_scores.erase(id);
            m_num_ranked_races.erase(id);
            m_raw_scores.erase(id);
            m_rating_deviations.erase(id);
            m_num_ranked_disconnects.erase(id);
            it = m_ranked_players.erase(it);
        }
        else
        {
            it++;
        }
    }
}   // clearDisconnectedRankedPlayer

//-----------------------------------------------------------------------------
void ServerLobby::kickPlayerWithReason(STKPeer* peer, const char* reason) const
{
    NetworkString *message = getNetworkString(2);
    message->setSynchronous(true);
    message->addUInt8(LE_CONNECTION_REFUSED).addUInt8(RR_BANNED);
    message->encodeString(std::string(reason));
    peer->cleanPlayerProfiles();
    peer->sendPacket(message, true/*reliable*/, false/*encrypted*/);
    peer->reset();
    delete message;
}   // kickPlayerWithReason

//-----------------------------------------------------------------------------
void ServerLobby::saveIPBanTable(const SocketAddress& addr)
{
#ifdef ENABLE_SQLITE3
    if (addr.isIPv6() || !m_db || !m_ip_ban_table_exists)
        return;

    std::string query = StringUtils::insertValues(
        "INSERT INTO %s (ip_start, ip_end) "
        "VALUES (%u, %u);",
        ServerConfig::m_ip_ban_table.c_str(), addr.getIP(), addr.getIP());
    easySQLQuery(query);
#endif
}   // saveIPBanTable

//-----------------------------------------------------------------------------
void ServerLobby::removeIPBanTable(const SocketAddress& addr)
{
#ifdef ENABLE_SQLITE3
    if (addr.isIPv6() || !m_db || !m_ip_ban_table_exists)
        return;

    std::string query = StringUtils::insertValues(
        "DELETE FROM %s "
        "WHERE ip_start = %u AND ip_end = %u;",
        ServerConfig::m_ip_ban_table.c_str(), addr.getIP(), addr.getIP());
    easySQLQuery(query);
#endif
}   // removeIPBanTable

//-----------------------------------------------------------------------------
bool ServerLobby::handleAssets(const NetworkString& ns, STKPeer* peer)
{
    std::set<std::string> client_karts, client_tracks;
    const unsigned kart_num = ns.getUInt16();
    const unsigned track_num = ns.getUInt16();
    for (unsigned i = 0; i < kart_num; i++)
    {
        std::string kart;
        ns.decodeString(&kart);
        client_karts.insert(kart);
    }
    for (unsigned i = 0; i < track_num; i++)
    {
        std::string track;
        ns.decodeString(&track);
        client_tracks.insert(track);
    }

    // Drop this player if he doesn't have at least 1 kart / track the same
    // as server
    float okt = 0.0f;
    float ott = 0.0f;
    for (auto& client_kart : client_karts)
    {
        if (m_official_kts.first.find(client_kart) !=
            m_official_kts.first.end())
            okt += 1.0f;
    }
    okt = okt / (float)m_official_kts.first.size();
    for (auto& client_track : client_tracks)
    {
        if (m_official_kts.second.find(client_track) !=
            m_official_kts.second.end())
            ott += 1.0f;
    }
    ott = ott / (float)m_official_kts.second.size();

    std::set<std::string> karts_erase, tracks_erase;
    for (const std::string& server_kart : m_available_kts.first)
    {
        if (client_karts.find(server_kart) == client_karts.end())
        {
            karts_erase.insert(server_kart);
        }
    }
    for (const std::string& server_track : m_available_kts.second)
    {
        if (client_tracks.find(server_track) == client_tracks.end())
        {
            tracks_erase.insert(server_track);
        }
    }

    if (karts_erase.size() == m_available_kts.first.size() ||
        tracks_erase.size() == m_available_kts.second.size() ||
        okt < ServerConfig::m_official_karts_threshold ||
        ott < ServerConfig::m_official_tracks_threshold)
    {
        NetworkString *message = getNetworkString(2);
        message->setSynchronous(true);
        message->addUInt8(LE_CONNECTION_REFUSED)
            .addUInt8(RR_INCOMPATIBLE_DATA);
        peer->cleanPlayerProfiles();
        peer->sendPacket(message, true/*reliable*/, false/*encrypted*/);
        peer->reset();
        delete message;
        Log::verbose("ServerLobby", "Player has incompatible karts / tracks.");
        return false;
    }

    std::array<int, AS_TOTAL> addons_scores = {{ -1, -1, -1, -1 }};
    size_t addon_kart = 0;
    size_t addon_track = 0;
    size_t addon_arena = 0;
    size_t addon_soccer = 0;

    for (auto& kart : m_addon_kts.first)
    {
        if (client_karts.find(kart) != client_karts.end())
            addon_kart++;
    }
    for (auto& track : m_addon_kts.second)
    {
        if (client_tracks.find(track) != client_tracks.end())
            addon_track++;
    }
    for (auto& arena : m_addon_arenas)
    {
        if (client_tracks.find(arena) != client_tracks.end())
            addon_arena++;
    }
    for (auto& soccer : m_addon_soccers)
    {
        if (client_tracks.find(soccer) != client_tracks.end())
            addon_soccer++;
    }

    if (!m_addon_kts.first.empty())
    {
        addons_scores[AS_KART] = int
            ((float)addon_kart / (float)m_addon_kts.first.size() * 100.0);
    }
    if (!m_addon_kts.second.empty())
    {
        addons_scores[AS_TRACK] = int
            ((float)addon_track / (float)m_addon_kts.second.size() * 100.0);
    }
    if (!m_addon_arenas.empty())
    {
        addons_scores[AS_ARENA] = int
            ((float)addon_arena / (float)m_addon_arenas.size() * 100.0);
    }
    if (!m_addon_soccers.empty())
    {
        addons_scores[AS_SOCCER] = int
            ((float)addon_soccer / (float)m_addon_soccers.size() * 100.0);
    }

    // Save available karts and tracks from clients in STKPeer so if this peer
    // disconnects later in lobby it won't affect current players
    peer->setAvailableKartsTracks(client_karts, client_tracks);
    peer->setAddonsScores(addons_scores);

    if (m_process_type == PT_CHILD &&
        peer->getHostId() == m_client_server_host_id.load())
    {
        // Update child process addons list too so player can choose later
        updateAddons();
        updateTracksForMode();
    }
    return true;
}   // handleAssets

//-----------------------------------------------------------------------------
void ServerLobby::connectionRequested(Event* event)
{
    std::shared_ptr<STKPeer> peer = event->getPeerSP();
    NetworkString& data = event->data();
    if (!checkDataSize(event, 14)) return;

    peer->cleanPlayerProfiles();

    // can we add the player ?
    if (!allowJoinedPlayersWaiting() &&
        (m_state.load() != WAITING_FOR_START_GAME ||
        m_game_setup->isGrandPrixStarted()))
    {
        NetworkString *message = getNetworkString(2);
        message->setSynchronous(true);
        message->addUInt8(LE_CONNECTION_REFUSED).addUInt8(RR_BUSY);
        // send only to the peer that made the request and disconnect it now
        peer->sendPacket(message, true/*reliable*/, false/*encrypted*/);
        peer->reset();
        delete message;
        Log::verbose("ServerLobby", "Player refused: selection started");
        return;
    }

    // Check server version
    int version = data.getUInt32();
    if (version < stk_config->m_min_server_version ||
        version > stk_config->m_max_server_version)
    {
        NetworkString *message = getNetworkString(2);
        message->setSynchronous(true);
        message->addUInt8(LE_CONNECTION_REFUSED)
                .addUInt8(RR_INCOMPATIBLE_DATA);
        peer->sendPacket(message, true/*reliable*/, false/*encrypted*/);
        peer->reset();
        delete message;
        Log::verbose("ServerLobby", "Player refused: wrong server version");
        return;
    }
    std::string user_version;
    data.decodeString(&user_version);
    event->getPeer()->setUserVersion(user_version);

    unsigned list_caps = data.getUInt16();
    std::set<std::string> caps;
    for (unsigned i = 0; i < list_caps; i++)
    {
        std::string cap;
        data.decodeString(&cap);
        caps.insert(cap);
    }
    event->getPeer()->setClientCapabilities(caps);
    if (!handleAssets(data, event->getPeer()))
        return;

    unsigned player_count = data.getUInt8();
    uint32_t online_id = 0;
    uint32_t encrypted_size = 0;
    online_id = data.getUInt32();
    encrypted_size = data.getUInt32();

    // Will be disconnected if banned by IP
    testBannedForIP(peer.get());
    if (peer->isDisconnected())
        return;

    testBannedForIPv6(peer.get());
    if (peer->isDisconnected())
        return;

    if (online_id != 0)
        testBannedForOnlineId(peer.get(), online_id);
    // Will be disconnected if banned by online id
    if (peer->isDisconnected())
        return;

    unsigned total_players = 0;
    STKHost::get()->updatePlayers(NULL, NULL, &total_players);
    if (total_players + player_count + m_ai_profiles.size() >
        (unsigned)ServerConfig::m_server_max_players)
    {
        NetworkString *message = getNetworkString(2);
        message->setSynchronous(true);
        message->addUInt8(LE_CONNECTION_REFUSED).addUInt8(RR_TOO_MANY_PLAYERS);
        peer->sendPacket(message, true/*reliable*/, false/*encrypted*/);
        peer->reset();
        delete message;
        Log::verbose("ServerLobby", "Player refused: too many players");
        return;
    }

    // Reject non-valiated player joinning if WAN server and not disabled
    // encforement of validation, unless it's player from localhost or lan
    // And no duplicated online id or split screen players in ranked server
    // AIPeer only from lan and only 1 if ai handling
    std::set<uint32_t> all_online_ids =
        STKHost::get()->getAllPlayerOnlineIds();
    bool duplicated_ranked_player =
        all_online_ids.find(online_id) != all_online_ids.end();

    if (((encrypted_size == 0 || online_id == 0) &&
        !(peer->getAddress().isPublicAddressLocalhost() ||
        peer->getAddress().isLAN()) &&
        NetworkConfig::get()->isWAN() &&
        ServerConfig::m_validating_player) ||
        (ServerConfig::m_strict_players &&
        (player_count != 1 || online_id == 0 || duplicated_ranked_player)) ||
        (peer->isAIPeer() && !peer->getAddress().isLAN() &&!ServerConfig::m_ai_anywhere) ||
        (peer->isAIPeer() &&
        ServerConfig::m_ai_handling && !m_ai_peer.expired()))
    {
        NetworkString* message = getNetworkString(2);
        message->setSynchronous(true);
        message->addUInt8(LE_CONNECTION_REFUSED).addUInt8(RR_INVALID_PLAYER);
        peer->sendPacket(message, true/*reliable*/, false/*encrypted*/);
        peer->reset();
        delete message;
        Log::verbose("ServerLobby", "Player refused: invalid player");
        return;
    }

    if (ServerConfig::m_ai_handling && peer->isAIPeer())
        m_ai_peer = peer;

    if (encrypted_size != 0)
    {
        m_pending_connection[peer] = std::make_pair(online_id,
            BareNetworkString(data.getCurrentData(), encrypted_size));
    }
    else
    {
        core::stringw online_name;
        if (online_id > 0)
            data.decodeStringW(&online_name);
        handleUnencryptedConnection(peer, data, online_id, online_name,
            false/*is_pending_connection*/);
    }
}   // connectionRequested

//-----------------------------------------------------------------------------
void ServerLobby::handleUnencryptedConnection(std::shared_ptr<STKPeer> peer,
    BareNetworkString& data, uint32_t online_id,
    const core::stringw& online_name, bool is_pending_connection,
    std::string country_code)
{
    if (data.size() < 2) return;

    // Check for password
    std::string password;
    data.decodeString(&password);
    const std::string& server_pw = ServerConfig::m_private_server_password;
    if (password != server_pw)
    {
        NetworkString *message = getNetworkString(2);
        message->setSynchronous(true);
        message->addUInt8(LE_CONNECTION_REFUSED)
                .addUInt8(RR_INCORRECT_PASSWORD);
        peer->sendPacket(message, true/*reliable*/, false/*encrypted*/);
        peer->reset();
        delete message;
        Log::verbose("ServerLobby", "Player refused: incorrect password");
        return;
    }

    // Check again max players and duplicated player in ranked server,
    // if this is a pending connection
    unsigned total_players = 0;
    unsigned player_count = data.getUInt8();

    if (is_pending_connection)
    {
        STKHost::get()->updatePlayers(NULL, NULL, &total_players);
        if (total_players + player_count >
            (unsigned)ServerConfig::m_server_max_players)
        {
            NetworkString *message = getNetworkString(2);
            message->setSynchronous(true);
            message->addUInt8(LE_CONNECTION_REFUSED)
                .addUInt8(RR_TOO_MANY_PLAYERS);
            peer->sendPacket(message, true/*reliable*/, false/*encrypted*/);
            peer->reset();
            delete message;
            Log::verbose("ServerLobby", "Player refused: too many players");
            return;
        }

        std::set<uint32_t> all_online_ids =
            STKHost::get()->getAllPlayerOnlineIds();
        bool duplicated_ranked_player =
            all_online_ids.find(online_id) != all_online_ids.end();
        if (ServerConfig::m_ranked && duplicated_ranked_player)
        {
            NetworkString* message = getNetworkString(2);
            message->setSynchronous(true);
            message->addUInt8(LE_CONNECTION_REFUSED)
                .addUInt8(RR_INVALID_PLAYER);
            peer->sendPacket(message, true/*reliable*/, false/*encrypted*/);
            peer->reset();
            delete message;
            Log::verbose("ServerLobby", "Player refused: invalid player");
            return;
        }
    }

#ifdef ENABLE_SQLITE3
    if (country_code.empty() && !peer->getAddress().isIPv6())
        country_code = ip2Country(peer->getAddress());
    if (country_code.empty() && peer->getAddress().isIPv6())
        country_code = ipv62Country(peer->getAddress());
#endif

    auto red_blue = STKHost::get()->getAllPlayersTeamInfo();
    for (unsigned i = 0; i < player_count; i++)
    {
        core::stringw name;
        int permlvl;
        uint32_t restrictions;
        data.decodeStringW(&name);
        // 30 to make it consistent with stk-addons max user name length
        if (name.empty())
            name = L"unnamed";
        else if (name.size() > 30)
            name = name.subString(0, 30);
        float default_kart_color = data.getFloat();
        HandicapLevel handicap = (HandicapLevel)data.getUInt8();
        if (ServerConfig::m_server_owner > 0 && 
                online_id == ServerConfig::m_server_owner)
        {
            permlvl = std::numeric_limits<int>::max();
        }
        else
        {
            permlvl = loadPermissionLevelForOID(online_id);
        }
        restrictions = loadRestrictionsForOID(online_id);

        auto player = std::make_shared<NetworkPlayerProfile>
            (peer, i == 0 && !online_name.empty() && !peer->isAIPeer() ?
            online_name : name,
            peer->getHostId(), default_kart_color, i == 0 ? online_id : 0,
            handicap, (uint8_t)i, KART_TEAM_NONE,
            country_code, permlvl, restrictions);

        if (player->hasRestriction(PRF_NOGAME) ||
                player->getPermissionLevel() < PERM_PLAYER)
        {
            player->setTeam(KART_TEAM_NONE);
        }

        else if (ServerConfig::m_team_choosing)
        {
            KartTeam cur_team = KART_TEAM_NONE;
            if (red_blue.first > red_blue.second)
            {
                cur_team = KART_TEAM_BLUE;
                red_blue.second++;
            }
            else
            {
                cur_team = KART_TEAM_RED;
                red_blue.first++;
            }
            player->setTeam(cur_team);
        }
        peer->addPlayer(player);
    }

    peer->setValidated(true);

    // send a message to the one that asked to connect
    NetworkString* server_info = getNetworkString();
    server_info->setSynchronous(true);
    server_info->addUInt8(LE_SERVER_INFO);
    m_game_setup->addServerInfo(server_info);
    peer->sendPacket(server_info);
    delete server_info;

    sendRandomInstalladdonLine(peer);
    sendCurrentModifiers(peer);

    const bool game_started = m_state.load() != WAITING_FOR_START_GAME;
    NetworkString* message_ack = getNetworkString(4);
    message_ack->setSynchronous(true);
    // connection success -- return the host id of peer
    float auto_start_timer = 0.0f;
    if (m_timeout.load() == std::numeric_limits<int64_t>::max())
        auto_start_timer = std::numeric_limits<float>::max();
    else
    {
        auto_start_timer =
            (m_timeout.load() - (int64_t)StkTime::getMonoTimeMs()) / 1000.0f;
    }
    message_ack->addUInt8(LE_CONNECTION_ACCEPTED).addUInt32(peer->getHostId())
        .addUInt32(ServerConfig::m_server_version);

    message_ack->addUInt16(
        (uint16_t)stk_config->m_network_capabilities.size());
    for (const std::string& cap : stk_config->m_network_capabilities)
        message_ack->encodeString(cap);

    message_ack->addFloat(auto_start_timer)
        .addUInt32(ServerConfig::m_state_frequency)
        .addUInt8(ServerConfig::m_chat ? 1 : 0)
        .addUInt8(m_player_reports_table_exists ? 1 : 0);

    peer->setSpectator(false);

    // The 127.* or ::1/128 will be in charged for controlling AI
    if (m_ai_profiles.empty() && peer->getAddress().isLoopback())
    {
        unsigned ai_add = NetworkConfig::get()->getNumFixedAI();
        unsigned max_players = ServerConfig::m_server_max_players;
        // We need to reserve at least 1 slot for new player
        if (player_count + ai_add + 1 > max_players)
            ai_add = max_players - player_count - 1;
        for (unsigned i = 0; i < ai_add; i++)
        {
#ifdef SERVER_ONLY
            core::stringw name = L"Bot";
#else
            core::stringw name = _("Bot");
#endif
            name += core::stringw(" ") + StringUtils::toWString(i + 1);
            
            m_ai_profiles.push_back(std::make_shared<NetworkPlayerProfile>
                (peer, name, peer->getHostId(), 0.0f, 0, HANDICAP_NONE,
                player_count + i, KART_TEAM_NONE, ""));
        }
    }

    if (game_started)
    {
        peer->setWaitingForGame(true);
        updatePlayerList();
        peer->sendPacket(message_ack);
        delete message_ack;
    }
    else
    {
        peer->setWaitingForGame(false);
        m_peers_ready[peer] = false;
        if (!ServerConfig::m_sql_management)
        {
            for (std::shared_ptr<NetworkPlayerProfile>& npp :
                peer->getPlayerProfiles())
            {
                Log::info("ServerLobby",
                    "New player %s with online id %u from %s with %s.",
                    StringUtils::wideToUtf8(npp->getName()).c_str(),
                    npp->getOnlineId(), peer->getAddress().toString().c_str(),
                    peer->getUserVersion().c_str());
            }
        }
        updatePlayerList();
        peer->sendPacket(message_ack);
        delete message_ack;

        if (ServerConfig::m_ranked)
        {
            getRankingForPlayer(peer->getPlayerProfiles()[0]);
        }
    }


#ifdef ENABLE_SQLITE3
    if (m_server_stats_table.empty() || peer->isAIPeer())
        return;
    std::string query;
    if (ServerConfig::m_ipv6_connection && peer->getAddress().isIPv6())
    {
        query = StringUtils::insertValues(
            "INSERT INTO %s "
            "(host_id, ip, ipv6 ,port, online_id, username, player_num, "
            "country_code, version, os, ping) "
            "VALUES (%u, 0, \"%s\" ,%u, %u, ?, %u, ?, ?, ?, %u);",
            m_server_stats_table.c_str(), peer->getHostId(),
            peer->getAddress().toString(false), peer->getAddress().getPort(),
            online_id, player_count, peer->getAveragePing());
    }
    else
    {
        query = StringUtils::insertValues(
            "INSERT INTO %s "
            "(host_id, ip, port, online_id, username, player_num, "
            "country_code, version, os, ping) "
            "VALUES (%u, %u, %u, %u, ?, %u, ?, ?, ?, %u);",
            m_server_stats_table.c_str(), peer->getHostId(),
            peer->getAddress().getIP(), peer->getAddress().getPort(),
            online_id, player_count, peer->getAveragePing());
    }
    easySQLQuery(query, [peer, country_code](sqlite3_stmt* stmt)
        {
            if (sqlite3_bind_text(stmt, 1, StringUtils::wideToUtf8(
                peer->getPlayerProfiles()[0]->getName()).c_str(),
                -1, SQLITE_TRANSIENT) != SQLITE_OK)
            {
                Log::error("easySQLQuery", "Failed to bind %s.",
                    StringUtils::wideToUtf8(
                    peer->getPlayerProfiles()[0]->getName()).c_str());
            }
            if (country_code.empty())
            {
                if (sqlite3_bind_null(stmt, 2) != SQLITE_OK)
                {
                    Log::error("easySQLQuery",
                        "Failed to bind NULL for country code.");
                }
            }
            else
            {
                if (sqlite3_bind_text(stmt, 2, country_code.c_str(),
                    -1, SQLITE_TRANSIENT) != SQLITE_OK)
                {
                    Log::error("easySQLQuery", "Failed to bind country: %s.",
                        country_code.c_str());
                }
            }
            auto version_os =
                StringUtils::extractVersionOS(peer->getUserVersion());
            if (sqlite3_bind_text(stmt, 3, version_os.first.c_str(),
                -1, SQLITE_TRANSIENT) != SQLITE_OK)
            {
                Log::error("easySQLQuery", "Failed to bind %s.",
                    version_os.first.c_str());
            }
            if (sqlite3_bind_text(stmt, 4, version_os.second.c_str(),
                -1, SQLITE_TRANSIENT) != SQLITE_OK)
            {
                Log::error("easySQLQuery", "Failed to bind %s.",
                    version_os.second.c_str());
            }
        }
    );
#endif
}   // handleUnencryptedConnection

//-----------------------------------------------------------------------------
/** Called when any players change their setting (team for example), or
 *  connection / disconnection, it will use the game_started parameter to
 *  determine if this should be send to all peers in server or just in game.
 *  \param update_when_reset_server If true, this message will be sent to
 *  all peers.
 */
void ServerLobby::updatePlayerList(bool update_when_reset_server)
{
    const bool game_started = m_state.load() != WAITING_FOR_START_GAME &&
        !update_when_reset_server;

    auto all_profiles = STKHost::get()->getAllPlayerProfiles();
    size_t all_profiles_size = all_profiles.size();
    for (auto& profile : all_profiles)
    {
        if (profile->getPeer()->alwaysSpectate())
            all_profiles_size--;
    }

    auto spectators_by_limit = getSpectatorsByLimit();

    // N - 1 AI
    auto ai_instance = m_ai_peer.lock();
    if (supportsAI())
    {
        if (ai_instance)
        {
            auto ai_profiles = ai_instance->getPlayerProfiles();
            if (m_state.load() == WAITING_FOR_START_GAME ||
                update_when_reset_server)
            {
                if (all_profiles_size > ai_profiles.size())
                    ai_profiles.clear();
                else if (all_profiles_size != 0)
                {
                    ai_profiles.resize(
                        ai_profiles.size() - all_profiles_size + 1);
                }
            }
            else
            {
                // Use fixed number of AI calculated when started game
                ai_profiles.resize(m_ai_count);
            }
            all_profiles.insert(all_profiles.end(), ai_profiles.begin(),
                ai_profiles.end());
            m_current_ai_count.store((int)ai_profiles.size());
        }
        else if (!m_ai_profiles.empty())
        {
            all_profiles.insert(all_profiles.end(), m_ai_profiles.begin(),
                m_ai_profiles.end());
            m_current_ai_count.store((int)m_ai_profiles.size());
        }
    }
    else
        m_current_ai_count.store(0);

    m_lobby_players.store((int)all_profiles.size());

    // No need to update player list (for started grand prix currently)
    if (!allowJoinedPlayersWaiting() &&
        m_state.load() > WAITING_FOR_START_GAME && !update_when_reset_server)
        return;

    NetworkString* pl = getNetworkString();
    pl->setSynchronous(true);
    pl->addUInt8(LE_UPDATE_PLAYER_LIST)
        .addUInt8((uint8_t)(game_started ? 1 : 0))
        .addUInt8((uint8_t)all_profiles.size());
    for (auto profile : all_profiles)
    {
        auto profile_name = profile->getName();

        // get OS information
        auto version_os = StringUtils::extractVersionOS(profile->getPeer()->getUserVersion());
        std::string os_type_str = version_os.second;
        // if mobile OS
        if (os_type_str == "iOS" || os_type_str == "Android")
            // Add a Mobile emoji for mobile OS
            profile_name = StringUtils::utf32ToWide({ 0x1F4F1 }) + profile_name;

        // Add an hourglass emoji for players waiting because of the player limit
        if (spectators_by_limit.find(profile->getPeer()) != spectators_by_limit.end()) 
            profile_name = StringUtils::utf32ToWide({ 0x231B }) + profile_name;

        pl->addUInt32(profile->getHostId()).addUInt32(profile->getOnlineId())
            .addUInt8(profile->getLocalPlayerId())
            .encodeString(profile_name);

        std::shared_ptr<STKPeer> p = profile->getPeer();
        uint8_t boolean_combine = 0;

        // Show tux icon, otherwise show hourglass (lobby or ingame)
        if (p && p->isWaitingForGame())
            boolean_combine |= 1;

        // Show monitor display icon (spectating)
        if (p && (p->isSpectator() ||
            ((m_state.load() == WAITING_FOR_START_GAME ||
            update_when_reset_server) && p->alwaysSpectate())))
            boolean_combine |= (1 << 1);

        // Show crown icon (owner)
        if (p && m_server_owner_id.load() == p->getHostId())
            boolean_combine |= (1 << 2);

        // Show checkmark icon (player is ready)
        if (ServerConfig::m_owner_less && !game_started &&
            m_peers_ready.find(p) != m_peers_ready.end() &&
            m_peers_ready.at(p))
            boolean_combine |= (1 << 3);

        // Show robot icon (player is controlled by AI)
        if ((p && p->isAIPeer()) || isAIProfile(profile))
            boolean_combine |= (1 << 4);

        pl->addUInt8(boolean_combine);
        pl->addUInt8(profile->getHandicap());
        if (ServerConfig::m_team_choosing &&
            RaceManager::get()->teamEnabled())
            pl->addUInt8(profile->getTeam());
        else
            pl->addUInt8(KART_TEAM_NONE);
        pl->encodeString(profile->getCountryCode());
    }

    // Don't send this message to in-game players
    STKHost::get()->sendPacketToAllPeersWith([game_started]
        (STKPeer* p)
        {
            if (!p->isValidated())
                return false;
            if (!p->isWaitingForGame() && game_started)
                return false;
            return true;
        }, pl);
    delete pl;
}   // updatePlayerList

//-----------------------------------------------------------------------------
void ServerLobby::updateServerOwner()
{
    if (m_state.load() < WAITING_FOR_START_GAME ||
        m_state.load() > RESULT_DISPLAY ||
        ServerConfig::m_owner_less)
        return;
    if (!m_server_owner.expired())
        return;
    auto peers = STKHost::get()->getPeers();
    if (peers.empty())
        return;
    std::sort(peers.begin(), peers.end(), [](const std::shared_ptr<STKPeer> a,
        const std::shared_ptr<STKPeer> b)->bool
        {
            return a->getHostId() < b->getHostId();
        });

    std::shared_ptr<STKPeer> owner;
    for (auto peer: peers)
    {
        // Only matching host id can be server owner in case of
        // graphics-client-server
        if (peer->isValidated() && !peer->isAIPeer() &&
            (m_process_type == PT_MAIN ||
            peer->getHostId() == m_client_server_host_id.load()))
        {
            owner = peer;
            break;
        }
    }
    if (owner)
    {
        NetworkString* ns = getNetworkString();
        ns->setSynchronous(true);
        ns->addUInt8(LE_SERVER_OWNERSHIP);
        owner->sendPacket(ns);
        delete ns;
        m_server_owner = owner;
        m_server_owner_id.store(owner->getHostId());
        updatePlayerList();
    }
}   // updateServerOwner

//-----------------------------------------------------------------------------
/*! \brief Called when a player asks to select karts.
 *  \param event : Event providing the information.
 */
void ServerLobby::kartSelectionRequested(Event* event)
{
    if (m_state != SELECTING || m_game_setup->isGrandPrixStarted())
    {
        Log::warn("ServerLobby", "Received kart selection while in state %d.",
                  m_state.load());
        return;
    }

    if (!checkDataSize(event, 1) ||
        event->getPeer()->getPlayerProfiles().empty())
        return;

    const NetworkString& data = event->data();
    STKPeer* peer = event->getPeer();
    setPlayerKarts(data, peer);
}   // kartSelectionRequested

//-----------------------------------------------------------------------------
/*! \brief Called when a player votes for track(s), it will auto correct client
 *         data if it sends some invalid data.
 *  \param event : Event providing the information.
 */
void ServerLobby::handlePlayerVote(Event* event)
{
    if (m_state != SELECTING || !ServerConfig::m_track_voting)
    {
        Log::warn("ServerLobby", "Received track vote while in state %d.",
                  m_state.load());
        return;
    }

    if (!checkDataSize(event, 4) ||
        event->getPeer()->getPlayerProfiles().empty() ||
        event->getPeer()->isWaitingForGame())
        return;

    if (isVotingOver())  return;

    // Check permissions, otherwise won't vote for anything
    if (event->getPeer()->getPlayerProfiles()[0]->hasRestriction(PRF_TRACK)
            || event->getPeer()->getPlayerProfiles()[0]->getPermissionLevel()
            < PERM_PLAYER)
        return;

    NetworkString& data = event->data();
    PeerVote vote(data);
    Log::debug("ServerLobby",
        "Vote from client: host %d, track %s, laps %d, reverse %d.",
        event->getPeer()->getHostId(), vote.m_track_name.c_str(),
        vote.m_num_laps, vote.m_reverse);

    Track* t = track_manager->getTrack(vote.m_track_name);
    if (!t)
    {
        vote.m_track_name = *m_available_kts.second.begin();
        t = track_manager->getTrack(vote.m_track_name);
        assert(t);
    }

    // Remove / adjust any invalid settings
    if (RaceManager::get()->modeHasLaps())
    {
        if (ServerConfig::m_auto_game_time_ratio > 0.0f)
        {
            vote.m_num_laps =
                (uint8_t)(fmaxf(1.0f, (float)t->getDefaultNumberOfLaps() *
                ServerConfig::m_auto_game_time_ratio));
        }
        else if (m_fixed_laps != -1)
            vote.m_num_laps = m_fixed_laps;
        else if (vote.m_num_laps == 0 || vote.m_num_laps > 20)
            vote.m_num_laps = (uint8_t)3;
        if (!t->reverseAvailable() && vote.m_reverse)
            vote.m_reverse = false;
    }
    else if (RaceManager::get()->isSoccerMode())
    {
        if (m_game_setup->isSoccerGoalTarget())
        {
            if (ServerConfig::m_auto_game_time_ratio > 0.0f)
            {
                vote.m_num_laps = (uint8_t)(ServerConfig::m_auto_game_time_ratio *
                                            UserConfigParams::m_num_goals);
            }
            else if (vote.m_num_laps > 10)
                vote.m_num_laps = (uint8_t)5;
        }
        else
        {
            if (ServerConfig::m_auto_game_time_ratio > 0.0f)
            {
                vote.m_num_laps = (uint8_t)(ServerConfig::m_auto_game_time_ratio *
                                            UserConfigParams::m_soccer_time_limit);
            }
            else if (vote.m_num_laps > 15)
                vote.m_num_laps = (uint8_t)7;
        }
    }
    else if (RaceManager::get()->getMinorMode() ==
        RaceManager::MINOR_MODE_FREE_FOR_ALL)
    {
        vote.m_num_laps = 0;
    }
    else if (RaceManager::get()->getMinorMode() ==
        RaceManager::MINOR_MODE_CAPTURE_THE_FLAG)
    {
        vote.m_num_laps = 0;
        vote.m_reverse = false;
    }

    // Store vote:
    vote.m_player_name = event->getPeer()->getPlayerProfiles()[0]->getName();
    addVote(event->getPeer()->getHostId(), vote);

    // Now inform all clients about the vote
    NetworkString other = NetworkString(PROTOCOL_LOBBY_ROOM);
    other.setSynchronous(true);
    other.addUInt8(LE_VOTE);
    other.addUInt32(event->getPeer()->getHostId());
    vote.encode(&other);
    sendMessageToPeers(&other);

}   // handlePlayerVote

// ----------------------------------------------------------------------------
/** Select the track to be used based on all votes being received.
 * \param winner_vote The PeerVote that was picked.
 * \param winner_peer_id The host id of winner (unchanged if no vote).
 *  \return True if race can go on, otherwise wait.
 */
bool ServerLobby::handleAllVotes(PeerVote* winner_vote,
                                 uint32_t* winner_peer_id)
{
    // Determine majority agreement when 35% of voting time remains,
    // reserve some time for kart selection so it's not 50%
    if (getRemainingVotingTime() / getMaxVotingTime() > 0.35f)
    {
        return false;
    }

    // First remove all votes from disconnected hosts
    auto it = m_peers_votes.begin();
    while (it != m_peers_votes.end())
    {
        auto peer = STKHost::get()->findPeerByHostId(it->first);
        if (peer == nullptr)
        {
            it = m_peers_votes.erase(it);
        }
        else
            it++;
    }

    if (m_peers_votes.empty())
    {
        if (isVotingOver())
        {
            *winner_vote = *m_default_vote;
            return true;
        }
        return false;
    }

    // Count number of players 
    float cur_players = 0.0f;
    auto peers = STKHost::get()->getPeers();
    for (auto peer : peers)
    {
        if (peer->isAIPeer())
            continue;
        if (peer->hasPlayerProfiles() && !peer->isWaitingForGame())
            cur_players += 1.0f;
    }

    std::string top_track = m_default_vote->m_track_name;
    unsigned top_laps = m_default_vote->m_num_laps;
    bool top_reverse = m_default_vote->m_reverse;

    std::map<std::string, unsigned> tracks;
    std::map<unsigned, unsigned> laps;
    std::map<bool, unsigned> reverses;

    // Ratio to determine majority agreement
    float tracks_rate = 0.0f;
    float laps_rate = 0.0f;
    float reverses_rate = 0.0f;

    for (auto& p : m_peers_votes)
    {
        auto track_vote = tracks.find(p.second.m_track_name);
        if (track_vote == tracks.end())
            tracks[p.second.m_track_name] = 1;
        else
            track_vote->second++;
        auto lap_vote = laps.find(p.second.m_num_laps);
        if (lap_vote == laps.end())
            laps[p.second.m_num_laps] = 1;
        else
            lap_vote->second++;
        auto reverse_vote = reverses.find(p.second.m_reverse);
        if (reverse_vote == reverses.end())
            reverses[p.second.m_reverse] = 1;
        else
            reverse_vote->second++;
    }

    findMajorityValue<std::string>(tracks, cur_players, &top_track, &tracks_rate);
    findMajorityValue<unsigned>(laps, cur_players, &top_laps, &laps_rate);
    findMajorityValue<bool>(reverses, cur_players, &top_reverse, &reverses_rate);

    // End early if there is majority agreement which is all entries rate > 0.5
    it = m_peers_votes.begin();
    if (tracks_rate > 0.5f && laps_rate > 0.5f && reverses_rate > 0.5f)
    {
        while (it != m_peers_votes.end())
        {
            if (it->second.m_track_name == top_track &&
                it->second.m_num_laps == top_laps &&
                it->second.m_reverse == top_reverse)
                break;
            else
                it++;
        }
        if (it == m_peers_votes.end())
        {
            // Don't end if no vote matches all majority choices
            Log::warn("ServerLobby",
                "Missing track %s from majority.", top_track.c_str());
            it = m_peers_votes.begin();
            if (!isVotingOver())
                return false;
        }
        *winner_peer_id = it->first;
        *winner_vote = it->second;
        return true;
    }
    if (isVotingOver())
    {
        // Pick the best lap (or soccer goal / time) from only the top track
        // if no majority agreement from all
        int diff = std::numeric_limits<int>::max();
        auto closest_lap = m_peers_votes.begin();
        while (it != m_peers_votes.end())
        {
            if (it->second.m_track_name == top_track &&
                std::abs((int)it->second.m_num_laps - (int)top_laps) < diff)
            {
                closest_lap = it;
                diff = std::abs((int)it->second.m_num_laps - (int)top_laps);
            }
            else
                it++;
        }
        *winner_peer_id = closest_lap->first;
        *winner_vote = closest_lap->second;
        return true;
    }
    return false;
}   // handleAllVotes

// ----------------------------------------------------------------------------
template<typename T>
void ServerLobby::findMajorityValue(const std::map<T, unsigned>& choices, unsigned cur_players,
                       T* best_choice, float* rate)
{
    RandomGenerator rg;
    unsigned max_votes = 0;
    auto best_iter = choices.begin();
    unsigned best_iters_count = 1;
    // Among choices with max votes, we need to pick one uniformly,
    // thus we have to keep track of their number
    for (auto iter = choices.begin(); iter != choices.end(); iter++)
    {
        if (iter->second > max_votes)
        {
            max_votes = iter->second;
            best_iter = iter;
            best_iters_count = 1;
        }
        else if (iter->second == max_votes)
        {
            best_iters_count++;
            if (rg.get(best_iters_count) == 0)
            {
                max_votes = iter->second;
                best_iter = iter;
            }
        }
    }
    if (best_iter != choices.end())
    {
        *best_choice = best_iter->first;
        *rate = float(best_iter->second) / cur_players;
    }
}   // findMajorityValue

// ----------------------------------------------------------------------------
void ServerLobby::getHitCaptureLimit()
{
    int hit_capture_limit = std::numeric_limits<int>::max();
    float time_limit = 0.0f;
    if (RaceManager::get()->getMinorMode() ==
        RaceManager::MINOR_MODE_CAPTURE_THE_FLAG)
    {
        if (ServerConfig::m_capture_limit > 0)
            hit_capture_limit = ServerConfig::m_capture_limit;
        if (ServerConfig::m_time_limit_ctf > 0)
            time_limit = (float)ServerConfig::m_time_limit_ctf;
    }
    else
    {
        if (ServerConfig::m_hit_limit > 0)
            hit_capture_limit = ServerConfig::m_hit_limit;
        if (ServerConfig::m_time_limit_ffa > 0.0f)
            time_limit = (float)ServerConfig::m_time_limit_ffa;
    }
    m_battle_hit_capture_limit = hit_capture_limit;
    m_battle_time_limit = time_limit;
}   // getHitCaptureLimit

// ----------------------------------------------------------------------------
/** Called from the RaceManager of the server when the world is loaded. Marks
 *  the server to be ready to start the race.
 */
void ServerLobby::finishedLoadingWorld()
{
    for (auto p : m_peers_ready)
    {
        if (auto peer = p.first.lock())
            peer->updateLastActivity();
    }
    m_server_has_loaded_world.store(true);
}   // finishedLoadingWorld;

//-----------------------------------------------------------------------------
/** Called when a client notifies the server that it has loaded the world.
 *  When all clients and the server are ready, the race can be started.
 */
void ServerLobby::finishedLoadingWorldClient(Event *event)
{
    std::shared_ptr<STKPeer> peer = event->getPeerSP();
    peer->updateLastActivity();
    m_peers_ready.at(peer) = true;
    Log::info("ServerLobby", "Peer %d has finished loading world at %lf",
        peer->getHostId(), StkTime::getRealTime());
}   // finishedLoadingWorldClient

//-----------------------------------------------------------------------------
/** Called when a client clicks on 'ok' on the race result screen.
 *  If all players have clicked on 'ok', go back to the lobby.
 */
void ServerLobby::playerFinishedResult(Event *event)
{
    if (m_rs_state.load() == RS_ASYNC_RESET ||
        m_state.load() != RESULT_DISPLAY)
        return;
    std::shared_ptr<STKPeer> peer = event->getPeerSP();
    m_peers_ready.at(peer) = true;
}   // playerFinishedResult

//-----------------------------------------------------------------------------
bool ServerLobby::waitingForPlayers() const
{
    if (m_game_setup->isGrandPrix() && m_game_setup->isGrandPrixStarted())
        return false;
    return m_state.load() >= WAITING_FOR_START_GAME;
}   // waitingForPlayers

//-----------------------------------------------------------------------------
void ServerLobby::handlePendingConnection()
{
    std::lock_guard<std::mutex> lock(m_keys_mutex);

    for (auto it = m_pending_connection.begin();
         it != m_pending_connection.end();)
    {
        auto peer = it->first.lock();
        if (!peer)
        {
            it = m_pending_connection.erase(it);
        }
        else
        {
            const uint32_t online_id = it->second.first;
            auto key = m_keys.find(online_id);
            if (key != m_keys.end() && key->second.m_tried == false)
            {
                try
                {
                    if (decryptConnectionRequest(peer, it->second.second,
                        key->second.m_aes_key, key->second.m_aes_iv, online_id,
                        key->second.m_name, key->second.m_country_code))
                    {
                        it = m_pending_connection.erase(it);
                        m_keys.erase(online_id);
                        continue;
                    }
                    else
                        key->second.m_tried = true;
                }
                catch (std::exception& e)
                {
                    Log::error("ServerLobby",
                        "handlePendingConnection error: %s", e.what());
                    key->second.m_tried = true;
                }
            }
            it++;
        }
    }
}   // handlePendingConnection

//-----------------------------------------------------------------------------
bool ServerLobby::decryptConnectionRequest(std::shared_ptr<STKPeer> peer,
    BareNetworkString& data, const std::string& key, const std::string& iv,
    uint32_t online_id, const core::stringw& online_name,
    const std::string& country_code)
{
    auto crypto = std::unique_ptr<Crypto>(new Crypto(
        Crypto::decode64(key), Crypto::decode64(iv)));
    if (crypto->decryptConnectionRequest(data))
    {
        peer->setCrypto(std::move(crypto));
        Log::info("ServerLobby", "%s validated",
            StringUtils::wideToUtf8(online_name).c_str());
        handleUnencryptedConnection(peer, data, online_id,
            online_name, true/*is_pending_connection*/, country_code);
        return true;
    }
    return false;
}   // decryptConnectionRequest

//-----------------------------------------------------------------------------
void ServerLobby::getRankingForPlayer(std::shared_ptr<NetworkPlayerProfile> p)
{
    int priority = Online::RequestManager::HTTP_MAX_PRIORITY;
    auto request = std::make_shared<Online::XMLRequest>(priority);
    NetworkConfig::get()->setUserDetails(request, "get-ranking");

    const uint32_t id = p->getOnlineId();
    request->addParameter("id", id);
    request->executeNow();

    const XMLNode* result = request->getXMLData();
    std::string rec_success;

    // Default result
    double raw_score = BASE_RANKING_POINTS;
    double score     = BASE_RANKING_POINTS - 3*BASE_RATING_DEVIATION + 3*MIN_RATING_DEVIATION;
    double max_score = BASE_RANKING_POINTS - 3*BASE_RATING_DEVIATION + 3*MIN_RATING_DEVIATION;
    unsigned num_races = 0;
    double rating_deviation = BASE_RATING_DEVIATION;
    uint64_t disconnects = 0;
    if (result->get("success", &rec_success))
    {
        if (rec_success == "yes")
        {
            result->get("scores", &score);
            result->get("max-scores", &max_score);
            result->get("num-races-done", &num_races);
            result->get("raw-scores", &raw_score);
            result->get("rating-deviation", &rating_deviation);
            result->get("disconnects", &disconnects);
        }
        else
        {
            Log::error("ServerLobby", "No ranking info found for player %s.",
                StringUtils::wideToUtf8(p->getName()).c_str());
            // Kick the player to avoid his score being reset in case
            // connection to stk addons is broken
            auto peer = p->getPeer();
            if (peer)
            {
                peer->kick();
                return;
            }
        }
    }
    else
    {
        Log::error("ServerLobby", "No ranking info found for player %s.",
            StringUtils::wideToUtf8(p->getName()).c_str());
        auto peer = p->getPeer();
        if (peer)
        {
            peer->kick();
            return;
        }
    }
    m_ranked_players[id] = p;
    m_scores[id] = score;
    m_max_scores[id] = max_score;
    m_num_ranked_races[id] = num_races;
    m_raw_scores[id] = raw_score;
    m_rating_deviations[id] = rating_deviation;
    m_num_ranked_disconnects[id] = disconnects;
}   // getRankingForPlayer

//-----------------------------------------------------------------------------
void ServerLobby::submitRankingsToAddons()
{
    // No ranking for battle mode
    if (!RaceManager::get()->modeHasLaps())
        return;

    for (unsigned i = 0; i < RaceManager::get()->getNumPlayers(); i++)
    {
        const uint32_t id = RaceManager::get()->getKartInfo(i).getOnlineId();
        auto request = std::make_shared<SubmitRankingRequest>
            (id, m_scores.at(id), m_max_scores.at(id),
            m_num_ranked_races.at(id), m_raw_scores.at(id),
            m_rating_deviations.at(id), m_num_ranked_disconnects.at(id),
            RaceManager::get()->getKartInfo(i).getCountryCode());
        NetworkConfig::get()->setUserDetails(request, "submit-ranking");
        Log::info("ServerLobby", "Submiting ranking for %s (%d) : %lf, %lf %d",
            StringUtils::wideToUtf8(
            RaceManager::get()->getKartInfo(i).getPlayerName()).c_str(), id,
            m_scores.at(id), m_max_scores.at(id), m_num_ranked_races.at(id));
        request->queue();
    }
}   // submitRankingsToAddons

//-----------------------------------------------------------------------------
/** This function serves a purpose to send the message to all peers that are
 *  in game and/or spectating.
 */
void ServerLobby::broadcastMessageInGame(const irr::core::stringw& message)
{

    NetworkString* chat = getNetworkString();
    chat->setSynchronous(true);
    chat->addUInt8(LE_CHAT).encodeString16(message);

    // what is wrong here?
    STKHost::get()->sendPacketToAllPeersWith(
        [](STKPeer* peer) {
		// is player
	    return peer->hasPlayerProfiles() &&
	        // is in game or spectating
		(!peer->isWaitingForGame() || peer->isSpectator());
    }, chat);
    delete chat;
}

//-----------------------------------------------------------------------------
/** This function is called when all clients have loaded the world and
 *  are therefore ready to start the race. It determine the start time in
 *  network timer for client and server based on pings and then switches state
 *  to WAIT_FOR_RACE_STARTED.
 */
void ServerLobby::configPeersStartTime()
{
    uint32_t max_ping = 0;
    const unsigned max_ping_from_peers = ServerConfig::m_max_ping;
    bool peer_exceeded_max_ping = false;
    for (auto p : m_peers_ready)
    {
        auto peer = p.first.lock();
        // Spectators don't send input so we don't need to delay for them
        if (!peer || peer->alwaysSpectate())
            continue;
        if (peer->getAveragePing() > max_ping_from_peers)
        {
            Log::warn("ServerLobby",
                "Peer %s cannot catch up with max ping %d.",
                peer->getAddress().toString().c_str(), max_ping);
            peer_exceeded_max_ping = true;
            continue;
        }
        max_ping = std::max(peer->getAveragePing(), max_ping);
    }
    if ((ServerConfig::m_high_ping_workaround && peer_exceeded_max_ping) ||
        (ServerConfig::m_live_players && RaceManager::get()->supportsLiveJoining()))
    {
        Log::info("ServerLobby", "Max ping to ServerConfig::m_max_ping for "
            "live joining or high ping workaround.");
        max_ping = ServerConfig::m_max_ping;
    }
    // Start up time will be after 2500ms, so even if this packet is sent late
    // (due to packet loss), the start time will still ahead of current time
    uint64_t start_time = STKHost::get()->getNetworkTimer() + (uint64_t)2500;
    powerup_manager->setRandomSeed(start_time);
    NetworkString* ns = getNetworkString(10);
    ns->setSynchronous(true);
    ns->addUInt8(LE_START_RACE).addUInt64(start_time);
    const uint8_t cc = (uint8_t)Track::getCurrentTrack()->getCheckManager()->getCheckStructureCount();
    ns->addUInt8(cc);
    *ns += *m_items_complete_state;
    m_client_starting_time = start_time;
    sendMessageToPeers(ns, /*reliable*/true);

    const unsigned jitter_tolerance = ServerConfig::m_jitter_tolerance;
    Log::info("ServerLobby", "Max ping from peers: %d, jitter tolerance: %d",
        max_ping, jitter_tolerance);
    // Delay server for max ping / 2 from peers and jitter tolerance.
    m_server_delay = (uint64_t)(max_ping / 2) + (uint64_t)jitter_tolerance;
    start_time += m_server_delay;
    m_server_started_at = start_time;
    delete ns;
    m_state = WAIT_FOR_RACE_STARTED;

    World::getWorld()->setPhase(WorldStatus::SERVER_READY_PHASE);
    // Different stk process thread may have different stk host
    STKHost* stk_host = STKHost::get();
    joinStartGameThread();
    m_start_game_thread = std::thread([start_time, stk_host, this]()
        {
            const uint64_t cur_time = stk_host->getNetworkTimer();
            assert(start_time > cur_time);
            int sleep_time = (int)(start_time - cur_time);
            //Log::info("ServerLobby", "Start game after %dms", sleep_time);
            StkTime::sleep(sleep_time);
            //Log::info("ServerLobby", "Started at %lf", StkTime::getRealTime());
            m_state.store(RACING);
	    const std::string game_start_message = ServerConfig::m_game_start_message;

	    // Have Fun
	    if (!game_start_message.empty())
	    {
		broadcastMessageInGame(
		    StringUtils::utf8ToWide(game_start_message));
	    }
        });
}   // configPeersStartTime

//-----------------------------------------------------------------------------
bool ServerLobby::allowJoinedPlayersWaiting() const
{
    return !m_game_setup->isGrandPrix();
}   // allowJoinedPlayersWaiting

//-----------------------------------------------------------------------------
void ServerLobby::addWaitingPlayersToGame()
{
    auto all_profiles = STKHost::get()->getAllPlayerProfiles();
    for (auto& profile : all_profiles)
    {
        auto peer = profile->getPeer();
        if (!peer || !peer->isValidated())
            continue;

        peer->resetAlwaysSpectateFull();
        peer->setWaitingForGame(false);
        peer->setSpectator(false);
        if (m_peers_ready.find(peer) == m_peers_ready.end())
        {
            m_peers_ready[peer] = false;
            if (!ServerConfig::m_sql_management)
            {
                Log::info("ServerLobby",
                    "New player %s with online id %u from %s with %s.",
                    StringUtils::wideToUtf8(profile->getName()).c_str(),
                    profile->getOnlineId(),
                    peer->getAddress().toString().c_str(),
                    peer->getUserVersion().c_str());
            }
        }
        uint32_t online_id = profile->getOnlineId();
        if (ServerConfig::m_ranked &&
            (m_ranked_players.find(online_id) == m_ranked_players.end() ||
            (m_ranked_players.find(online_id) != m_ranked_players.end() &&
            m_ranked_players.at(online_id).expired())))
        {
            getRankingForPlayer(peer->getPlayerProfiles()[0]);
        }
    }
    // Re-activiate the ai
    if (auto ai = m_ai_peer.lock())
        ai->setValidated(true);
}   // addWaitingPlayersToGame

//-----------------------------------------------------------------------------
void ServerLobby::resetServer()
{
    addWaitingPlayersToGame();
    resetPeersReady();
    updatePlayerList(true/*update_when_reset_server*/);
    NetworkString* server_info = getNetworkString();
    server_info->setSynchronous(true);
    server_info->addUInt8(LE_SERVER_INFO);
    m_game_setup->addServerInfo(server_info);

    sendMessageToPeersInServer(server_info);
    delete server_info;

    if (ServerConfig::m_enable_ril)
    {
        NetworkString* ril_pkt = getNetworkString();
        ril_pkt->setSynchronous(true);
        addRandomInstalladdonMessage(ril_pkt);

        sendMessageToPeersInServer(ril_pkt);
        delete ril_pkt;
    }
    setup();
    m_state = NetworkConfig::get()->isLAN() ?
        WAITING_FOR_START_GAME : REGISTER_SELF_ADDRESS;
    updatePlayerList();
}   // resetServer

//-----------------------------------------------------------------------------
void ServerLobby::testBannedForIP(STKPeer* peer) const
{
#ifdef ENABLE_SQLITE3
    if (!m_db || !m_ip_ban_table_exists)
        return;

    // Test for IPv4
    if (peer->getAddress().isIPv6())
        return;

    int row_id = -1;
    unsigned ip_start = 0;
    unsigned ip_end = 0;
    std::string query = StringUtils::insertValues(
        "SELECT rowid, ip_start, ip_end, reason, description FROM %s "
        "WHERE ip_start <= %u AND ip_end >= %u "
        "AND datetime('now') > datetime(starting_time) AND "
        "(expired_days is NULL OR datetime"
        "(starting_time, '+'||expired_days||' days') > datetime('now')) "
        "LIMIT 1;",
        ServerConfig::m_ip_ban_table.c_str(),
        peer->getAddress().getIP(), peer->getAddress().getIP());

    sqlite3_stmt* stmt = NULL;
    int ret = sqlite3_prepare_v2(m_db, query.c_str(), -1, &stmt, 0);
    if (ret == SQLITE_OK)
    {
        ret = sqlite3_step(stmt);
        if (ret == SQLITE_ROW)
        {
            row_id = sqlite3_column_int(stmt, 0);
            ip_start = (unsigned)sqlite3_column_int64(stmt, 1);
            ip_end = (unsigned)sqlite3_column_int64(stmt, 2);
            const char* reason = (char*)sqlite3_column_text(stmt, 3);
            const char* desc = (char*)sqlite3_column_text(stmt, 4);
            Log::info("ServerLobby", "%s banned by IP: %s "
                "(rowid: %d, description: %s).",
                peer->getAddress().toString().c_str(), reason, row_id, desc);
            kickPlayerWithReason(peer, reason);
        }
        ret = sqlite3_finalize(stmt);
        if (ret != SQLITE_OK)
        {
            Log::error("ServerLobby",
                "Error finalize database for query %s: %s",
                query.c_str(), sqlite3_errmsg(m_db));
        }
    }
    else
    {
        Log::error("ServerLobby", "Error preparing database for query %s: %s",
            query.c_str(), sqlite3_errmsg(m_db));
        return;
    }
    if (row_id != -1)
    {
        query = StringUtils::insertValues(
            "UPDATE %s SET trigger_count = trigger_count + 1, "
            "last_trigger = datetime('now') "
            "WHERE ip_start = %u AND ip_end = %u;",
            ServerConfig::m_ip_ban_table.c_str(), ip_start, ip_end);
        easySQLQuery(query);
    }
#endif
}   // testBannedForIP

//-----------------------------------------------------------------------------
void ServerLobby::testBannedForIPv6(STKPeer* peer) const
{
#ifdef ENABLE_SQLITE3
    if (!m_db || !m_ipv6_ban_table_exists)
        return;

    // Test for IPv6
    if (!peer->getAddress().isIPv6())
        return;

    int row_id = -1;
    std::string ipv6_cidr;
    std::string query = StringUtils::insertValues(
        "SELECT rowid, ipv6_cidr, reason, description FROM %s "
        "WHERE insideIPv6CIDR(ipv6_cidr, ?) = 1 "
        "AND datetime('now') > datetime(starting_time) AND "
        "(expired_days is NULL OR datetime"
        "(starting_time, '+'||expired_days||' days') > datetime('now')) "
        "LIMIT 1;",
        ServerConfig::m_ipv6_ban_table.c_str());

    sqlite3_stmt* stmt = NULL;
    int ret = sqlite3_prepare_v2(m_db, query.c_str(), -1, &stmt, 0);
    if (ret == SQLITE_OK)
    {
        if (sqlite3_bind_text(stmt, 1,
            peer->getAddress().toString(false).c_str(), -1, SQLITE_TRANSIENT)
            != SQLITE_OK)
        {
            Log::error("ServerLobby", "Error binding ipv6 addr for query: %s",
                sqlite3_errmsg(m_db));
        }

        ret = sqlite3_step(stmt);
        if (ret == SQLITE_ROW)
        {
            row_id = sqlite3_column_int(stmt, 0);
            ipv6_cidr = (char*)sqlite3_column_text(stmt, 1);
            const char* reason = (char*)sqlite3_column_text(stmt, 2);
            const char* desc = (char*)sqlite3_column_text(stmt, 3);
            Log::info("ServerLobby", "%s banned by IP: %s "
                "(rowid: %d, description: %s).",
                peer->getAddress().toString().c_str(), reason, row_id, desc);
            kickPlayerWithReason(peer, reason);
        }
        ret = sqlite3_finalize(stmt);
        if (ret != SQLITE_OK)
        {
            Log::error("ServerLobby",
                "Error finalize database for query %s: %s",
                query.c_str(), sqlite3_errmsg(m_db));
        }
    }
    else
    {
        Log::error("ServerLobby", "Error preparing database for query %s: %s",
            query.c_str(), sqlite3_errmsg(m_db));
        return;
    }
    if (row_id != -1)
    {
        query = StringUtils::insertValues(
            "UPDATE %s SET trigger_count = trigger_count + 1, "
            "last_trigger = datetime('now') "
            "WHERE ipv6_cidr = ?;", ServerConfig::m_ipv6_ban_table.c_str());
        easySQLQuery(query, [ipv6_cidr](sqlite3_stmt* stmt)
            {
                if (sqlite3_bind_text(stmt, 1, ipv6_cidr.c_str(),
                    -1, SQLITE_TRANSIENT) != SQLITE_OK)
                {
                    Log::error("easySQLQuery", "Failed to bind %s.",
                        ipv6_cidr.c_str());
                }
            });
    }
#endif
}   // testBannedForIPv6

//-----------------------------------------------------------------------------
void ServerLobby::testBannedForOnlineId(STKPeer* peer,
                                        uint32_t online_id) const
{
#ifdef ENABLE_SQLITE3
    if (!m_db || !m_online_id_ban_table_exists)
        return;

    int row_id = -1;
    std::string query = StringUtils::insertValues(
        "SELECT rowid, reason, description FROM %s "
        "WHERE online_id = %u "
        "AND datetime('now') > datetime(starting_time) AND "
        "(expired_days is NULL OR datetime"
        "(starting_time, '+'||expired_days||' days') > datetime('now')) "
        "LIMIT 1;",
        ServerConfig::m_online_id_ban_table.c_str(), online_id);

    sqlite3_stmt* stmt = NULL;
    int ret = sqlite3_prepare_v2(m_db, query.c_str(), -1, &stmt, 0);
    if (ret == SQLITE_OK)
    {
        ret = sqlite3_step(stmt);
        if (ret == SQLITE_ROW)
        {
            row_id = sqlite3_column_int(stmt, 0);
            const char* reason = (char*)sqlite3_column_text(stmt, 1);
            const char* desc = (char*)sqlite3_column_text(stmt, 2);
            Log::info("ServerLobby", "%s banned by online id: %s "
                "(online id: %u rowid: %d, description: %s).",
                peer->getAddress().toString().c_str(), reason, online_id,
                row_id, desc);
            kickPlayerWithReason(peer, reason);
        }
        ret = sqlite3_finalize(stmt);
        if (ret != SQLITE_OK)
        {
            Log::error("ServerLobby", "Error finalize database: %s",
                sqlite3_errmsg(m_db));
        }
    }
    else
    {
        Log::error("ServerLobby", "Error preparing database: %s",
            sqlite3_errmsg(m_db));
        return;
    }
    if (row_id != -1)
    {
        query = StringUtils::insertValues(
            "UPDATE %s SET trigger_count = trigger_count + 1, "
            "last_trigger = datetime('now') "
            "WHERE online_id = %u;",
            ServerConfig::m_online_id_ban_table.c_str(), online_id);
        easySQLQuery(query);
    }
#endif
}   // testBannedForOnlineId

//-----------------------------------------------------------------------------
void ServerLobby::listBanTable()
{
#ifdef ENABLE_SQLITE3
    if (!m_db)
        return;
    auto printer = [](void* data, int argc, char** argv, char** name)
        {
            for (int i = 0; i < argc; i++)
            {
                std::cout << name[i] << " = " << (argv[i] ? argv[i] : "NULL")
                    << "\n";
            }
            std::cout << "\n";
            return 0;
        };
    if (m_ip_ban_table_exists)
    {
        std::string query = "SELECT * FROM ";
        query += ServerConfig::m_ip_ban_table;
        query += ";";
        std::cout << "IP ban list:\n";
        sqlite3_exec(m_db, query.c_str(), printer, NULL, NULL);
    }
    if (m_online_id_ban_table_exists)
    {
        std::string query = "SELECT * FROM ";
        query += ServerConfig::m_online_id_ban_table;
        query += ";";
        std::cout << "Online Id ban list:\n";
        sqlite3_exec(m_db, query.c_str(), printer, NULL, NULL);
    }
#endif
}   // listBanTable

//-----------------------------------------------------------------------------
float ServerLobby::getStartupBoostOrPenaltyForKart(uint32_t ping,
                                                   unsigned kart_id)
{
    AbstractKart* k = World::getWorld()->getKart(kart_id);
    if (k->getStartupBoost() != 0.0f)
        return k->getStartupBoost();
    uint64_t now = STKHost::get()->getNetworkTimer();
    uint64_t client_time = now - ping / 2;
    uint64_t server_time = client_time + m_server_delay;
    int ticks = stk_config->time2Ticks(
        (float)(server_time - m_server_started_at) / 1000.0f);
    if (ticks < stk_config->time2Ticks(1.0f))
    {
        PlayerController* pc =
            dynamic_cast<PlayerController*>(k->getController());
        pc->displayPenaltyWarning();
        return -1.0f;
    }
    float f = k->getStartupBoostFromStartTicks(ticks);
    k->setStartupBoost(f);
    return f;
}   // getStartupBoostOrPenaltyForKart

//-----------------------------------------------------------------------------
/*! \brief Called when the server owner request to change game mode or
 *         difficulty.
 *  \param event : Event providing the information.
 *
 *  Format of the data :
 *  Byte 0            1            2
 *       -----------------------------------------------
 *  Size |     1      |     1     |         1          |
 *  Data | difficulty | game mode | soccer goal target |
 *       -----------------------------------------------
 */
void ServerLobby::handleServerConfiguration(Event* event)
{
    if (m_state != WAITING_FOR_START_GAME)
    {
        Log::warn("ServerLobby",
            "Received handleServerConfiguration while being in state %d.",
            m_state.load());
        return;
    }
    if (!ServerConfig::m_server_configurable)
    {
        Log::warn("ServerLobby", "server-configurable is not enabled.");
        return;
    }
    if (event->getPeerSP() != m_server_owner.lock())
    {
        Log::warn("ServerLobby",
            "Client %d is not authorised to config server.",
            event->getPeer()->getHostId());
        return;
    }
    NetworkString& data = event->data();
    int new_difficulty = data.getUInt8();
    int new_game_mode = data.getUInt8();
    bool new_soccer_goal_target = data.getUInt8() == 1;
    auto modes = ServerConfig::getLocalGameMode(new_game_mode);
    if (modes.second == RaceManager::MAJOR_MODE_GRAND_PRIX)
    {
        Log::warn("ServerLobby", "Grand prix is used for new mode.");
        return;
    }

    RaceManager::get()->setMinorMode(modes.first);
    RaceManager::get()->setMajorMode(modes.second);
    RaceManager::get()->setDifficulty(RaceManager::Difficulty(new_difficulty));
    m_game_setup->resetExtraServerInfo();
    if (RaceManager::get()->getMinorMode() == RaceManager::MINOR_MODE_SOCCER)
        m_game_setup->setSoccerGoalTarget(new_soccer_goal_target);

    if (NetworkConfig::get()->isWAN() &&
        (m_difficulty.load() != new_difficulty ||
        m_game_mode.load() != new_game_mode))
    {
        Log::info("ServerLobby", "Updating server info with new "
            "difficulty: %d, game mode: %d to stk-addons.", new_difficulty,
            new_game_mode);
        int priority = Online::RequestManager::HTTP_MAX_PRIORITY;
        auto request = std::make_shared<Online::XMLRequest>(priority);
        NetworkConfig::get()->setServerDetails(request, "update-config");
        const SocketAddress& addr = STKHost::get()->getPublicAddress();
        request->addParameter("address", addr.getIP());
        request->addParameter("port", addr.getPort());
        request->addParameter("new-difficulty", new_difficulty);
        request->addParameter("new-game-mode", new_game_mode);
        request->queue();
    }
    m_difficulty.store(new_difficulty);
    m_game_mode.store(new_game_mode);
    updateTracksForMode();

    auto peers = STKHost::get()->getPeers();
    for (auto& peer : peers)
    {
        auto assets = peer->getClientAssets();
        if (!peer->isValidated() || assets.second.empty())
            continue;
        std::set<std::string> tracks_erase;
        for (const std::string& server_track : m_available_kts.second)
        {
            if (assets.second.find(server_track) == assets.second.end())
            {
                tracks_erase.insert(server_track);
            }
        }
        if (tracks_erase.size() == m_available_kts.second.size())
        {
            NetworkString *message = getNetworkString(2);
            message->setSynchronous(true);
            message->addUInt8(LE_CONNECTION_REFUSED)
                .addUInt8(RR_INCOMPATIBLE_DATA);
            peer->cleanPlayerProfiles();
            peer->sendPacket(message, true/*reliable*/);
            peer->reset();
            delete message;
            Log::verbose("ServerLobby",
                "Player has incompatible tracks for new game mode.");
        }
    }
    NetworkString* server_info = getNetworkString();
    server_info->setSynchronous(true);
    server_info->addUInt8(LE_SERVER_INFO);
    m_game_setup->addServerInfo(server_info);
    sendMessageToPeers(server_info);
    delete server_info;
    if (ServerConfig::m_enable_ril)
    {
        NetworkString* ril_pkt = getNetworkString();
        ril_pkt->setSynchronous(true);
        addRandomInstalladdonMessage(ril_pkt);

        sendMessageToPeers(ril_pkt);
        delete ril_pkt;
    }
    updatePlayerList();
}   // handleServerConfiguration

//-----------------------------------------------------------------------------
/*! \brief Called when a player want to change his handicap
 *  \param event : Event providing the information.
 *
 *  Format of the data :
 *  Byte 0                 1
 *       ----------------------------------
 *  Size |       1         |       1      |
 *  Data | local player id | new handicap |
 *       ----------------------------------
 */
void ServerLobby::changeHandicap(Event* event)
{
    NetworkString& data = event->data();
    if (m_state.load() != WAITING_FOR_START_GAME &&
        !event->getPeer()->isWaitingForGame())
    {
        Log::warn("ServerLobby", "Set handicap at wrong time.");
        return;
    }
    uint8_t local_id = data.getUInt8();
    auto& player = event->getPeer()->getPlayerProfiles().at(local_id);

    // Check for restrictions
    if (player->hasRestriction(PRF_HANDICAP))
    {
        Log::info("ServerLobby",
                "Player %s tried to change the handicap without permission.",
                StringUtils::wideToUtf8(player->getName()).c_str());

        NetworkString* const response = getNetworkString();
        response->setSynchronous(true);
        response->addUInt8(LE_CHAT).encodeString16(
                L"You are not allowed to change the handicap.");
        event->getPeer()->sendPacket(response, true/*reliable*/);
        delete response;
        return;
    }

    uint8_t handicap_id = data.getUInt8();
    if (handicap_id >= HANDICAP_COUNT)
    {
        Log::warn("ServerLobby", "Wrong handicap %d.", handicap_id);
        return;
    }
    HandicapLevel h = (HandicapLevel)handicap_id;
    player->setHandicap(h);
    updatePlayerList();
}   // changeHandicap
//-----------------------------------------------------------------------------
void ServerLobby::forceChangeHandicap(NetworkPlayerProfile* const player,
        const HandicapLevel status)
{
    player->setHandicap(status);
    updatePlayerList();
}   // forceChangeHandicap

//-----------------------------------------------------------------------------
/** Update and see if any player disconnects, if so eliminate the kart in
 *  world, so this function must be called in main thread.
 */
void ServerLobby::handlePlayerDisconnection() const
{
    if (!World::getWorld() ||
        World::getWorld()->getPhase() < WorldStatus::MUSIC_PHASE)
    {
        return;
    }

    int red_count = 0;
    int blue_count = 0;
    unsigned total = 0;
    for (unsigned i = 0; i < RaceManager::get()->getNumPlayers(); i++)
    {
        RemoteKartInfo& rki = RaceManager::get()->getKartInfo(i);
        if (rki.isReserved())
            continue;
        bool disconnected = rki.disconnected();
        if (RaceManager::get()->getKartInfo(i).getKartTeam() == KART_TEAM_RED &&
            !disconnected)
            red_count++;
        else if (RaceManager::get()->getKartInfo(i).getKartTeam() ==
            KART_TEAM_BLUE && !disconnected)
            blue_count++;

        if (!disconnected)
        {
            total++;
            continue;
        }
        else
            rki.makeReserved();

        AbstractKart* k = World::getWorld()->getKart(i);
        if (!k->isEliminated() && !k->hasFinishedRace())
        {
            CaptureTheFlag* ctf = dynamic_cast<CaptureTheFlag*>
                (World::getWorld());
            if (ctf)
                ctf->loseFlagForKart(k->getWorldKartId());

            World::getWorld()->eliminateKart(i,
                false/*notify_of_elimination*/);
            if (ServerConfig::m_ranked)
            {
                // Handle disconnection earlier to prevent cheating by joining
                // another ranked server
                // Real score will be submitted later in computeNewRankings
                const uint32_t id =
                    RaceManager::get()->getKartInfo(i).getOnlineId();
                unsigned num_races = m_num_ranked_races.at(id);
                uint64_t disconnects = m_num_ranked_disconnects.at(id) << 1;
                auto request = std::make_shared<SubmitRankingRequest>
                    (id, m_scores.at(id) - 200.0, m_max_scores.at(id),
                    ++num_races, m_raw_scores.at(id) - 200.0,
                    m_rating_deviations.at(id), ++disconnects,
                    RaceManager::get()->getKartInfo(i).getCountryCode());
                NetworkConfig::get()->setUserDetails(request,
                    "submit-ranking");
                request->queue();
            }
            k->setPosition(
                World::getWorld()->getCurrentNumKarts() + 1);
            k->finishedRace(World::getWorld()->getTime(), true/*from_server*/);
        }
    }

    // If live players is enabled, don't end the game if unfair team
    if (!ServerConfig::m_live_players &&
        total != 1 && World::getWorld()->hasTeam() &&
        (red_count == 0 || blue_count == 0))
        World::getWorld()->setUnfairTeam(true);

}   // handlePlayerDisconnection

//-----------------------------------------------------------------------------
/** Add reserved players for live join later if required.
 */
void ServerLobby::addLiveJoinPlaceholder(
    std::vector<std::shared_ptr<NetworkPlayerProfile> >& players,
    unsigned int push_front_blue,
    unsigned int push_front_red) const
{
    assert(push_front_blue <= 7);
    assert(push_front_red <= 7);
    if (!ServerConfig::m_live_players || !RaceManager::get()->supportsLiveJoining())
        return;
    if (RaceManager::get()->getMinorMode() == RaceManager::MINOR_MODE_FREE_FOR_ALL)
    {
        Track* t = track_manager->getTrack(m_game_setup->getCurrentTrack());
        assert(t);
        int max_players = std::min((int)ServerConfig::m_server_max_players,
            (int)t->getMaxArenaPlayers());
        int add_size = max_players - (int)players.size();
        assert(add_size >= 0);
        for (int i = 0; i < add_size; i++)
        {
            players.push_back(
                NetworkPlayerProfile::getReservedProfile(KART_TEAM_NONE));
        }
    }
    else
    {
        // CTF or soccer, reserve at most 7 players on each team
        int red_count = (int)push_front_red;
        int blue_count = (int)push_front_blue;
        for (unsigned i = 0; i < players.size(); i++)
        {
            if (players[i]->getTeam() == KART_TEAM_RED)
                red_count++;
            else
                blue_count++;
        }
        red_count = red_count >= 7 ? 0 : 7 - red_count;
        blue_count = blue_count >= 7 ? 0 : 7 - blue_count;
        for (unsigned int i = 0; i < push_front_red; i++)
        {
            players.insert(players.begin(),
                NetworkPlayerProfile::getReservedProfile(KART_TEAM_RED));
        }
        for (unsigned int i = 0; i < push_front_blue; i++)
        {
            players.insert(players.begin(),
                NetworkPlayerProfile::getReservedProfile(KART_TEAM_BLUE));
        }
        for (int i = 0; i < red_count; i++)
        {
            players.push_back(
                NetworkPlayerProfile::getReservedProfile(KART_TEAM_RED));
        }
        for (int i = 0; i < blue_count; i++)
        {
            players.push_back(
                NetworkPlayerProfile::getReservedProfile(KART_TEAM_BLUE));
        }
    }
}   // addLiveJoinPlaceholder

//-----------------------------------------------------------------------------
void ServerLobby::setPlayerKarts(const NetworkString& ns, STKPeer* peer) const
{
    unsigned player_count = ns.getUInt8();
    for (unsigned i = 0; i < player_count; i++)
    {
        std::string kart;
        std::shared_ptr<NetworkPlayerProfile> player =
            peer->getPlayerProfiles()[i];
        bool forcedRandom = false;

        ns.decodeString(&kart);
        const bool isStandardKart = kart.find("addon_") == std::string::npos;

        if (!player->getForcedKart().empty()
                && kart != player->getForcedKart())
        {
            Log::verbose("ServerLobby",
                         "Player %s chose the kart %s that doesn't comply with the requirement.",
                    StringUtils::wideToUtf8(player->getName()).c_str(),
                    kart.c_str());
            auto chat = getNetworkString();
            chat->setSynchronous(true);
            chat->addUInt8(LE_CHAT);
            chat->encodeString16(irr::core::stringw(L"You may not use this kart for this game. Please choose a different one."));
            peer->sendPacket(chat, true/*reliable*/);
            delete chat;
            forcedRandom = true;
        }
        if (m_kart_restriction && !isStandardKart)
        {
            auto kt = m_addon_kts.first.find(kart);
            if (kt == m_addon_kts.first.cend())
            {
                Log::verbose("ServerLobby",
                             "Player %s chose the addon kart %s that is not installed on the server.",
                        StringUtils::wideToUtf8(player->getName()).c_str(),
                        kart.c_str());
                auto chat = getNetworkString();
                chat->setSynchronous(true);
                chat->addUInt8(LE_CHAT);
                chat->encodeString16(irr::core::stringw(L"You may not use this kart for this game. Please choose a different one."));
                peer->sendPacket(chat, true/*reliable*/);
                delete chat;
                forcedRandom = true;
            }
            else {
                const std::string ktr(kartRestrictedTypeName(m_kart_restriction));
                const std::string ktc = kart_properties_manager->getKart(*kt)->getKartType();

                if (kt != m_addon_kts.first.cend() && 
                        ktc != ktr)
                {
                    Log::verbose("ServerLobby",
                                 "Player %s chose the addon kart %s that is of the type %s, not %s.",
                            StringUtils::wideToUtf8(player->getName()).c_str(),
                            kart.c_str(), ktc.c_str(), ktr.c_str());
                    auto chat = getNetworkString();
                    chat->setSynchronous(true);
                    chat->addUInt8(LE_CHAT);
                    chat->encodeString16(irr::core::stringw(L"You may not use this kart for this game. Please choose a different one."));
                    peer->sendPacket(chat, true/*reliable*/);
                    delete chat;
                    forcedRandom = true;
                }
            }   
        }

        // decide if the kart is chosen incorrectly or randomly
        //
        if (kart.find("randomkart") != std::string::npos || forcedRandom ||
                    (isStandardKart && (
                    m_available_kts.first.find(kart) == m_available_kts.first.end())))
        {
                RandomGenerator rg;
                std::set<std::string>::iterator it =
                    m_available_kts.first.begin();
                std::advance(it,
                    rg.get((int)m_available_kts.first.size()));
                player->setKartName(*it);
        }
        else
        {
            player->setKartName(kart);
        }
    }
    if (peer->getClientCapabilities().find("real_addon_karts") ==
        peer->getClientCapabilities().end() || ns.size() == 0)
        return;
    for (unsigned i = 0; i < player_count; i++)
    {
        KartData kart_data(ns);
        std::string type = kart_data.m_kart_type;
        auto& player = peer->getPlayerProfiles()[i];
        if (!player->getForcedKart().empty())
            player->setKartName(player->getForcedKart());

        const std::string& kart_id = player->getKartName();
        if (NetworkConfig::get()->useTuxHitboxAddon() &&
            StringUtils::startsWith(kart_id, "addon_") &&
            kart_properties_manager->hasKartTypeCharacteristic(type))
        {
            const KartProperties* real_addon =
                kart_properties_manager->getKart(kart_id);
            if (ServerConfig::m_real_addon_karts && real_addon)
            {
                kart_data = KartData(real_addon);
            }
            else
            {
                const KartProperties* tux_kp =
                    kart_properties_manager->getKart("tux");
                kart_data = KartData(tux_kp);
                kart_data.m_kart_type = type;
            }
            player->setKartData(kart_data);
        }
    }
}   // setPlayerKarts

//-----------------------------------------------------------------------------
/** Tell the client \ref RemoteKartInfo of a player when some player joining
 *  live.
 */
void ServerLobby::handleKartInfo(Event* event)
{
    World* w = World::getWorld();
    if (!w)
        return;

    STKPeer* peer = event->getPeer();
    const NetworkString& data = event->data();
    uint8_t kart_id = data.getUInt8();
    if (kart_id > RaceManager::get()->getNumPlayers())
        return;

    AbstractKart* k = w->getKart(kart_id);
    int live_join_util_ticks = k->getLiveJoinUntilTicks();

    const RemoteKartInfo& rki = RaceManager::get()->getKartInfo(kart_id);

    NetworkString* ns = getNetworkString(1);
    ns->setSynchronous(true);
    ns->addUInt8(LE_KART_INFO).addUInt32(live_join_util_ticks)
        .addUInt8(kart_id) .encodeString(rki.getPlayerName())
        .addUInt32(rki.getHostId()).addFloat(rki.getDefaultKartColor())
        .addUInt32(rki.getOnlineId()).addUInt8(rki.getHandicap())
        .addUInt8((uint8_t)rki.getLocalPlayerId())
        .encodeString(rki.getKartName()).encodeString(rki.getCountryCode());
    if (peer->getClientCapabilities().find("real_addon_karts") !=
        peer->getClientCapabilities().end())
        rki.getKartData().encode(ns);
    peer->sendPacket(ns, true/*reliable*/);
    delete ns;
}   // handleKartInfo

//-----------------------------------------------------------------------------
/** Client if currently in-game (including spectator) wants to go back to
 *  lobby.
 */
void ServerLobby::clientInGameWantsToBackLobby(Event* event)
{
    World* w = World::getWorld();
    std::shared_ptr<STKPeer> peer = event->getPeerSP();

    if (!w || !worldIsActive() || peer->isWaitingForGame())
    {
        Log::warn("ServerLobby", "%s try to leave the game at wrong time.",
            peer->getAddress().toString().c_str());
        return;
    }

    if (m_process_type == PT_CHILD &&
        event->getPeer()->getHostId() == m_client_server_host_id.load())
    {
        // For child server the remaining client cannot go on player when the
        // server owner quited the game (because the world will be deleted), so
        // we reset all players
        auto pm = ProtocolManager::lock();
        if (RaceEventManager::get())
        {
            RaceEventManager::get()->stop();
            pm->findAndTerminate(PROTOCOL_GAME_EVENTS);
        }
        auto gp = GameProtocol::lock();
        if (gp)
        {
            auto lock = gp->acquireWorldDeletingMutex();
            pm->findAndTerminate(PROTOCOL_CONTROLLER_EVENTS);
            exitGameState();
        }
        else
            exitGameState();
        NetworkString* back_to_lobby = getNetworkString(2);
        back_to_lobby->setSynchronous(true);
        back_to_lobby->addUInt8(LE_BACK_LOBBY)
            .addUInt8(BLR_SERVER_ONWER_QUITED_THE_GAME);
        sendMessageToPeersInServer(back_to_lobby, /*reliable*/true);
        delete back_to_lobby;
        m_rs_state.store(RS_ASYNC_RESET);
        return;
    }

    for (const int id : peer->getAvailableKartIDs())
    {
        RemoteKartInfo& rki = RaceManager::get()->getKartInfo(id);
        if (rki.getHostId() == peer->getHostId())
        {
            Log::info("ServerLobby", "%s left the game with kart id %d.",
                peer->getAddress().toString().c_str(), id);
            rki.setNetworkPlayerProfile(
                std::shared_ptr<NetworkPlayerProfile>());
        }
        else
        {
            Log::error("ServerLobby", "%s doesn't exist anymore in server.",
                peer->getAddress().toString().c_str());
        }
    }
    NetworkItemManager* nim = dynamic_cast<NetworkItemManager*>
        (Track::getCurrentTrack()->getItemManager());
    assert(nim);
    nim->erasePeerInGame(peer);
    m_peers_ready.erase(peer);
    peer->setWaitingForGame(true);
    peer->setSpectator(false);

    NetworkString* reset = getNetworkString(2);
    reset->setSynchronous(true);
    reset->addUInt8(LE_BACK_LOBBY).addUInt8(BLR_NONE);
    peer->sendPacket(reset, /*reliable*/true);
    delete reset;
    updatePlayerList();
    NetworkString* server_info = getNetworkString();
    server_info->setSynchronous(true);
    server_info->addUInt8(LE_SERVER_INFO);
    m_game_setup->addServerInfo(server_info);
    peer->sendPacket(server_info, /*reliable*/true);
    delete server_info;
    sendRandomInstalladdonLine(peer);
    sendCurrentModifiers(peer);
}   // clientInGameWantsToBackLobby

//-----------------------------------------------------------------------------
/** Client if currently select assets wants to go back to lobby.
 */
void ServerLobby::clientSelectingAssetsWantsToBackLobby(Event* event)
{
    std::shared_ptr<STKPeer> peer = event->getPeerSP();

    if (m_state.load() != SELECTING || peer->isWaitingForGame())
    {
        Log::warn("ServerLobby",
            "%s try to leave selecting assets at wrong time.",
            peer->getAddress().toString().c_str());
        return;
    }

    if (m_process_type == PT_CHILD &&
        event->getPeer()->getHostId() == m_client_server_host_id.load())
    {
        NetworkString* back_to_lobby = getNetworkString(2);
        back_to_lobby->setSynchronous(true);
        back_to_lobby->addUInt8(LE_BACK_LOBBY)
            .addUInt8(BLR_SERVER_ONWER_QUITED_THE_GAME);
        sendMessageToPeersInServer(back_to_lobby, /*reliable*/true);
        delete back_to_lobby;
        resetVotingTime();
        resetServer();
        m_rs_state.store(RS_NONE);
        return;
    }

    m_peers_ready.erase(peer);
    peer->setWaitingForGame(true);
    peer->setSpectator(false);

    NetworkString* reset = getNetworkString(2);
    reset->setSynchronous(true);
    reset->addUInt8(LE_BACK_LOBBY).addUInt8(BLR_NONE);
    peer->sendPacket(reset, /*reliable*/true);
    delete reset;
    updatePlayerList();
    NetworkString* server_info = getNetworkString();
    server_info->setSynchronous(true);
    server_info->addUInt8(LE_SERVER_INFO);
    m_game_setup->addServerInfo(server_info);
    peer->sendPacket(server_info, /*reliable*/true);
    delete server_info;
    sendRandomInstalladdonLine(peer);
    sendCurrentModifiers(peer);
}   // clientSelectingAssetsWantsToBackLobby

std::set<std::shared_ptr<STKPeer>> ServerLobby::getSpectatorsByLimit()
{
    std::set<std::shared_ptr<STKPeer>> spectators_by_limit;

    auto peers = STKHost::get()->getPeers();
    std::set<std::shared_ptr<STKPeer>> always_spectate_peers;

    unsigned player_limit = m_max_players_in_game;
    // only 10 players allowed for battle or soccer
    if (RaceManager::get()->isBattleMode() || RaceManager::get()->isSoccerMode())
        player_limit = std::min(player_limit, 10u);

    unsigned ingame_players = 0, waiting_players = 0, total_players = 0;
    STKHost::get()->updatePlayers(&ingame_players, &waiting_players, &total_players);
    if (total_players <= player_limit)
        return spectators_by_limit;

    std::sort(peers.begin(), peers.end(),
        [](const std::shared_ptr<STKPeer>& a,
            const std::shared_ptr<STKPeer>& b)
        { return a->getHostId() < b->getHostId(); });

    if (m_state.load() >= RACING)
    {
        for (auto &peer : peers)
            if (peer->isSpectator())
                ingame_players -= (int)peer->getPlayerProfiles().size();
    }

    unsigned player_count = 0;
    for (unsigned i = 0; i < peers.size(); i++)
    {
        auto& peer = peers[i];
        if (!peer->isValidated())
            continue;
        if (m_state.load() < RACING)
        {
            if (peer->alwaysSpectate() || peer->isWaitingForGame())
                continue;
            player_count += (unsigned)peer->getPlayerProfiles().size();
            if (player_count > player_limit)
                spectators_by_limit.insert(peer);
        }
        else
        {
            if (peer->isSpectator())
                continue;
            player_count += (unsigned)peer->getPlayerProfiles().size();
            if (peer->isWaitingForGame() && (player_count > player_limit || ingame_players >= player_limit))
                spectators_by_limit.insert(peer);
        }
    }
    return spectators_by_limit;
}

//-----------------------------------------------------------------------------
void ServerLobby::saveInitialItems(std::shared_ptr<NetworkItemManager> nim)
{
    m_items_complete_state->getBuffer().clear();
    m_items_complete_state->reset();
    nim->saveCompleteState(m_items_complete_state);
}   // saveInitialItems

//-----------------------------------------------------------------------------
bool ServerLobby::supportsAI()
{
    return getGameMode() == 3 || getGameMode() == 4;
}   // supportsAI

//-----------------------------------------------------------------------------
bool ServerLobby::checkPeersCanPlay(bool ignore_ai_peer) const
{
// frustrating indentation, in this vim setting
    const bool is_team_game = (
        RaceManager::get()->getMinorMode() == RaceManager::MINOR_MODE_SOCCER ||
        RaceManager::get()->getMinorMode() == RaceManager::MINOR_MODE_CAPTURE_THE_FLAG
    );
    for (auto p : m_peers_ready)
    {
        auto peer = p.first.lock();
	KartTeam team;
        if (!peer)
            continue;
	if (!peer->hasPlayerProfiles())
	    continue;
	team = peer->getPlayerProfiles()[0]->getTeam();
        if (ignore_ai_peer && peer->isAIPeer())
            continue;
	// player won't play the game without teams
	if (is_team_game && (team == KART_TEAM_NONE))
	    continue;
	if (!peer->alwaysSpectate())
	    return true;
    }
    return false;
}   // checkPeersCanPlay

//-----------------------------------------------------------------------------
bool ServerLobby::checkPeersReady(bool ignore_ai_peer) const
{
    bool all_ready = true;
    for (auto p : m_peers_ready)
    {
        auto peer = p.first.lock();
        if (!peer)
            continue;
        if (ignore_ai_peer && peer->isAIPeer())
            continue;
	if (peer->alwaysSpectate())
            continue;
        all_ready = all_ready && p.second;
        if (!all_ready)
            return false;
    }
    return true;
}   // checkPeersReady

// Optimization: you can go through the loop once, without the second time
//-----------------------------------------------------------------------------
char ServerLobby::checkPeersCanPlayAndReady(bool ignore_ai_peer) const
{
    const bool is_team_game = (
        RaceManager::get()->getMinorMode() == RaceManager::MINOR_MODE_SOCCER ||
        RaceManager::get()->getMinorMode() == RaceManager::MINOR_MODE_CAPTURE_THE_FLAG
    );
    // 0x1: all the players are ready, 0x2: there is at least one player that can play,
    // by default all players are ready
    char all_ready_play = !m_peers_ready.empty();
    for (auto p : m_peers_ready)
    {
        auto peer = p.first.lock();
        KartTeam team;
        if (!peer)
            continue;
        if (!peer->hasPlayerProfiles())
            continue;
        team = peer->getPlayerProfiles()[0]->getTeam();
        if (ignore_ai_peer && peer->isAIPeer())
            continue;
        auto& player = peer->getPlayerProfiles()[0];
        if (player->hasRestriction(PRF_NOGAME) ||
                player->getPermissionLevel() < PERM_PLAYER)
            continue;
        if (peer->alwaysSpectate())
            continue;
        else if(is_team_game == (team != KART_TEAM_NONE))
            all_ready_play |= 2;

        if (all_ready_play&1 && !p.second)
            all_ready_play &= ~1;
    }
    return all_ready_play;
}   // checkPeersCanPlayAndReady


//-----------------------------------------------------------------------------
void ServerLobby::handleServerCommand(Event* event,
                                      std::shared_ptr<STKPeer> peer)
{
    SoccerWorld* sw = (SoccerWorld*)World::getWorld();
    NetworkString& data = event->data();
    std::string language;
    data.decodeString(&language);
    std::string cmd;
    data.decodeString(&cmd);
    auto argv = StringUtils::split(cmd, ' ');
    bool noVeto = false;
    if (argv.size() == 0)
        return;

    std::shared_ptr<NetworkPlayerProfile> player = nullptr;
    if (peer->hasPlayerProfiles())
        player = peer->getPlayerProfiles()[0];

    if (argv[0] == "vote")
    {
        if (player->getVeto() < 80 && ServerConfig::m_command_voting == false)
        {
            std::string msg = "Command voting is disabled on this server.";
            sendStringToPeer(msg, peer);
            return;
        }
        else if (argv.size() < 2)
        {
            std::string msg = "Usage: /vote [command]";
            sendStringToPeer(msg, peer);
            return;
        }
        argv.erase(argv.begin());
        cmd = cmd.substr(5, cmd.length());
        noVeto = true;
    }

    unsigned int argv0_number;
    std::stringstream argv0_ss;
    argv0_ss << argv[0];
    argv0_ss >> argv0_number;

    if (isPoleEnabled() && argv.size() == 1 && !argv0_ss.fail())
    {
        // command is a number, /1 /2 /3 ... and pole is enabled
        submitPoleVote(peer, argv0_number);
    }
    else if (argv[0] == "spectate" || argv[0] == "s" || argv[0] == "sp" || argv[0] == "spec" || argv[0] == "spect")
    {
        if (m_game_setup->isGrandPrix() || !ServerConfig::m_live_players)
        {
            NetworkString* chat = getNetworkString();
            chat->addUInt8(LE_CHAT);
            chat->setSynchronous(true);
            std::string msg = "Server doesn't support spectate";
            chat->encodeString16(StringUtils::utf8ToWide(msg));
            peer->sendPacket(chat, true/*reliable*/);
            delete chat;
            return;
        }

        if (argv.size() != 2 || (argv[1] != "0" && argv[1] != "1") ||
            m_state.load() != WAITING_FOR_START_GAME)
        {
            NetworkString* chat = getNetworkString();
            chat->addUInt8(LE_CHAT);
            chat->setSynchronous(true);
            std::string msg = "Usage: spectate [0 or 1], before game started";
            chat->encodeString16(StringUtils::utf8ToWide(msg));
            peer->sendPacket(chat, true/*reliable*/);
            delete chat;
            return;
        }

        if (argv[1] == "1")
        {
            if (m_process_type == PT_CHILD &&
                peer->getHostId() == m_client_server_host_id.load())
            {
                NetworkString* chat = getNetworkString();
                chat->addUInt8(LE_CHAT);
                chat->setSynchronous(true);
                std::string msg = "Graphical client server cannot spectate";
                chat->encodeString16(StringUtils::utf8ToWide(msg));
                peer->sendPacket(chat, true/*reliable*/);
                delete chat;
                return;
            }
            peer->setAlwaysSpectate(ASM_COMMAND);
        }
        else
            peer->setAlwaysSpectate(ASM_NONE);
        updatePlayerList();
    }

    else if (argv[0] == "score" || argv[0] == "sc")
    {
        if (m_state.load() != RACING)
        {
            NetworkString* chat = getNetworkString();
            chat->addUInt8(LE_CHAT);
            chat->setSynchronous(true);
            std::string msg = "No on-going game!";
            chat->encodeString16(StringUtils::utf8ToWide(msg));
            peer->sendPacket(chat, true/*reliable*/);
            delete chat;
            return;
        }


        const int red_score = sw->getScore(KART_TEAM_RED);
        const int blue_score = sw->getScore(KART_TEAM_BLUE);

        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        std::string msg = "\U0001f7e5 Red " + std::to_string(red_score)+ " : " + std::to_string(blue_score) + " Blue \U0001f7e6";
        chat->encodeString16(StringUtils::utf8ToWide(msg));
        peer->sendPacket(chat, true/*reliable*/);
        delete chat;
    }

    else if (argv[0] == "teamchat" || argv[0] == "tc" || argv[0] == "tchat")
    {

        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        if (player->hasRestriction(PRF_NOCHAT) ||
                player->getPermissionLevel() <= PERM_NONE)
            chat->encodeString16(L"You are not allowed to chat, /teamchat command has no effect.");
        else
        {
            m_team_speakers.insert(peer.get());
            chat->encodeString16(L"Your messages are now addressed to team only");
        }
        peer->sendPacket(chat, true/*reliable*/);
        delete chat;
    }

    else if (argv[0] == "to" || argv[0] == "msg" || argv[0] == "dm" || argv[0] == "pm")
    {       
        if (!peer->hasPlayerProfiles())
            return;

        if (player->hasRestriction(PRF_NOPCHAT) ||
                player->getPermissionLevel() <= PERM_NONE)
        {
            NetworkString* chat = getNetworkString();
            chat->addUInt8(LE_CHAT);
            chat->setSynchronous(true);
            chat->encodeString16(L"You are not allowed to send private messages.");
            peer->sendPacket(chat, true/*reliable*/);
            delete chat; 
            return;
        }

        if (argv.size() == 1)
        {
            NetworkString* chat = getNetworkString();
            chat->addUInt8(LE_CHAT);
            chat->setSynchronous(true);
            chat->encodeString16(L"Usage: /to (username) message...");
            peer->sendPacket(chat, true/*reliable*/);
            delete chat; 
            return;
        }
        NetworkString* senderMsg = getNetworkString();
        senderMsg->addUInt8(LE_CHAT);
        senderMsg->setSynchronous(true);
        std::shared_ptr<STKPeer> target = STKHost::get()->findPeerByName(
            StringUtils::utf8ToWide(argv[1]), true/*ignoreCase*/, true/*onlyPrefix*/);
        if (!target)
        {
            senderMsg->encodeString16(L"Recipient is not online.");
            peer->sendPacket(senderMsg);
            delete senderMsg;
            return;
        }
        core::stringw msg = StringUtils::utf8ToWide(
                cmd.substr(3 + argv[1].size())
                );

        NetworkString* recipientMsg = getNetworkString();
        recipientMsg->addUInt8(LE_CHAT);
        recipientMsg->setSynchronous(true);

        // make the message for recipient
        core::stringw recipientMsgS = L"🔒 from ";
        core::stringw senderMsgS = L"🔒 to ";

        recipientMsgS += peer->getPlayerProfiles()[0]->getName();
        senderMsgS += target->getPlayerProfiles()[0]->getName();
        recipientMsgS += L": ";
        senderMsgS += L": ";
        recipientMsgS += msg;
        senderMsgS += msg;

        recipientMsg->encodeString16(recipientMsgS);
        senderMsg->encodeString16(senderMsgS);

        peer->sendPacket(senderMsg, true/*reliable*/);
        target->sendPacket(recipientMsg, true/*reliable*/);
        delete senderMsg;
        delete recipientMsg;
    }

    else if (argv[0] == "slots" || argv[0] == "sl")
    {
        if (argv.size() == 1)
        {
            std::string msg = "Please use the command in the form /slots [number]";
            sendStringToPeer(msg, peer);
            return;
        }
        if ((noVeto || player->getVeto() < 60) && m_server_owner.lock() != peer)
        {
            if (!voteForCommand(peer,cmd)) return;
        }
        else if (m_server_owner.lock() != peer &&
                player->getPermissionLevel() < 60) 
        {
            sendNoPermissionToPeer(peer.get());
            return;
        }
        int limit = std::stoi(argv[1]);

        if (limit < 6)
        {
            std::string msg = "The number of slots cannot be smaller than 6.";
            sendStringToPeer(msg, peer);
            return;
        }
        else if (limit > 12){
            std::string msg = "The number of slots cannot be larger than 12.";
            sendStringToPeer(msg, peer);
            return;
        }
        else
        {
            m_max_players_in_game = limit;
        }
        updatePlayerList();
        std::string message = "The number of slots have been changed to " + std::to_string(m_max_players_in_game)+".";
        sendStringToAllPeers(message);
    }
#if 0
    else if (argv[0] == "powerupper-on")
    {
	if (!(m_allow_powerupper)) return;
	if (m_server_owner.lock() != peer)
        {
	    if (!voteForCommand(peer,cmd)) return;
        }
	m_powerupper_active = true;
        std::string message = "The powerupper is now on.";
        sendStringToAllPeers(message);
    }

    else if (argv[0] == "powerupper-off")
    {
	if (!(m_allow_powerupper)) return;
	if (m_server_owner.lock() != peer)
        {
	    if (!voteForCommand(peer,cmd)) return;
        }
	m_powerupper_active = false;
        std::string message = "The powerupper is now off.";
        sendStringToAllPeers(message);
    }
#endif
    
    else if (argv[0] == "public" || argv[0] == "pub" || argv[0] == "all")
    {
        std::string s;
        //m_message_receivers[peer.get()].clear();
        if (player->hasRestriction(PRF_NOCHAT) ||
                player->getPermissionLevel() <= PERM_NONE)
        {
            s = "You are not allowed to chat, /public has no effect.";
        }
        else
        {
            m_team_speakers.erase(peer.get());
            s = "Your messages are now public";
        }
        sendStringToPeer(s, peer);
    }

    else if (argv[0] == "listserveraddon" || argv[0] == "lsa")
    {
        if (player->getPermissionLevel() <= PERM_NONE)
        {
            sendNoPermissionToPeer(peer.get());
            return;
        }
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        bool has_options = argv.size() > 1 &&
            (argv[1].compare("-track") == 0 ||
            argv[1].compare("-arena") == 0 ||
            argv[1].compare("-kart") == 0 ||
            argv[1].compare("-soccer") == 0);
        if (argv.size() == 1 || argv.size() > 3 || argv[1].size() < 3 ||
            (argv.size() == 2 && (argv[1].size() < 3 || has_options)) ||
            (argv.size() == 3 && (!has_options || argv[2].size() < 3)))
        {
            chat->encodeString16(
                L"Usage: /listserveraddon [option][addon string to find "
                "(at least 3 characters)]. Available options: "
                "-track, -arena, -kart, -soccer.");
        }
        else
        {
            std::string type = "";
            std::string text = "";
            if(argv.size() > 1)
            {
                if(argv[1].compare("-track") == 0 ||
                   argv[1].compare("-arena") == 0 ||
                   argv[1].compare("-kart" ) == 0 ||
                   argv[1].compare("-soccer" ) == 0)
                    type = argv[1].substr(1);
                if((argv.size() == 2 && type.empty()) || argv.size() == 3)
                    text = argv[argv.size()-1];
            }

            std::set<std::string> total_addons;
            if(type.empty() || // not specify addon type
               (!type.empty() && type.compare("kart") == 0)) // list kart addon
            {
                total_addons.insert(m_addon_kts.first.begin(), m_addon_kts.first.end());
            }
            if(type.empty() || // not specify addon type
               (!type.empty() && type.compare("track") == 0))
            {
                total_addons.insert(m_addon_kts.second.begin(), m_addon_kts.second.end());
            }
            if(type.empty() || // not specify addon type
               (!type.empty() && type.compare("arena") == 0))
            {
                total_addons.insert(m_addon_arenas.begin(), m_addon_arenas.end());
            }
            if(type.empty() || // not specify addon type
               (!type.empty() && type.compare("soccer") == 0))
            {
                total_addons.insert(m_addon_soccers.begin(), m_addon_soccers.end());
            }
            std::string msg = "";
            for (auto& addon : total_addons)
            {
                // addon_ (6 letters)
                if (!text.empty() && addon.find(text, 6) == std::string::npos)
                    continue;

                msg += addon.substr(6);
                msg += ", ";
            }
            if (msg.empty())
                chat->encodeString16(L"Addon not found");
            else
            {
                msg = msg.substr(0, msg.size() - 2);
                chat->encodeString16(StringUtils::utf8ToWide(
                    std::string("Server addon: ") + msg));
            }
        }
        peer->sendPacket(chat, true/*reliable*/);
        delete chat;
    }
    else if (StringUtils::startsWith(cmd, "playerhasaddon"))
    {
        if (!player || player->getPermissionLevel() <= PERM_NONE)
        {
            sendNoPermissionToPeer(peer.get());
            return;
        }
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        std::string part;
        if (cmd.length() > 15)
            part = cmd.substr(15);
        std::string addon_id = part.substr(0, part.find(' '));
        std::string player_name;
        if (part.length() > addon_id.length() + 1)
            player_name = part.substr(addon_id.length() + 1);
        std::shared_ptr<STKPeer> player_peer = STKHost::get()->findPeerByName(
            StringUtils::utf8ToWide(player_name));
        if (player_name.empty() || !player_peer || addon_id.empty())
        {
            chat->encodeString16(
                L"Usage: /playerhasaddon [addon_identity] [player name]");
        }
        else
        {
            std::string addon_id_test = Addon::createAddonId(addon_id);
            bool found = false;
            const auto& kt = player_peer->getClientAssets();
            for (auto& kart : kt.first)
            {
                if (kart == addon_id_test)
                {
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                for (auto& track : kt.second)
                {
                    if (track == addon_id_test)
                    {
                        found = true;
                        break;
                    }
                }
            }
            if (found)
            {
                chat->encodeString16(StringUtils::utf8ToWide
                    (player_name + " has addon " + addon_id));
            }
            else
            {
                chat->encodeString16(StringUtils::utf8ToWide
                    (player_name + " has no addon " + addon_id));
            }
        }
        peer->sendPacket(chat, true/*reliable*/);
        delete chat;
    }
    else if (argv[0] == "kick")
    {
        if (argv.size() == 1)
        {
            std::string msg = "Please use the command in the form /kick [player name]";
            sendStringToPeer(msg, peer);
            return;
        }
        if ((noVeto || player->getVeto() < 80) && m_server_owner.lock() != peer)
        {
            if (!voteForCommand(peer,cmd)) return;
        }
        else if (m_server_owner.lock() != peer &&
                (!player || player->getPermissionLevel() < 80))
        {
            sendNoPermissionToPeer(peer.get());
            return;
        }
        std::string player_name;
        if (cmd.length() > 5)
            player_name = cmd.substr(5);
        std::shared_ptr<STKPeer> player_peer = STKHost::get()->findPeerByName(
            StringUtils::utf8ToWide(player_name), true/*ignoreCase*/, true/*prefixOnly*/);
        if (player_name.empty() || !player_peer || player_peer->isAIPeer())
        {
            NetworkString* chat = getNetworkString();
            chat->addUInt8(LE_CHAT);
            chat->setSynchronous(true);
            chat->encodeString16(
                L"Usage: /kick [player name]");
            peer->sendPacket(chat, true/*reliable*/);
            delete chat;
        }
        else
        {
            player_peer->kick();
        }
    }
    else if (StringUtils::startsWith(cmd, "playeraddonscore"))
    {
        if (!player || player->getPermissionLevel() <= PERM_NONE)
        {
            sendNoPermissionToPeer(peer.get());
            return;
        }
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        std::string player_name;
        if (cmd.length() > 17)
            player_name = cmd.substr(17);
        std::shared_ptr<STKPeer> player_peer = STKHost::get()->findPeerByName(
            StringUtils::utf8ToWide(player_name));
        if (player_name.empty() || !player_peer)
        {
            chat->encodeString16(
                L"Usage: /playeraddonscore [player name] (return 0-100)");
        }
        else
        {
            auto& scores = player_peer->getAddonsScores();
            if (scores[AS_KART] == -1 && scores[AS_TRACK] == -1 &&
                scores[AS_ARENA] == -1 && scores[AS_SOCCER] == -1)
            {
                chat->encodeString16(StringUtils::utf8ToWide
                    (player_name + " has no addon"));
            }
            else
            {
                std::string msg = player_name;
                msg += " addon:";
                if (scores[AS_KART] != -1)
                    msg += " kart: " + StringUtils::toString(scores[AS_KART]) + ",";
                if (scores[AS_TRACK] != -1)
                    msg += " track: " + StringUtils::toString(scores[AS_TRACK]) + ",";
                if (scores[AS_ARENA] != -1)
                    msg += " arena: " + StringUtils::toString(scores[AS_ARENA]) + ",";
                if (scores[AS_SOCCER] != -1)
                    msg += " soccer: " + StringUtils::toString(scores[AS_SOCCER]) + ",";
                msg = msg.substr(0, msg.size() - 1);
                chat->encodeString16(StringUtils::utf8ToWide(msg));
            }
        }
        peer->sendPacket(chat, true/*reliable*/);
        delete chat;
    }
    else if (argv[0] == "serverhasaddon" || argv[0] == "sha")
    {
        if (!player || player->getPermissionLevel() <= PERM_NONE)
        {
            sendNoPermissionToPeer(peer.get());
            return;
        }
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        if (argv.size() != 2)
        {
            chat->encodeString16(
                L"Usage: /serverhasaddon [addon_identity]");
        }
        else
        {
            std::set<std::string> total_addons;
            total_addons.insert(m_addon_kts.first.begin(), m_addon_kts.first.end());
            total_addons.insert(m_addon_kts.second.begin(), m_addon_kts.second.end());
            total_addons.insert(m_addon_arenas.begin(), m_addon_arenas.end());
            total_addons.insert(m_addon_soccers.begin(), m_addon_soccers.end());
            std::string addon_id_test = Addon::createAddonId(argv[1]);
            bool found = total_addons.find(addon_id_test) != total_addons.end();
            if (found)
            {
                chat->encodeString16(StringUtils::utf8ToWide(std::string
                    ("Server has addon ") + argv[1]));
            }
            else
            {
                chat->encodeString16(StringUtils::utf8ToWide(std::string
                    ("Server has no addon ") + argv[1]));
            }
        }
        peer->sendPacket(chat, true/*reliable*/);
        delete chat;
    }
    else if (argv[0] == "feature")
    {
        if (!player || player->hasRestriction(PRF_NOPCHAT) ||
                player->getPermissionLevel() <= PERM_NONE)
        {
            sendNoPermissionToPeer(peer.get());
            return;
        }
        const size_t _cmd_size = sizeof("feature");
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        irr::core::stringw response;

        // ensure there is a message specified "feature" = 7 characters long,
        // 1 whitespace, and 5 is the minimum
        if (cmd.length() < _cmd_size + 5)
        {
            response = L"You need to specify the message that is at least 5 characters long.";
            chat->encodeString16(response);
            peer->sendPacket(chat, true/*reliable*/);
            delete chat;
            return;
        }

        // open a file, for append
        std::fstream file(
                ServerConfig::m_feature_filepath, std::ios_base::app );
        if (file.fail() || file.bad())
        {
            response = L"Failed to record a feature. Input/output error (1). Please inform the administrator.";
            chat->encodeString16(response);
            peer->sendPacket(chat, true/*reliable*/);
            delete chat;
            return;
        }
        std::string player_name;
        if (!peer->hasPlayerProfiles())
            player_name = "(unknown)";
        else
            player_name = StringUtils::wideToUtf8(peer->getPlayerProfiles()[0]->getName());

        // TODO: check if player can send the feature at all
        const std::time_t now = std::chrono::system_clock::to_time_t(
                std::chrono::system_clock::now());

        // write current date and time
        char datetime[20];
        std::strftime(datetime, 20, "%Y-%m-%d %H:%M:%S", std::localtime(&now));
        file << datetime;

        // other details
        std::string suggestionMsg = cmd.substr(_cmd_size);
        file << " [" << player_name << "]: " << suggestionMsg << std::endl;

        file.flush();
        if (!file.good())
        {
            response = "Failed to record a feature. Input/output error (2). Please inform the administrator.";
            chat->encodeString16(response);
            peer->sendPacket(chat, true/*reliable*/);
            delete chat;
            return;
        }

        // inform success
        response = "Thanks for your suggestion! Your suggestion has been recorded, and we will review it at some point.";
        chat->encodeString16(response);
        peer->sendPacket(chat, true/*reliable*/);
        delete chat;
    }
    else if (ServerConfig::m_allow_heavyparty && (argv[0] == "heavyparty" || argv[0] == "hp"))
    {
        irr::core::stringw response;
        if (argv.size() < 2 || (argv[1] != "on" && argv[1] != "off") )
        {
            auto chat = getNetworkString();
            chat->setSynchronous(true);
            chat->addUInt8(LE_CHAT);
            response = "Specify on or off as a second argument.";
            chat->encodeString16(response);
            peer->sendPacket(chat, true/*reliable*/);
            delete chat;
            return;
        }
        bool state = argv[1] == "on";

        if ((noVeto || player->getVeto() < 100) && m_server_owner.lock() != peer)
        {
            if (!voteForCommand(peer,cmd)) return;
        }
        else if (m_server_owner.lock() != peer &&
                (!player || player->getPermissionLevel() < 100))
        {
            sendNoPermissionToPeer(peer.get());
            return;
        }
        m_kart_restriction = state ? HEAVY : NONE;
        std::string message("Heavy party is now ");
        if (state)
        {
            message += "ACTIVE. Only heavy karts can be chosen.";
        }
        else
        {
            message += "INACTIVE. All karts can be chosen.";
        }

        sendStringToAllPeers(message);
    }
    else if (ServerConfig::m_allow_mediumparty && (argv[0] == "mediumparty" || argv[0] == "mp"))
    {
        irr::core::stringw response;
        if (argv.size() < 2 || (argv[1] != "on" && argv[1] != "off") )
        {
            auto chat = getNetworkString();
            chat->setSynchronous(true);
            chat->addUInt8(LE_CHAT);
            response = "Specify on or off as a second argument.";
            chat->encodeString16(response);
            peer->sendPacket(chat, true/*reliable*/);
            delete chat;
            return;
        }
        bool state = argv[1] == "on";

        if ((noVeto || player->getVeto() < 100) && m_server_owner.lock() != peer)
        {
            if (!voteForCommand(peer,cmd)) return;
        }
        else if (m_server_owner.lock() != peer &&
                (!player || player->getPermissionLevel() < 100))
        {
            sendNoPermissionToPeer(peer.get());
            return;
        }
        m_kart_restriction = state ? MEDIUM : NONE;
        std::string message("Medium party is now ");
        if (state)
        {
            message += "ACTIVE. Only medium karts can be chosen.";
        }
        else
        {
            message += "INACTIVE. All karts can be chosen.";
        }

        sendStringToAllPeers(message);
    }
    else if ((argv[0] == "scanservers" || argv[0] == "online" || argv[0] == "o")
            && ServerConfig::m_check_servers_cooldown > 0.0f)
    {
        if (!player || player->getPermissionLevel() <= PERM_NONE)
        {
            sendNoPermissionToPeer(peer.get());
            return;
        }
        NetworkString* response = getNetworkString();
        response->setSynchronous(true);
        uint64_t now = StkTime::getMonoTimeMs();
        // first, check if the timeout has not ran out yet
        if (m_last_wanrefresh_cmd_time + (uint64_t)(ServerConfig::m_check_servers_cooldown * 1000.0f)
                > now)
        {
            response->addUInt8(LE_CHAT);
            response->encodeString16(L"Someone has already used the command. Please wait before doing it again.");
            peer->sendPacket(response, true/*reliable*/);
            delete response;
            return;
        }

        // then, set current time
        m_last_wanrefresh_cmd_time = now;
        // and current sender
        m_last_wanrefresh_requester = peer;
        // and, create a request

        m_last_wanrefresh_res = ServersManager::get()->getWANRefreshRequest();
        
        response->addUInt8(LE_CHAT);
        response->encodeString16(L"Fetching, please wait...");
        peer->sendPacket(response, true/*reliable*/);
        delete response;
    }
    else if (argv[0] == "mute")
    {
        std::shared_ptr<STKPeer> player_peer;
        std::string result_msg;
        core::stringw player_name;
        NetworkString* result = NULL;

        if (argv.size() != 2 || argv[1].empty())
            goto mute_error;

        player_name = StringUtils::utf8ToWide(argv[1]);
        player_peer = STKHost::get()->findPeerByName(player_name);

        if (!player_peer || player_peer == peer)
            goto mute_error;

        m_peers_muted_players[peer].insert(player_name);
        result = getNetworkString();
        result->addUInt8(LE_CHAT);
        result->setSynchronous(true);
        result_msg = "Muted player ";
        result_msg += argv[1];
        result->encodeString16(StringUtils::utf8ToWide(result_msg));
        peer->sendPacket(result, true/*reliable*/);
        delete result;
        return;

mute_error:
        NetworkString* error = getNetworkString();
        error->addUInt8(LE_CHAT);
        error->setSynchronous(true);
        std::string msg = "Usage: /mute player_name (not including yourself)";
        error->encodeString16(StringUtils::utf8ToWide(msg));
        peer->sendPacket(error, true/*reliable*/);
        delete error;
    }
    else if (argv[0] == "unmute")
    {
        std::shared_ptr<STKPeer> player_peer;
        std::string result_msg;
        core::stringw player_name;
        NetworkString* result = NULL;

        if (argv.size() != 2 || argv[1].empty())
            goto unmute_error;

        player_name = StringUtils::utf8ToWide(argv[1]);
        for (auto it = m_peers_muted_players[peer].begin();
            it != m_peers_muted_players[peer].end();)
        {
            if (*it == player_name)
            {
                it = m_peers_muted_players[peer].erase(it);
                goto unmute_found;
            }
            else
            {
                it++;
            }
        }
        goto unmute_error;

unmute_found:
        result = getNetworkString();
        result->addUInt8(LE_CHAT);
        result->setSynchronous(true);
        result_msg = "Unmuted player ";
        result_msg += argv[1];
        result->encodeString16(StringUtils::utf8ToWide(result_msg));
        peer->sendPacket(result, true/*reliable*/);
        delete result;
        return;

unmute_error:
        NetworkString* error = getNetworkString();
        error->addUInt8(LE_CHAT);
        error->setSynchronous(true);
        std::string msg = "Usage: /unmute player_name";
        error->encodeString16(StringUtils::utf8ToWide(msg));
        peer->sendPacket(error, true/*reliable*/);
        delete error;
    }
    else if (argv[0] == "listmute")
    {
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        core::stringw total;
        for (auto& name : m_peers_muted_players[peer])
        {
            total += name;
            total += " ";
        }
        if (total.empty())
            chat->encodeString16("No player has been muted by you");
        else
        {
            total += "muted";
            chat->encodeString16(total);
        }
        peer->sendPacket(chat, true/*reliable*/);
        delete chat;
    }
    else if (argv[0] == "showcommands" || argv[0] == "commands" || argv[0] == "cmds" || argv[0] == "cmd")
    {
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        core::stringw res = (
            L"/showcommands|commands|cmds /vote /spectate|s|sp|spec|spect /score|sc /teamchat|tc|tchat "
            L"/to|msg|dm|pm /slots|sl /public|pub|all "
            L"/listserveraddon|lsa /playerhasaddon /kick /playeraddonscore /serverhasaddon|sha /feature "
            L"/heavyparty|hp /mediumparty|mp /scanservers|online /mute /unmute /listmute /pole"
        );
        chat->encodeString16(res);
        peer->sendPacket(chat, true/*reliable*/);
        delete chat;
    }
    else if (ServerConfig::m_allow_pole && (argv[0] == "pole"))
    {
        if (
            RaceManager::get()->getMinorMode() != RaceManager::MINOR_MODE_SOCCER &&
            RaceManager::get()->getMinorMode() != RaceManager::MINOR_MODE_CAPTURE_THE_FLAG
        )
        {
            NetworkString* chat = getNetworkString();
            chat->addUInt8(LE_CHAT);
            chat->setSynchronous(true);
            core::stringw msg = L"Pole only applies to team games.";
            chat->encodeString16(msg);
            peer->sendPacket(chat, true/*reliable*/);
            delete chat;

        }
        if (argv.size() < 2 || (argv[1] != "on" && argv[1] != "off") )
        {
            auto chat = getNetworkString();
            chat->setSynchronous(true);
            chat->addUInt8(LE_CHAT);
            core::stringw response = "Specify on or off as a second argument.";
            chat->encodeString16(response);
            peer->sendPacket(chat, true/*reliable*/);
            delete chat;
            return;
        }
        bool state = argv[1] == "on";

        if ((noVeto || (player && player->getVeto() < 100)) && m_server_owner.lock() != peer)
        {
            if (!voteForCommand(peer,cmd)) return;
        }
        else if (m_server_owner.lock() != peer &&
                (!player || player->getPermissionLevel() < 100))
        {
            sendNoPermissionToPeer(peer.get());
            return;
        }

        setPoleEnabled(state);
    }
    // MODERATION TOOLKIT
    else if (argv[0] == "veto")
    {
        if (!player || player->getPermissionLevel() < 50)
        {
            sendNoPermissionToPeer(peer.get());
            return;
        }
        if (argv.size() < 2)
        {
            std::string msg = "Specify on or off as a second argument.";
            sendStringToPeer(msg, peer);
        }
        if (argv[1] == "on")
        {
            player->setVeto(player->getPermissionLevel());
            std::string msg = "Forcing votable commands is now enabled.";
            sendStringToPeer(msg, peer);
        }
        else
        {
            player->setVeto(0);
            std::string msg = "Votable commands are no longer forced.";
            sendStringToPeer(msg, peer);
        }
    }
    else if (argv[0] == "ban")
    {
        std::string msg;
        if (!player || player->getPermissionLevel() < 80)
        {
            sendNoPermissionToPeer(peer.get());
            return;
        }
        if (argv.size() < 2)
        {
            msg = "Usage: /ban [player] [reason] or /ban [player] days [days] [reason]";
            sendStringToPeer(msg, peer);
            return;
        }
        std::string reason;
        int days = -1;
        if (argv.size() >= 4 && argv[2] == "days")
        {
            days = std::stoi(argv[3]);
            const size_t length = 
                argv[0].length() +
                argv[1].length() +
                argv[2].length() +
                argv[3].length() +
                // changeme
                4;
            reason = cmd.substr(std::min(length, cmd.length()), cmd.length());
        }
        else 
        {
            reason = cmd.substr(std::min(
                    argv[0].length() +
                    //                 v too
                    argv[1].length() + 2, cmd.length()), cmd.length());
        }
        // get target
        int32_t trg_permlvl = loadPermissionLevelForUsername(
            StringUtils::utf8ToWide(argv[1]));
        int32_t sender_permlvl = player->getPermissionLevel();

        if (trg_permlvl >= sender_permlvl)
        {
            msg = "You cannot ban someone who has at least your level of permissions.";
            sendStringToPeer(msg, peer);
            return;
        }

        int res;
        if ((res = banPlayer(argv[1], reason, days)) == 0)
        {
            msg = "Banned player " + argv[1] + ".";
            sendStringToPeer(msg, peer);
        }
        else if (res == 1)
        {
            msg = "Player's online id is not known in the database.";
            sendStringToPeer(msg, peer);
        }
        else if (res == 2)
        {
            msg = "Failed to ban the player, check the network console.";
            sendStringToPeer(msg, peer);
        }
    }
    else if (argv[0] == "unban" || argv[0] == "pardon")
    {
        std::string msg;
        if (!player || player->getPermissionLevel() < 80)
        {
            sendNoPermissionToPeer(peer.get());
            return;
        }
        if (argv.size() < 2)
        {
            msg = "Usage: /unban [player]";
            sendStringToPeer(msg, peer);
            return;
        }

        int res;
        if ((res = unbanPlayer(argv[1])) == 0)
        {
            msg = "Unbanned player " + argv[1] + ".";
            sendStringToPeer(msg, peer);
        }
        else if (res == 1)
        {
            msg = "Player's online id is not known in the database.";
            sendStringToPeer(msg, peer);
        }
        else if (res == 2)
        {
            msg = "Failed to unban the player, check the network console.";
            sendStringToPeer(msg, peer);
        }
    }
    else if (argv[0] == "restrict" || argv[0] == "punish")
    {
        std::string msg;
        if (!player || player->getPermissionLevel() < 80)
        {
            sendNoPermissionToPeer(peer.get());
            return;
        }
        if (argv.size() < 4)
        {
            msg = "Usage: /restrict [on/off] [nospec/nogame/nochat/nopchat/noteam/handicap/kart/track] [player]."
                " Or /restrict off all [player] to remove all restrictions.";
            sendStringToPeer(msg, peer);
            return;
        }

        PlayerRestriction restriction = getRestrictionValue(
                argv[2]);
        bool state = argv[1] == "on";

        if (restriction == PRF_OK && state)
        {
            msg = "Invalid name for restriction: " + argv[2] + ".";
            sendStringToPeer(msg, peer);
            return;
        }

        auto target = STKHost::get()->findPeerByName(
                StringUtils::utf8ToWide(argv[3]), true, true);
        int target_permlvl = loadPermissionLevelForUsername(
                StringUtils::utf8ToWide(argv[3]));
        uint32_t target_r = loadRestrictionsForUsername(
                StringUtils::utf8ToWide(argv[3])
                );
        if ((player->getPermissionLevel() < target_permlvl) &&
                ServerConfig::m_server_owner > 0 &&
                ServerConfig::m_server_owner != player->getOnlineId())
        {
            msg = "You can only apply restrictions to a player that has lower permission level than yours.";
            sendStringToPeer(msg, peer);
            return;
        }
        if (target && target->hasPlayerProfiles() &&
                target->getPlayerProfiles()[0]->getOnlineId() != 0)
        {
            auto& targetPlayer = target->getPlayerProfiles()[0];
            if (argv[1] == "off" && argv[2] == "all")
                targetPlayer->clearRestrictions();
            else if (state)
                targetPlayer->addRestriction(restriction);
            else
                targetPlayer->removeRestriction(restriction);
            writeRestrictionsForUsername(
                    targetPlayer->getName(),
                    targetPlayer->getRestrictions());
            msg = StringUtils::insertValues(
                    "Set %s to %s for player %s.",
                    getRestrictionName(restriction),
                    argv[1],
                    StringUtils::wideToUtf8(targetPlayer->getName()).c_str());
            sendStringToPeer(msg, peer);
            return;
        }
        
        uint32_t online_id = lookupOID(argv[3]);
        if (online_id)
        {
            writeRestrictionsForUsername(
                    StringUtils::utf8ToWide(argv[3]), 
                    state ? target_r | restriction : target_r & ~restriction);
            msg = StringUtils::insertValues(
                    "Set %s to %s for offline player %s.",
                    getRestrictionName(restriction),
                    argv[1],
                    argv[3].c_str());
            sendStringToPeer(msg, peer);
            return;
        }
        else
        {
            msg = "Invalid target player: " + argv[2];
            sendStringToPeer(msg, peer);
        }
    }
    else if (argv[0] == "setteam")
    {
        std::string msg;
        if (!player || player->getPermissionLevel() < 80)
        {
            sendNoPermissionToPeer(peer.get());
            return;
        }
        if (argv.size() < 3)
        {
            msg = "Usage: /setteam [red/blue/none] [player]";
            sendStringToPeer(msg, peer);
            return;
        }

        KartTeam team = KART_TEAM_NONE;
        
        if (argv[1] == "blue")
        {
            team = KART_TEAM_BLUE;
        }
        else if (argv[1] == "red")
        {
            team = KART_TEAM_RED;
        }

        std::shared_ptr<NetworkPlayerProfile> player = nullptr;
        auto peer = STKHost::get()->findPeerByName(
                StringUtils::utf8ToWide(argv[2]), true, true, &player);
        if (!player || !peer || !peer->hasPlayerProfiles())
        {
            msg = "Invalid target player: " + argv[2];
            sendStringToPeer(msg, peer);
            return;
        }
        
        player->setTeam(team);
        updatePlayerList();

        msg = "Player team has been updated.";
        sendStringToPeer(msg, peer);
    }
    else if (argv[0] == "setkart")
    {
        std::string msg;
        if (!player || player->getPermissionLevel() < 80)
        {
            sendNoPermissionToPeer(peer.get());
            return;
        }
        if (argv.size() < 3)
        {
            msg = "Usage: /setkart [kart_name or off] [player]";
            sendStringToPeer(msg, peer);
            return;
        }

        const KartProperties* kart =
            kart_properties_manager->getKart(argv[1]);
        if ((!kart || kart->isAddon()) && argv[1] != "off")
        {
            msg = "Kart does not exist or is an addon kart: " + argv[1] + ".";
            sendStringToPeer(msg, peer);
            return;
        }
        else
            kart = nullptr;

        std::shared_ptr<NetworkPlayerProfile> t_player = nullptr;
        auto t_peer = STKHost::get()->findPeerByName(
                StringUtils::utf8ToWide(argv[2]), true, true, &t_player);
        if (!t_player || !t_peer || !t_peer->hasPlayerProfiles())
        {
            msg = "Invalid target player: " + argv[2];
            sendStringToPeer(msg, peer);
            return;
        }

        if (argv[1] == "off")
        {
            std::string targetmsg = "You can choose any kart now.";
            sendStringToPeer(targetmsg, t_peer);
            t_player->unforceKart();
            msg = "No longer forcing a kart for " + argv[2] + ".";
            sendStringToPeer(msg, peer);
            return;
        }
        else
        {
            std::string targetmsg = "Your kart is " + argv[2] + " now.";
            sendStringToPeer(targetmsg, t_peer);
            t_player->forceKart(argv[2]);
            msg = StringUtils::insertValues(
                    "Made %s use kart %s.",
                    StringUtils::wideToUtf8(t_player->getName()).c_str(), argv[2]);
            sendStringToPeer(msg, peer);
        }
    }
    else if (argv[0] == "sethandicap")
    {
        std::string msg;
        if (!player || player->getPermissionLevel() < 80)
        {
            sendNoPermissionToPeer(peer.get());
            return;
        }
        if (argv.size() < 3)
        {
            msg = "Usage: /sethandicap [none/count/medium] [player]";
            sendStringToPeer(msg, peer);
            return;
        }

        HandicapLevel handicap = HANDICAP_NONE;
        
        if (argv[1] == "count")
        {
            handicap = HANDICAP_COUNT;
        }
        else if (argv[1] == "medium")
        {
            handicap = HANDICAP_MEDIUM;
        }
        else if (argv[1] != "none")
        {
            msg = "Specify either count, medium or none for second argument.";
            sendStringToPeer(msg, peer);
            return;
        }

        std::shared_ptr<NetworkPlayerProfile> player = nullptr;
        auto peer = STKHost::get()->findPeerByName(
                StringUtils::utf8ToWide(argv[2]), true, true, &player);
        if (!player || !peer || !peer->hasPlayerProfiles())
        {
            msg = "Invalid target player: " + argv[2];
            sendStringToPeer(msg, peer);
            return;
        }
        
        player->setHandicap(handicap);
        updatePlayerList();

        msg = "Player handicap has been updated.";
        sendStringToPeer(msg, peer);
    }
    else
    {
        std::string msg = "Unknown command: ";
        msg += cmd;
        sendStringToPeer(msg, peer);
    }
}   // handleServerCommand

//-----------------------------------------------------------------------------
void ServerLobby::sendStringToPeer(const std::string& s, std::shared_ptr<STKPeer>& peer) const
{
    if (peer==NULL)
    {
        Log::info("ServerLobby",s.c_str());
        return;
    }
    NetworkString* chat = getNetworkString();
    chat->addUInt8(LE_CHAT);
    chat->setSynchronous(true);
    chat->encodeString16(StringUtils::utf8ToWide(s));
    peer->sendPacket(chat, true/*reliable*/);
    delete chat;
}   // sendStringToPeer
//-----------------------------------------------------------------------------
void ServerLobby::sendStringToPeer(const irr::core::stringw& s, std::shared_ptr<STKPeer>& peer) const
{
    if (peer==NULL)
    {
        Log::info("ServerLobby",StringUtils::wideToUtf8(s).c_str());
        return;
    }
    NetworkString* chat = getNetworkString();
    chat->addUInt8(LE_CHAT);
    chat->setSynchronous(true);
    chat->encodeString16(s);
    peer->sendPacket(chat, true/*reliable*/);
    delete chat;
}   // sendStringToPeer

//-----------------------------------------------------------------------------
void ServerLobby::sendStringToAllPeers(std::string& s)
{
    Log::info("ServerLobby",s.c_str());
    NetworkString* chat = getNetworkString();
    chat->addUInt8(LE_CHAT);
    chat->setSynchronous(true);
    chat->encodeString16(StringUtils::utf8ToWide(s));
    sendMessageToPeers(chat, true/*reliable*/);
    delete chat;
}   // sendStringToAllPeers

bool ServerLobby::voteForCommand(std::shared_ptr<STKPeer>& peer, std::string command)
{
    if (!ServerConfig::m_command_voting) return false;

    std::string username = StringUtils::wideToUtf8(peer->getPlayerProfiles()[0]->getName());
    int playerCount = STKHost::get()->getPeers().size();

    if (m_command_voters.count(command) == 0)
    {
        m_command_voters[command] = std::vector<std::string>();
    }

    if (std::find(m_command_voters[command].begin(), m_command_voters[command].end(), username) != m_command_voters[command].end())
    {
        std::string msg = "You already voted for \"" + command + "\".";
        sendStringToPeer(msg, peer);
    }
    else
    {
        m_command_voters[command].push_back(username);
        std::string message = username + " voted for \"/" + command + "\" (" + std::to_string(m_command_voters[command].size()) + " of " + std::to_string(playerCount) + " votes).";
        sendStringToAllPeers(message);
        Log::info("ServerLobby", message.c_str());
    }


    if (m_command_voters[command].size() > (playerCount / 2))
    {
        m_command_voters.erase(command);
        return true;
    }

    return false;
} // voteForCommand
//-----------------------------------------------------------------------------
// Specify default argument to determine current mode from server config
const std::string ServerLobby::getRandomAddon(RaceManager::MinorRaceModeType m) const
{
    const std::set<std::string>* addon_list;

    m = m != RaceManager::MINOR_MODE_NONE ? m : RaceManager::get()->getMinorMode();
    switch (m)
    {
        case RaceManager::MINOR_MODE_NORMAL_RACE:
        case RaceManager::MINOR_MODE_TIME_TRIAL:
        case RaceManager::MINOR_MODE_FOLLOW_LEADER:
	    addon_list = &m_addon_kts.second;
            break;
        case RaceManager::MINOR_MODE_FREE_FOR_ALL:
        case RaceManager::MINOR_MODE_CAPTURE_THE_FLAG:
	    addon_list = &m_addon_arenas;
            break;
        case RaceManager::MINOR_MODE_SOCCER:
	    addon_list = &m_addon_soccers;
            break;
        default:
            assert(false);
	    return "";
            break;
    }

    if (addon_list->empty())
    {
        return "(no-addons-installed)";
    }
    
    RandomGenerator rg;
    std::set<std::string>::const_iterator it = addon_list->cbegin();
    std::advance(it, rg.get((int)addon_list->size()));
    std::string result = *it; 
    // remove "addon_" prefix
    result.erase(0, 6);

    return result;
} // getRandomSoccerAddon

//------------------------------------------------------------------------------------------
core::stringw ServerLobby::formatTeammateList(
    const std::vector<std::shared_ptr<NetworkPlayerProfile>> &team) const
{
    core::stringw res;
    for (unsigned int i = 0; i < team.size(); ++i)
    {
        res += L"/";
        res += core::stringw(i + 1);
        res += L" to vote for ";
        res += team[i]->getName();
        res += L"\n";
    }
    return res;
} // formatTeammateList

//------------------------------------------------------------------------------------------
void ServerLobby::setPoleEnabled(bool mode)
{
    m_blue_pole_votes.clear();
    m_red_pole_votes.clear();

    if (m_pole_enabled == mode)
        return;
    m_pole_enabled = mode;

    STKHost* const host = STKHost::get();
    RaceManager* const race_manager = RaceManager::get();
    host->setForcedFirstPlayer(nullptr);
    host->setForcedSecondPlayer(nullptr);
    race_manager->clearPoles();

    if (mode)
    {
        resetPeersReady();

        std::vector<std::shared_ptr<NetworkPlayerProfile>>
            team_blue, team_red;

        host->getTeamLists(team_blue, team_red);

        // send message to team members
        const core::stringw header =
            L"Pole vote has been opened. Please vote for the teammate that will be at the "
            L"most front position towards the ball or puck:\n";

        core::stringw msg_red = header, msg_blue = header;

        msg_blue += formatTeammateList(team_blue);
        msg_red += formatTeammateList(team_red);

        NetworkString* const pkt_blue = getNetworkString();
        NetworkString* const pkt_red = getNetworkString();

        pkt_blue->setSynchronous(true);
        pkt_blue->addUInt8(LE_CHAT);
        pkt_red->setSynchronous(true);
        pkt_red->addUInt8(LE_CHAT);
        pkt_blue->encodeString16(msg_blue);
        pkt_red->encodeString16(msg_red);

        host->sendPacketToAllPeersWith([host](STKPeer* p)
                {
                    if (p->isAIPeer() || !p->isConnected() || !p->isValidated()) return false;
                    return host->isPeerInTeam(p, KART_TEAM_BLUE);
                }, pkt_blue);
        host->sendPacketToAllPeersWith([host](STKPeer* p)
                {
                    if (p->isAIPeer() || !p->isConnected() || !p->isValidated()) return false;
                    return host->isPeerInTeam(p, KART_TEAM_RED);
                }, pkt_red);

        delete pkt_blue;
        delete pkt_red;
    }
    else 
    {
        std::string resp("Pole has been disabled.");
        sendStringToAllPeers(resp);
    }
} // setPoleEnabled

//------------------------------------------------------------------------------------------
void ServerLobby::submitPoleVote(std::shared_ptr<STKPeer>& voter, const unsigned int vote)
{
    STKPeer* const voter_p = voter.get();

    if (!voter->hasPlayerProfiles())
        return;

    if (m_state.load() != WAITING_FOR_START_GAME)
    {
        sendStringToPeer(L"You can only vote for the poles before the game started.", voter);
        return;
    }

    std::shared_ptr<NetworkPlayerProfile> voter_profile = voter->getPlayerProfiles()[0];
    const KartTeam team = voter_profile->getTeam();
    std::map<STKPeer*, std::weak_ptr<NetworkPlayerProfile>>*
        mapping = &m_blue_pole_votes;
    if (team == KART_TEAM_NONE)
    {
        sendStringToPeer(L"You need to be in the team in order to vote for the pole.", voter);
        return;
    }

    if (team == KART_TEAM_RED)
        mapping = &m_red_pole_votes;

    auto teammates = STKHost::get()->getPlayerProfilesOfTeam(team);

    if (vote == 0 || teammates.size() == 0 || vote > teammates.size())
    {
        sendStringToPeer(L"Out of range. Please select one of the listed teammates.", voter);
        return;
    }

    std::weak_ptr<NetworkPlayerProfile> teammate = teammates[vote - 1];
    auto teammate_p = teammate.lock();
    const core::stringw& tmName = teammate_p->getName();
    (*mapping)[voter_p] = teammate;
    core::stringw msg = voter_profile->getName();
    msg += L" voted for ";
    msg += tmName;
    msg += L" to be the pole.";

    NetworkString* const packet = getNetworkString();
    packet->setSynchronous(true);
    packet->addUInt8(LE_CHAT);
    packet->encodeString16(msg);

    STKHost::get()->sendPacketToAllPeersWith(
            [team](STKPeer* p)
            {
                if (p->isAIPeer()) return false;
                return STKHost::get()->isPeerInTeam(p, team);
            }, packet);
    delete packet;

} // submitPoleVote

//-----------------------------------------------------------------------------------------

std::shared_ptr<NetworkPlayerProfile> ServerLobby::decidePoleFor(
        const PoleVoterMap& mapping, const KartTeam team) const
{
    std::shared_ptr<NetworkPlayerProfile> npp, max_npp;
    unsigned max_npp_c = 0;
    std::map<std::shared_ptr<NetworkPlayerProfile>, unsigned> res;

    for (auto entry : mapping)
    {
        if (entry.second.expired())
            continue;

        npp = entry.second.lock();
        if (npp->getTeam() != team)
            continue;

        auto rentry = res.find(npp);
        if (rentry == res.cend())
        {
            res[npp] = 1;
            if (max_npp_c < 1)
            {
                max_npp_c = 1;
                max_npp = npp;
            }
        }
        else
        {
            res[npp] += 1;
            if (max_npp_c < res[npp])
            {
                max_npp_c = res[npp];
                max_npp = npp;
            }
        }
    }

    if (res.size() == 0)
        return nullptr;

    /*return std::max_element(
            res.cbegin(), res.cend(),
            [](const PoleVoterResultEntry& a,
               const PoleVoterResultEntry& b)
            {
                return a.second < b.second;
            })->first;*/
    return max_npp;
} // countPoleVotes
//-----------------------------------------------------------------------------------------
std::pair<
    std::shared_ptr<NetworkPlayerProfile>,
    std::shared_ptr<NetworkPlayerProfile>>
ServerLobby::decidePoles()
{
    std::shared_ptr<NetworkPlayerProfile> blue, red;

    blue = decidePoleFor(m_blue_pole_votes, KART_TEAM_BLUE);
    red = decidePoleFor(m_red_pole_votes, KART_TEAM_RED);

    return std::make_pair(blue, red);
} // decidePoles

void ServerLobby::announcePoleFor(std::shared_ptr<NetworkPlayerProfile>& pole, const KartTeam team) const
{
    if (team == KART_TEAM_NONE || pole == nullptr)
        return;

    core::stringw msgS = L"Current pole player is ";
    msgS += pole->getName();
    NetworkString* const msg = getNetworkString();
    msg->setSynchronous(true);
    msg->addUInt8(LE_CHAT);
    msg->encodeString16(msgS);

    STKHost::get()->sendPacketToAllPeersWith(
            [team](STKPeer* p)
            {
                if (!p->isConnected() || !p->hasPlayerProfiles() || p->isAIPeer() || !p->isValidated() ||
                        p->isWaitingForGame())
                    return false;

                return STKHost::get()->isPeerInTeam(p, team);
            }, msg);
    
    delete msg;
}

NetworkString* ServerLobby::addRandomInstalladdonMessage(NetworkString* const ril_pkt) const
{
    if (!ServerConfig::m_enable_ril)
        return ril_pkt;
    std::string ril_prefix = ServerConfig::m_ril_prefix;
    std::string msg(ril_prefix + " ");
    // choose an addon
    msg += getRandomAddon();

    ril_pkt->addUInt8(LE_CHAT);
    ril_pkt->encodeString16(StringUtils::utf8ToWide(msg));
    //peer->sendPacket(ril_pkt, true*reliable*);
    return ril_pkt;
}
//-----------------------------------------------------------------------------
void ServerLobby::sendRandomInstalladdonLine(STKPeer* const peer) const
{
    if (ServerConfig::m_enable_ril)
    {
        NetworkString* ril_pkt = getNetworkString();
        ril_pkt->setSynchronous(true);
        addRandomInstalladdonMessage(ril_pkt);

        peer->sendPacket(ril_pkt, true/*reliable*/);
        delete ril_pkt;
    }
} // sendRandomInstalladdonLine
//-----------------------------------------------------------------------------
void ServerLobby::sendRandomInstalladdonLine(std::shared_ptr<STKPeer> const peer) const
{
    if (ServerConfig::m_enable_ril)
    {
	NetworkString* ril_pkt = getNetworkString();
	ril_pkt->setSynchronous(true);
	addRandomInstalladdonMessage(ril_pkt);

        peer->sendPacket(ril_pkt, true/*reliable*/);
	delete ril_pkt;
    }
} // sendRandomInstalladdonLine
void ServerLobby::sendCurrentModifiers(STKPeer* const peer) const
{
    NetworkString* pkt = getNetworkString();
    pkt->setSynchronous(true);

    // add stuff here
    addKartRestrictionMessage(pkt);

    peer->sendPacket(pkt, true/*reliable*/);
    delete pkt;
}
void ServerLobby::sendCurrentModifiers(std::shared_ptr<STKPeer>& peer) const
{
    NetworkString* pkt = getNetworkString();
    pkt->setSynchronous(true);

    // add stuff here
    addKartRestrictionMessage(pkt);

    peer->sendPacket(pkt, true/*reliable*/);
    delete pkt;
}
NetworkString* ServerLobby::addKartRestrictionMessage(NetworkString* ns) const
{
    if (m_kart_restriction == NONE)
        return ns;

    ns->addUInt8(LE_CHAT);
    core::stringw msg = L"\n---===---\n";
    switch (m_kart_restriction)
    {
        case HEAVY:
            msg += L"HEAVY PARTY is ACTIVE! Only heavy karts can be chosen";
            break;
        case MEDIUM:
            msg += L"MEDIUM PARTY is ACTIVE! Only medium karts can be chosen";
            break;
        case LIGHT:
            msg += L"LIGHT INSURANCE is ACTIVE! Only light karts can be chosen, to ensure better experience.";
            break;
        case NONE:
            break;
    }
    msg += L"\n---===---";
    ns->encodeString16(msg);
    return ns;
}
int ServerLobby::getPeerPermissionLevel(STKPeer* p)
{
    if (!p->hasPlayerProfiles()) 
    {
        return PERM_NONE;
    }
    return p->getPlayerProfiles()[0]->getPermissionLevel();
}
int ServerLobby::loadPermissionLevelForOID(const uint32_t online_id)
{
#ifdef ENABLE_SQLITE3
    if (!m_db || !m_permissions_table_exists)
        return 0;

    if (ServerConfig::m_server_owner != -1 
            && online_id == ServerConfig::m_server_owner)
        return std::numeric_limits<int>::max();

    int lvl = 0;
    char* errmsg;
    std::string query = StringUtils::insertValues(
        "SELECT level FROM %s WHERE online_id = %u;",
        ServerConfig::m_permissions_table.c_str(),
        online_id);
    sqlite3_exec(m_db, query.c_str(),
            [](void* ptr, int amount, char** data, char** columns) {
                int* target = (int*)ptr;
                *target = std::atoi(data[0]);
                return 0;
            }, &lvl, &errmsg);
    if (errmsg)
    {
        Log::error("ServerLobby", "loadPermissionLevelForOID failure: %s", errmsg);
        sqlite3_free(errmsg);
        return 0;
    }
    return lvl;
#else
    return 0;
#endif
}
void ServerLobby::writePermissionLevelForOID(const uint32_t online_id, const int lvl)
{
#ifdef ENABLE_SQLITE3
    if (!m_db || !m_permissions_table_exists)
        return;

    std::string query = StringUtils::insertValues(
        "INSERT INTO %s (online_id, level) VALUES (%u, %d) "
        "ON CONFLICT (online_id) DO UPDATE SET level = %d;",
        ServerConfig::m_permissions_table.c_str(),
        online_id, lvl, lvl
    );
    easySQLQuery(query);
#endif
}
void ServerLobby::writePermissionLevelForUsername(const core::stringw& name, const int lvl)
{
#ifdef ENABLE_SQLITE3
    if (!m_db || !m_permissions_table_exists)
        return;

    std::string query = StringUtils::insertValues(
        "INSERT INTO %s (online_id, level) "
        "SELECT online_id, %d AS level FROM %s WHERE "
        "username = ?"
        "ON CONFLICT (online_id) DO UPDATE SET level = %d;",
        (std::string) ServerConfig::m_permissions_table.c_str(),
        lvl, m_server_stats_table, lvl
    );
    easySQLQuery(query,
        [name](sqlite3_stmt* stmt)
        {
            if ((sqlite3_bind_text(stmt, 1,
                    StringUtils::wideToUtf8(name).c_str(),
                    -1, SQLITE_TRANSIENT))
                    != SQLITE_OK)
            {
                Log::error("easySQLQuery", "Failed to bind %s.",
                    name.c_str());
            }
            return 0;
        });
#endif
}
uint32_t ServerLobby::loadRestrictionsForOID(const uint32_t online_id)
{
#ifdef ENABLE_SQLITE3
    if (!m_db || !m_restrictions_table_exists)
        return 0;

    int res;
    uint32_t lvl;
    char* errmsg;
    std::string query = StringUtils::insertValues(
        "SELECT flags FROM %s WHERE online_id = %u;",
        ServerConfig::m_restrictions_table.c_str(),
        online_id);
    res = sqlite3_exec(m_db, query.c_str(),
            [](void* ptr, int amount, char** data, char** columns) {
                uint32_t* target = (uint32_t*)ptr;
                *target = std::atol(data[0]);
                return 0;
            }, &lvl, &errmsg);
    if (res != SQLITE_OK && errmsg)
    {
        Log::error("ServerLobby", "loadRestrictionsForOID failure: %s", errmsg);
        sqlite3_free(errmsg);
        return 0;
    }
    return lvl;
#else
    return 0;
#endif
}
uint32_t ServerLobby::loadRestrictionsForUsername(const core::stringw& name)
{
#ifdef ENABLE_SQLITE3
    if (!m_db || !m_restrictions_table_exists)
        return 0;

    int res;
    std::string query = StringUtils::insertValues(
        "SELECT flags FROM %s AS r INNER JOIN %s AS s ON (r.online_id = s.online_id) WHERE username = ?;",
        ServerConfig::m_restrictions_table.c_str(),
        m_server_stats_table);
    sqlite3_stmt* stmt = NULL;
    res = sqlite3_prepare_v2(m_db, query.c_str(), query.size(), &stmt, NULL);
    if (res != SQLITE_OK || !stmt)
    {
        Log::error("ServerLobby", "loadRestrictionsForUsername failure: %s",
                sqlite3_errmsg(m_db));
        return PRF_OK;
    }
    res = sqlite3_bind_text(stmt, 1, 
            StringUtils::wideToUtf8(name).c_str(), -1, SQLITE_TRANSIENT);
    if (res != SQLITE_OK)
    {
        Log::error("ServerLobby::loadRestrictionsForUsername", "Failed to bind %s.",
            name.c_str());
        return PRF_OK;
    }

    res = sqlite3_step(stmt);
    if (res == SQLITE_DONE)
    {
        sqlite3_finalize(stmt);
        return PRF_OK;
    }
    if (res == SQLITE_ROW)
    {
        uint32_t flags = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        return flags;
    }
    Log::error("ServerLobby", "loadRestrictionsForUsername failed to dispatch: %s",
            sqlite3_errmsg(m_db));
    sqlite3_finalize(stmt);
    return PRF_OK;
#else
    return 0;
#endif
}
void ServerLobby::writeRestrictionsForOID(const uint32_t online_id, const uint32_t flags)
{
#ifdef ENABLE_SQLITE3
    if (!m_db || !m_restrictions_table_exists)
        return;

    std::string query = StringUtils::insertValues(
        "INSERT INTO %s (online_id, flags) VALUES (%u, %u) "
        "ON CONFLICT (online_id) DO UPDATE SET flags = %u;",
        ServerConfig::m_restrictions_table.c_str(),
        online_id, flags, flags
    );
    easySQLQuery(query);
#endif
}
void ServerLobby::writeRestrictionsForUsername(const core::stringw& name, const uint32_t flags)
{
#ifdef ENABLE_SQLITE3
    if (!m_db || !m_restrictions_table_exists)
        return;

    std::string query = StringUtils::insertValues(
        "INSERT INTO %s (online_id, flags) "
        "SELECT online_id, %d AS flags FROM %s WHERE "
        "username = ?"
        "ON CONFLICT (online_id) DO UPDATE SET flags = %d;",
        ServerConfig::m_restrictions_table.c_str(),
        flags, m_server_stats_table, flags
    );
    easySQLQuery(query,
        [name](sqlite3_stmt* stmt)
        {
            if ((sqlite3_bind_text(stmt, 1,
                    StringUtils::wideToUtf8(name).c_str(),
                    -1, SQLITE_TRANSIENT))
                    != SQLITE_OK)
            {
                Log::error("easySQLQuery", "Failed to bind %s.",
                    name.c_str());
            }
            return 0;
        });
#endif
}
void ServerLobby::sendNoPermissionToPeer(STKPeer* p)
{
    NetworkString* const msg = getNetworkString();
    msg->setSynchronous(true);
    msg->addUInt8(LE_CHAT).encodeString16(
            L"You are not allowed to run this command.");
    p->sendPacket(msg, true/*reliable*/);
    delete msg;
}
uint32_t ServerLobby::lookupOID(const std::string& name)
{
#ifdef ENABLE_SQLITE3
    if (name.empty() || !m_db)
        return 0;

    std::string query = StringUtils::insertValues(
        "SELECT online_id FROM %s WHERE username = ? LIMIT 1;",
        m_server_stats_table
    );
    sqlite3_stmt* stmt = NULL;
    int res = sqlite3_prepare_v2(m_db, query.c_str(), query.size(), &stmt, NULL);
    if (res != SQLITE_OK || !stmt)
    {
        Log::error("ServerLobby", "Error in lookupOID, sqlite3_prepare_v2 returned %d: %s",
                res, sqlite3_errmsg(m_db));
        return 0;
    }
    res = sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    if (res != SQLITE_OK)
    {
        Log::error("ServerLobby::lookupOID", "Failed to bind %s.",
            name.c_str());
        return 0;
    }

    res = sqlite3_step(stmt);
    if (res == SQLITE_ROW)
    {
        uint32_t ret = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        return ret;
    }
    if (res == SQLITE_DONE)
    {
        sqlite3_finalize(stmt);
        // not found
        return 0;
    }
    // error occurred
    Log::error("ServerLobby", "Error in lookupOID, step returned %d: %s",
            res, sqlite3_errmsg(m_db));
    sqlite3_finalize(stmt);
#endif
    return 0;
}
uint32_t ServerLobby::lookupOID(const core::stringw& name)
{
#ifdef ENABLE_SQLITE3
    if (name.empty() || !m_db)
        return 0;

    std::string query = StringUtils::insertValues(
        "SELECT online_id FROM %s WHERE username = ? LIMIT 1;",
        m_server_stats_table
    );
    sqlite3_stmt* stmt = NULL;
    int res = sqlite3_prepare_v2(m_db, query.c_str(), query.size(), &stmt, NULL);
    if (res != SQLITE_OK || !stmt)
    {
        Log::error("ServerLobby", "Error in lookupOID, sqlite3_prepare_v2 returned %d: %s",
                res, sqlite3_errmsg(m_db));
        return 0;
    }
    res = sqlite3_bind_text(stmt, 1, StringUtils::wideToUtf8(name).c_str(), -1, SQLITE_TRANSIENT);
    if (res != SQLITE_OK)
    {
        Log::error("ServerLobby::lookupOID", "Failed to bind %s.",
            name.c_str());
        return 0;
    }

    res = sqlite3_step(stmt);
    if (res == SQLITE_ROW)
    {
        uint32_t ret = sqlite3_column_int(stmt, 1);
        sqlite3_finalize(stmt);
        return ret;
    }
    if (res == SQLITE_DONE)
    {
        sqlite3_finalize(stmt);
        // not found
        return 0;
    }
    // error occurred
    Log::error("ServerLobby", "Error in lookupOID, step returned %d: %s",
            res, sqlite3_errmsg(m_db));
    sqlite3_finalize(stmt);
#endif
    return 0;
}
int ServerLobby::banPlayer(const std::string& name, const std::string& reason, const int days)
{
#ifdef ENABLE_SQLITE3
    if (!m_db || !m_online_id_ban_table_exists)
        return -1;

    if (name.empty())
        return 1;
    
    std::string query = StringUtils::insertValues(
            "INSERT INTO %s (online_id, reason, expired_days) "
            "SELECT online_id, ?1 AS reason, ?2 AS expired_days FROM %s "
            "WHERE online_id > 0 AND username = ?3 ON CONFLICT (online_id) DO "
            "UPDATE SET reason = ?1, expired_days = ?2;",
        ServerConfig::m_online_id_ban_table.c_str(),
        m_server_stats_table
    );
    sqlite3_stmt* stmt = NULL;
    int res = sqlite3_prepare_v2(m_db, query.c_str(), query.size(), &stmt, NULL);
    if (res != SQLITE_OK || !stmt)
    {
        Log::error("ServerLobby", "Error in banPlayer, sqlite3_prepare_v2 returned %d: %s",
                res, sqlite3_errmsg(m_db));
        return 2;
    }
    if (reason.empty())
    {
        res = sqlite3_bind_null(stmt, 1);
        if (res != SQLITE_OK)
        {
            Log::error("ServerLobby::banPlayer", "Failed to bind arg #1 (null).");
            return 2;
        }
    }
    else
    {
        res = sqlite3_bind_text(stmt, 1, reason.c_str(), -1, SQLITE_TRANSIENT);
        if (res != SQLITE_OK)
        {
            Log::error("ServerLobby::banPlayer", "Failed to bind arg #1.");
            return 2;
        }
    }
    if (days < 0)
    {
        res = sqlite3_bind_null(stmt, 2);
        if (res != SQLITE_OK)
        {
            Log::error("ServerLobby::banPlayer", "Failed to bind arg #2 (null).");
            return 2;
        }
    }
    else
    {
        res = sqlite3_bind_int(stmt, 2, days);
        if (res != SQLITE_OK)
        {
            Log::error("ServerLobby::banPlayer", "Failed to bind arg #2.");
            return 2;
        }
    }
    res = sqlite3_bind_text(stmt, 3, name.c_str(), -1, SQLITE_TRANSIENT);
    if (res != SQLITE_OK)
    {
        Log::error("ServerLobby::banPlayer", "Failed to bind arg #3.");
        return 2;
    }

    res = sqlite3_step(stmt);
    if (res != SQLITE_DONE)
    {
        Log::error("ServerLobby", "Error in banPlayer, step returned %d: %s",
            res, sqlite3_errmsg(m_db));
        sqlite3_finalize(stmt);
        return 2;
    }
    if ((res = sqlite3_changes(m_db)) == 0)
    {
        sqlite3_finalize(stmt);
        // nothing is done
        return 1;
    }

    sqlite3_finalize(stmt);
    // player is banned, attempt to kick the player if there's one online

    std::shared_ptr<STKPeer> peer = STKHost::get()->findPeerByName(
            StringUtils::utf8ToWide(name));
    if (peer)
        peer->kick();

    return 0;
#else
    return -2;
#endif
}
int ServerLobby::unbanPlayer(const std::string& name)
{
#ifdef ENABLE_SQLITE3
    if (!m_db || !m_online_id_ban_table_exists)
        return -2;

    if (name.empty())
        return 1;

    std::string query = StringUtils::insertValues(
            "DELETE FROM %s WHERE online_id IN ("
            "SELECT online_id FROM %s "
            "WHERE online_id > 0 AND username = ? LIMIT 1);",
        ServerConfig::m_online_id_ban_table.c_str(),
        m_server_stats_table
    );
    sqlite3_stmt* stmt = NULL;
    int res = sqlite3_prepare_v2(m_db, query.c_str(), query.size(), &stmt, NULL);
    if (res != SQLITE_OK || !stmt)
    {
        Log::error("ServerLobby", "Error in unbanPlayer, sqlite3_prepare_v2 returned %d: %s",
                res, sqlite3_errmsg(m_db));
        return 2;
    }
    res = sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    if (res != SQLITE_OK)
    {
        Log::error("ServerLobby::unbanPlayer", "Failed to bind arg #1.");
        return 2;
    }

    res = sqlite3_step(stmt);
    if (res != SQLITE_DONE)
    {
    Log::error("ServerLobby", "Error in unbanPlayer, step returned %d: %s",
            res, sqlite3_errmsg(m_db));
        sqlite3_finalize(stmt);
        return 2;
    }
    if ((res = sqlite3_changes(m_db)) == 0)
    {
        sqlite3_finalize(stmt);
        // nothing is done, which means player wasn't banned
        return 1;
    }
#if 0
    if (res > 1)
    {
        // multiple players are banned?
        Log::error("ServerLobby::unbanPlayer",
                "Multiple players were unbanned (%d rows affected)", res);
        sqlite3_finalize(stmt);
        return 2;
    }
#endif

    sqlite3_finalize(stmt);
    return 0;
#else
    return -2;
#endif
}
const std::string ServerLobby::formatBanList(unsigned int page,
        const unsigned int psize)
{
#ifdef ENABLE_SQLITE3
    if (!m_db || !m_online_id_ban_table_exists || !psize)
        return "";

    // get number of pages first
    std::string query_agg = StringUtils::insertValues(
            "SELECT (count(*) / 8 + iif(count(*) %% 8 > 0, 1, 0)) AS num_pages FROM %s;",
            ServerConfig::m_online_id_ban_table.c_str());
    sqlite3_stmt* stmt = NULL;
    int res = sqlite3_prepare_v2(m_db, query_agg.c_str(), query_agg.size(), &stmt, NULL);
    if (res != SQLITE_OK || !stmt)
    {
        Log::error("ServerLobby::formatBanList", "Unable to prepare the statement: %d, %s",
                res, sqlite3_errmsg(m_db));
        return "";
    }
    
    unsigned int pages = 0;
    res = sqlite3_step(stmt);
    if (res == SQLITE_DONE)
    {
        sqlite3_finalize(stmt);
        return "No players have been banned.\n(Page 1 of 1)";
    }
    if (res == SQLITE_ROW)
    {
        sqlite3_finalize(stmt);
        pages = sqlite3_column_int64(stmt, 0);
    }
    else
    {
        Log::error("ServerLobby::formatBanList", "Unable to execute the statement: %d, %s",
                res, sqlite3_errmsg(m_db));
        sqlite3_finalize(stmt);
        return "";
    }
    sqlite3_finalize(stmt);

    clamp<unsigned>(page, 1, pages);

    // aggregate information acquired
    std::string query = StringUtils::insertValues(
            "SELECT DISTINCT b.online_id, s.username, reason, expired_days FROM %s AS b"
            "RIGHT OUTER JOIN %s AS s ON (s.online_id = b.online_id) "
            "LIMIT %u OFFSET %u * %u;",
            ServerConfig::m_online_id_ban_table.c_str(),
            m_server_stats_table, psize, psize, page);
    
    stmt = NULL;
    res = sqlite3_prepare_v2(m_db, query.c_str(), query.size(), &stmt, NULL);
    if (res != SQLITE_OK || !stmt)
    {
        Log::error("ServerLobby::formatBanList", "Unable to prepare the statement: %d, %s",
                res, sqlite3_errmsg(m_db));
        return "";
    }

    std::string result = StringUtils::insertValues(
            "Online ID bans (page %d of %d):\n", page, pages
            );

    while ((res = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        unsigned int online_id = sqlite3_column_int(stmt, 0);
        const unsigned char* username = sqlite3_column_text(stmt, 1);
        const unsigned char* reason;
        if (sqlite3_column_type(stmt, 2) == SQLITE_NULL)
        {
            reason = (const unsigned char*)"[UNSPECIFIED]";
        }
        else
        {
            reason = sqlite3_column_text(stmt, 2);
        }

        result += StringUtils::insertValues(
                "[%u] %s: %s", online_id, username, reason);

        if (sqlite3_column_type(stmt, 3) != SQLITE_NULL)
        {
            result += (std::string(" (expires in ") +
                    std::to_string(sqlite3_column_int(stmt, 3))
                    + " days).");
        }
        result += "\n";
    }
    if (res != SQLITE_ROW)
    {
        Log::error("ServerLobby", "Could not make a proper ban list with consistency... "
                "sqlite3_step returns %d", res);
    }
    sqlite3_finalize(stmt);
    return result;
#else
    return "";
#endif
}
const std::string ServerLobby::formatBanInfo(const std::string& name)
{
    if (!m_db || !m_online_id_ban_table_exists || name.empty())
        return "";

    // get number of pages first
    std::string query_agg = StringUtils::insertValues(
            "SELECT DISTINCT b.online_id, s.username, reason, expired_days FROM %s "
            "INNER JOIN %s AS s ON (b.online_id = s.online_id) WHERE s.username = ?;",
            ServerConfig::m_online_id_ban_table.c_str(),
            m_server_stats_table);
    sqlite3_stmt* stmt = NULL;
    int res = sqlite3_prepare_v2(m_db, query_agg.c_str(), query_agg.size(), &stmt, NULL);
    if (res != SQLITE_OK || !stmt)
    {
        Log::error("ServerLobby::formatBanList", "Unable to prepare the statement: %d, %s",
                res, sqlite3_errmsg(m_db));
        return "";
    }

    res = sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    if (res != SQLITE_OK)
    {
        Log::error("ServerLobby::unbanPlayer", "Failed to bind arg #1.");
        return "";
    }
    
    res = sqlite3_step(stmt);
    if (res == SQLITE_DONE)
    {
        sqlite3_finalize(stmt);
        return "This player has not been banned.";
    }
    if (res == SQLITE_ROW)
    {
        std::string result = StringUtils::insertValues(
            "Online ID: %u, %s banned because: %s, days: %s",
            sqlite3_column_int(stmt, 0),
            sqlite3_column_text(stmt, 1),
            (sqlite3_column_type(stmt, 2) == SQLITE_TEXT) ?
                sqlite3_column_text(stmt, 2) :
                (const unsigned char*)"[UNSPECIFIED]",
            (sqlite3_column_type(stmt, 3) == SQLITE_INTEGER) ?
                std::to_string(sqlite3_column_int(stmt, 3)) :
                "[FOREVER]");
        sqlite3_finalize(stmt);
        return result;
    }
    else
    {
        sqlite3_finalize(stmt);
        Log::error("ServerLobby::formatBanList", "Unable to execute the statement: %d, %s",
                res, sqlite3_errmsg(m_db));
        return "";
    }
    sqlite3_finalize(stmt);
}
int ServerLobby::loadPermissionLevelForUsername(const core::stringw& name)
{
#if ENABLE_SQLITE3
    if (!m_db || !m_permissions_table_exists)
        return PERM_PLAYER;

    std::string query = StringUtils::insertValues(
            "SELECT p.level FROM %s AS p"
            " INNER JOIN %s AS s (p.online_id = s.online_id) WHERE s.username = ?;",
            ServerConfig::m_permissions_table.c_str(),
            m_server_stats_table
            );
    sqlite3_stmt* stmt = NULL;
    int res = sqlite3_prepare_v2(m_db, query.c_str(), query.size(), &stmt, NULL);
    if (res != SQLITE_OK || !stmt)
    {
        Log::error("ServerLobby::loadPermissionLevelForUsername", "Unable to prepare the statement: %d, %s",
                res, sqlite3_errmsg(m_db));
        return PERM_PLAYER;
    }

    res = sqlite3_bind_text(stmt, 1, StringUtils::wideToUtf8(name).c_str(), -1, SQLITE_TRANSIENT);
    if (res != SQLITE_OK)
    {
        Log::error("ServerLobby::loadPermissionLevelForUsername", "Failed to bind arg #1.");
        return PERM_PLAYER;
    }

    res = sqlite3_step(stmt);
    if (res == SQLITE_DONE)
    {
        // nothing found
        sqlite3_finalize(stmt);
        return PERM_PLAYER;
    }
    else if (res == SQLITE_ROW)
    {
        uint32_t result = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        return result;
    }
    else 
    {
        Log::error("ServerLobby::loadPermissionLevelForUsername", "Unable to execute the statement: %d, %s",
                res, sqlite3_errmsg(m_db));
        sqlite3_finalize(stmt);
        return PERM_PLAYER;
    }
#else
    return PERM_PLAYER;
#endif
}
const char* ServerLobby::getPermissionLevelName(int lvl) const
{
    if (lvl >= PERM_ADMINISTRATOR)
        return "administrator";
    if (lvl >= PERM_MODERATOR)
        return "moderator";
    if (lvl >= PERM_PLAYER)
        return "player";
    if (lvl >= PERM_PRISONER)
        return "prisoner";
    if (lvl >= PERM_SPECTATOR)
        return "spectator";

    return "none";
}
ServerLobby::ServerPermissionLevel 
    ServerLobby::getPermissionLevelByName(const std::string& name) const
{
    if (name == "administrator")
        return PERM_ADMINISTRATOR;
    if (name == "moderator")
        return PERM_MODERATOR;
    if (name == "player")
        return PERM_PLAYER;
    if (name == "prisoner")
        return PERM_PRISONER;
    if (name == "spectator")
        return PERM_SPECTATOR;

    return PERM_NONE;
}
const char* 
    ServerLobby::getRestrictionName(PlayerRestriction prf) const
{
    switch(prf)
    {
        case PRF_NOTEAM:
            return "noteam";
        case PRF_NOPCHAT:
            return "nopchat";
        case PRF_NOCHAT:
            return "nochat";
        case PRF_NOGAME:
            return "nogame";
        case PRF_NOSPEC:
            return "nospec";
        case PRF_HANDICAP:
            return "handicap";
        case PRF_TRACK:
            return "track";
        case PRF_KART:
            return "kart";
        case PRF_ITEMS:
            return "items";
        case PRF_OK:
            return "ok";
    }
}
PlayerRestriction ServerLobby::getRestrictionValue(
        const std::string& restriction) const
{
    if (restriction == "noteam")
        return PRF_NOTEAM;
    if (restriction == "nopchat")
        return PRF_NOPCHAT;
    if (restriction == "nochat")
        return PRF_NOCHAT;
    if (restriction == "nogame")
        return PRF_NOGAME;
    if (restriction == "nospec")
        return PRF_NOSPEC;
    if (restriction == "handicap")
        return PRF_HANDICAP;
    if (restriction == "track")
        return PRF_TRACK;
    if (restriction == "kart")
        return PRF_KART;
    if (restriction == "items")
        return PRF_ITEMS;
    if (restriction == "ok")
        return PRF_OK;
    return PRF_OK;
}
const std::string ServerLobby::formatRestrictions(PlayerRestriction prf) const
{
    std::vector<std::string> res_v;
    for (unsigned char i = 0; i < 8; ++i)
    {
        uint32_t c = prf & (1 << i);
        if (c != 0)
            res_v.push_back(getRestrictionName((PlayerRestriction)c));
    }

    std::string result;
    for (unsigned char i = 0; i < res_v.size(); ++i)
    {
        result += res_v[i];
        if (i != res_v.size() - 1)
            result += ", ";
    }
    return result;
}
