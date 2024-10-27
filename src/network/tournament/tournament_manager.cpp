//
//  SuperTuxKart - a fun racing game with go-kart
//  Copyright (C) 2021 SuperTuxKart-Team
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

#include "network/tournament/tournament_manager.hpp"

void TournamentManager::FilterScorerData(std::vector<SoccerWorld::ScorerData>& scorers)
{
    for (int i = scorers.size() - 1; i >= 0; i--)
    {
        std::string player_name = StringUtils::wideToUtf8(scorers[i].m_player);
        if (StringUtils::hasSuffix(player_name, " (not counted)"))
            scorers.erase(scorers.begin() + i);
    }

    for (auto& scorer : scorers)
        scorer.m_time += m_elapsed_time;
}

void TournamentManager::GetAdditionalTime(int& minutes, int& seconds) const
{
    int total_seconds = (int)GetAdditionalSeconds();
    minutes = total_seconds / 60;
    seconds = total_seconds - 60 * minutes;
}

void TournamentManager::OnGameEnded()
{
    if (GameInitialized())
    {
        std::string log = "Match: " + m_red_team + " vs " + m_blue_team + "\n";
        log += "Game: " + std::to_string(m_current_game_index) + "\n";
        log += "Soccer Field: " + GetPlayedField() + "\n";
        log += "Result: " + std::to_string(m_current_game_result.m_red_goals) + ":" + std::to_string(m_current_game_result.m_blue_goals) + "\n";
        for (auto& scorer : m_current_game_result.m_red_scorers)
            log += "Goal " + m_red_team + " " + StringUtils::wideToUtf8(scorer.m_player) + " " + std::to_string(scorer.m_time) + "\n";
        for (auto& scorer : m_current_game_result.m_blue_scorers)
            log += "Goal " + m_blue_team + " " + StringUtils::wideToUtf8(scorer.m_player) + " " + std::to_string(scorer.m_time) + "\n";

        std::ofstream logfile;
        logfile.open(ServerConfig::m_tourn_log, std::ios_base::app);
        if (logfile.is_open())
        {
            logfile << log;
            logfile.close();
        }
        Log::info("TournamentManager", log.c_str());

        std::string cmd = "python3 update_matchplan.py " + m_red_team + " " + m_blue_team + " " + std::to_string(m_current_game_index) + " " + std::to_string(m_current_game_result.m_red_goals) + "-" + std::to_string(m_current_game_result.m_blue_goals) + " " + m_current_game_result.m_played_field + " " + m_referee + " " + m_video;
        system(cmd.c_str());

        m_current_game_result.m_elapsed_time = m_elapsed_time;
        m_game_results[m_current_game_index] = m_current_game_result;
        m_current_game_index = -1;
    }
}

TournamentManager::TournamentManager()
{
}

TournamentManager::~TournamentManager()
{
}

// Format of config_string = "player1 A player2 A player3 B player4 B player5 Sub"
void TournamentManager::InitializePlayersAndTeams(std::string config_string, std::string red_team, std::string blue_team)
{
    std::vector<std::string> player_teams = StringUtils::split(config_string, ' ');
    if (player_teams.size() % 2 != 0)
    {
        Log::error("TournamentManager", "Invalid config string");
        return;
    }
        
    for (size_t i = 0; i < player_teams.size(); i += 2)
        m_player_teams[player_teams[i]] = player_teams[i + 1];

    UpdateTeams(red_team, blue_team);
}

void TournamentManager::UpdateTeams(std::string red_team, std::string blue_team)
{
    m_red_team = red_team;
    m_blue_team = blue_team;

    m_red_players.clear();
    m_blue_players.clear();
    for (auto& pt : m_player_teams)
    {
        if (pt.second == red_team)
            m_red_players.insert(pt.first);
        if (pt.second == blue_team)
            m_blue_players.insert(pt.first);
    }
}

std::string TournamentManager::GetTeam(std::string player_name)
{
    return (m_player_teams.find(player_name) == m_player_teams.end()) ? "" 
        : m_player_teams[player_name];
}

