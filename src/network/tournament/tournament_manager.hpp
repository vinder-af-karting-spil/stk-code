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

#ifndef TOURNAMENT_MANAGER_HPP
#define TOURNAMENT_MANAGER_HPP

#include "modes/soccer_world.hpp"
#include "network/remote_kart_info.hpp"
#include "network/server_config.hpp"
#include "utils/log.hpp"
#include "utils/string_utils.hpp"

#include <iostream>
#include <fstream>
#include <map>
#include <set>
#include <string>

struct GameResult
{
    int m_red_goals;
    int m_blue_goals;
    std::vector<SoccerWorld::ScorerData> m_red_scorers;
    std::vector<SoccerWorld::ScorerData> m_blue_scorers;
    std::string m_played_field;
    float m_elapsed_time;

    GameResult() 
    {
        m_red_goals = 0;
        m_blue_goals = 0;
        m_played_field = "";
        m_elapsed_time = 0;
    };

    GameResult(std::vector<SoccerWorld::ScorerData> red_scorers, std::vector<SoccerWorld::ScorerData> blue_scorers)
    {
        m_red_goals = 0;
        m_blue_goals = 0;
        m_red_scorers = red_scorers;
        m_blue_scorers = blue_scorers;
        m_played_field = "";
        m_elapsed_time = 0;
    }
};

class TournamentManager
{
private:
    std::map<std::string, std::string> m_player_teams; // m_player_teams[player1] = "A"
    std::string m_red_team; // "A"
    std::string m_blue_team; // "B"
    std::set<std::string> m_red_players;
    std::set<std::string> m_blue_players;
    std::map<std::string, std::string> m_player_karts;
    
    std::string m_referee = "TheRocker";
    std::string m_video = "https://the-rocker.de/stk-supertournament/";

    std::map<int, GameResult> m_game_results;
    int m_current_game_index = -1; // 1-based count index of the games
    GameResult m_current_game_result;
    float m_target_time = 0;
    float m_elapsed_time = 0;
    float m_stopped_at = 0;

    void FilterScorerData(std::vector<SoccerWorld::ScorerData>& scorers);
    void GetAdditionalTime(int& minutes, int& seconds) const;
    void OnGameEnded();

public:
    TournamentManager();
    virtual ~TournamentManager();

    void InitializePlayersAndTeams(std::string config_string, std::string red_team, std::string blue_team);
    void UpdateTeams(std::string red_team, std::string blue_team);
    std::string GetTeam(std::string player_name);
    KartTeam GetKartTeam(std::string player_name) const;
    void SetKartTeam(std::string player_name, KartTeam team);
    std::string GetKart(std::string player_name) const;
    void SetKart(std::string player_name, std::string kart_name);
    std::set<std::string> GetKartRestrictedUsers() const;
    bool CanPlay(std::string player_name) const;
    bool CountPlayerVote(std::string player_name) const;

    void StartGame(int index, float target_time);
    void StopGame(float elapsed_time);
    void ResumeGame(float elapsed_time);
    void HandleGameResult(float elapsed_time, GameResult result);
    void ForceEndGame();
    void ResetGame(int index);
    void GetCurrentResult(int& red_goals, int& blue_goals);
    void SetCurrentResult(int red_goals, int blue_goals);
    float GetAdditionalSeconds() const;
    int GetAdditionalMinutesRounded() const;
    std::string GetAdditionalTimeMessage() const;
    void AddAdditionalSeconds(float seconds);
    void AddAdditionalSeconds(int game, float seconds);
    bool GameInitialized() const;
    bool GameOpen() const;
    bool GameDone(int index) const;
    std::string GetPlayedField() const;
    void SetPlayedField(std::string field);
    bool HasRequiredAddons(const std::set<std::string>& player_tracks) const;
    std::set<std::string> GetExcludedAddons();

    void SetReferee(std::string name);
    void SetVideo(std::string name);
};

#endif