KartTeam TournamentManager::GetKartTeam(std::string player_name) const
{
    bool blue = m_blue_players.find(player_name) != m_blue_players.end();
    bool red = m_red_players.find(player_name) != m_red_players.end();

    return red ? KART_TEAM_RED : (blue ? KART_TEAM_BLUE : KART_TEAM_NONE);
}

void TournamentManager::SetKartTeam(std::string player_name, KartTeam team)
{
    switch (team)
    {
    case KART_TEAM_NONE:
        m_red_players.erase(player_name);
        m_blue_players.erase(player_name);
        return;
    case KART_TEAM_RED:
        m_red_players.insert(player_name);
        m_blue_players.erase(player_name);
        return;
    case KART_TEAM_BLUE:
        m_blue_players.insert(player_name);
        m_red_players.erase(player_name);
        return;
    }
}

std::string TournamentManager::GetKart(std::string player_name) const
{
    return m_player_karts.count(player_name) ? m_player_karts.at(player_name) : "";
}

void TournamentManager::SetKart(std::string player_name, std::string kart_name)
{
    if (kart_name == "")
        m_player_karts.erase(player_name);
    else
        m_player_karts[player_name] = kart_name;
}

std::set<std::string> TournamentManager::GetKartRestrictedUsers() const
{
    std::set<std::string> player_names;
    for (auto& keyValue : m_player_karts)
        player_names.insert(keyValue.first);
    return player_names;
}

bool TournamentManager::CanPlay(std::string player_name) const
{
    return GameInitialized() && (GetKartTeam(player_name) != KART_TEAM_NONE);
}

bool TournamentManager::CountPlayerVote(std::string player_name) const
{
    if (m_current_game_index == 2)
        return GetKartTeam(player_name) == KART_TEAM_RED;
    else if (m_current_game_index == 3)
        return GetKartTeam(player_name) == KART_TEAM_BLUE;
    return true;
}

void TournamentManager::StartGame(int index, float target_time)
{
    m_current_game_index = index;
    m_current_game_result = GameResult();
    m_target_time = target_time;
    m_elapsed_time = 0;
    m_stopped_at = 0;
    m_player_karts.clear();
}

void TournamentManager::StopGame(float elapsed_time)
{
    m_stopped_at += elapsed_time;
}

void TournamentManager::ResumeGame(float elapsed_time)
{
    if (m_stopped_at != 0)
    {
        m_target_time += elapsed_time - m_stopped_at;
        m_stopped_at = 0;
    }
}

void TournamentManager::HandleGameResult(float elapsed_time, GameResult result)
{
    FilterScorerData(result.m_red_scorers);
    FilterScorerData(result.m_blue_scorers);

    m_current_game_result.m_red_goals += result.m_red_scorers.size();
    m_current_game_result.m_blue_goals += result.m_blue_scorers.size();

    m_current_game_result.m_red_scorers.insert(m_current_game_result.m_red_scorers.end(), result.m_red_scorers.begin(), result.m_red_scorers.end());
    m_current_game_result.m_blue_scorers.insert(m_current_game_result.m_blue_scorers.end(), result.m_blue_scorers.begin(), result.m_blue_scorers.end());

    if (m_stopped_at != 0)
    {
        m_elapsed_time += m_stopped_at;
        m_stopped_at = 0;
    }
    else
    {
        m_elapsed_time += elapsed_time;
    }

    if (!GameOpen())
        OnGameEnded();
}

void TournamentManager::ForceEndGame()
{
    m_target_time = m_elapsed_time;
    OnGameEnded();
}

void TournamentManager::ResetGame(int index)
{
    m_current_game_index = -1;
    m_current_game_result = GameResult();
    m_target_time = 0;
    m_stopped_at = 0;
    m_elapsed_time = 0;
    m_game_results.erase(index);

    std::ofstream logfile;
    logfile.open(ServerConfig::m_tourn_log, std::ios_base::app);
    if (logfile.is_open())
    {
        logfile << "RESET Game " + std::to_string(index) + "\n";
        logfile.close();
    }
}

void TournamentManager::GetCurrentResult(int& red_goals, int& blue_goals)
{
    red_goals = m_current_game_result.m_red_goals;
    blue_goals = m_current_game_result.m_blue_goals;
}

void TournamentManager::SetCurrentResult(int red_goals, int blue_goals)
{
    m_current_game_result.m_red_goals = red_goals;
    m_current_game_result.m_blue_goals = blue_goals;
}

float TournamentManager::GetAdditionalSeconds() const
{
    return m_target_time - m_elapsed_time;
}

int TournamentManager::GetAdditionalMinutesRounded() const
{
    return std::max(0, ((int)GetAdditionalSeconds() + 30) / 60);
}

std::string TournamentManager::GetAdditionalTimeMessage() const
{
    int additional_minutes = GetAdditionalMinutesRounded();
    int minutes = 0, seconds = 0;
    GetAdditionalTime(minutes, seconds);
    std::string min_str = additional_minutes == 1 ? " minute" : " minutes";
    char sec_string[3];
    sprintf(sec_string, "%02d", seconds);
    return std::to_string(additional_minutes) + min_str + " (" + std::to_string(minutes) + ":" + sec_string + ") to replay.";
}

void TournamentManager::AddAdditionalSeconds(float seconds)
{
    m_target_time += seconds;
}

void TournamentManager::AddAdditionalSeconds(int game, float seconds)
{
    if (GameDone(game))
    {
        m_current_game_index = game;
        m_current_game_result = m_game_results[game];
        m_target_time = m_current_game_result.m_elapsed_time + seconds;
        m_elapsed_time = m_current_game_result.m_elapsed_time;
        m_stopped_at = 0;
    }
}

bool TournamentManager::GameInitialized() const
{
    return m_current_game_index > 0;
}

bool TournamentManager::GameOpen() const
{
    return GetAdditionalMinutesRounded() > 0;
}

bool TournamentManager::GameDone(int index) const
{
    return m_game_results.find(index) != m_game_results.end();
}

std::string TournamentManager::GetPlayedField() const
{
    return m_current_game_result.m_played_field;
}

void TournamentManager::SetPlayedField(std::string field)
{
    m_current_game_result.m_played_field = field;
}

bool TournamentManager::HasRequiredAddons(const std::set<std::string>& player_tracks) const
{
    std::vector<std::string> required_fields{ "icy_soccer_field", "soccer_field", "lasdunassoccer", "addon_wood-warbler-soccer" };
    std::vector<std::string> semi_required_fields{ "addon_xr-4r3n4_1", "addon_ice-rink_1", "addon_hole-drop", "addon_skyline--soccer-","addon_babyfball"};

    for (const std::string& track : required_fields)
    {
        if (player_tracks.find(track) == player_tracks.end())
            return false;
    }

    int semi_required_count = 0;
    for (const std::string& track : semi_required_fields)
    {
        if (player_tracks.find(track) != player_tracks.end())
            semi_required_count++;
    }

    return semi_required_count >= 4;
}

std::set<std::string> TournamentManager::GetExcludedAddons()
{
    std::set<std::string> excluded_addons;

    if (m_current_game_index == 2)
    {
        excluded_addons.insert("icy_soccer_field");
    }
    else if (m_current_game_index == 3)
    {
        excluded_addons.insert("icy_soccer_field");

        if (m_game_results.find(2) != m_game_results.end())
        {
            std::string field_game_2 = m_game_results[2].m_played_field;
            if (field_game_2 != "addon_wood-warbler-soccer")
                excluded_addons.insert(field_game_2);
        }
    }

    return excluded_addons;
}

void TournamentManager::SetReferee(std::string name)
{
    m_referee = name;
}

void TournamentManager::SetVideo(std::string link)
{
    m_video = link;
}


