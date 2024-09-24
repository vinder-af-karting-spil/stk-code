#include "network/soccer_ranking.hpp"
#include "network/server_config.hpp"
#include "utils/log.hpp"
#include <fstream>
#include <sstream>

void SoccerRanking::parseLineTo(
        SoccerRanking::RankingEntry& out,
        const std::string&& line)
{
    std::stringstream ss(line);
    ss.exceptions(
            std::stringstream::failbit |
            std::stringstream::badbit |
            std::stringstream::eofbit);
    try
    {
        ss >> out.m_name;
        ss >> out.m_played_games;
        ss >> out.m_avg_team_size;
        ss >> out.m_goals_per_game;
        ss >> out.m_win_rate;
        ss >> out.m_elo;
    }
    catch (const std::ios_base::failure& e)
    {
        Log::error("SoccerRanking",
                "cannot parse line: \"%s\" (%s)",
                line.c_str(), e.what());
    }
} // parseLineTo
//--------------------------------------------------------------------
SoccerRanking::RankingEntry SoccerRanking::parseLine(
        const std::string&& line)
{
    RankingEntry re = {};
    SoccerRanking::parseLineTo(re, std::move(line));
    return re;
} // parseLine

//--------------------------------------------------------------------
void SoccerRanking::readRankings(
        std::vector<RankingEntry>& out,
        const std::size_t max,
        std::size_t offset
        )
{
    const std::string path = ServerConfig::m_soccer_ranking_file;
    if (path.empty())
        return;
    // REPLACE ME WHEN PROPER DATABASE INTERFACE IS IMPLEMENTED
    // open a file (closes it automatically)
    std::ifstream f(path, std::ios_base::in);
    f.exceptions(
            std::ifstream::failbit |
            std::ifstream::badbit);
    char linebuf[256];
    RankingEntry re = {.m_rank = 1};
    
    try 
    {
        if (offset)
            for (std::size_t i = 0; i < offset; ++i, ++re.m_rank)
                f.getline(linebuf, 256);

        if (f.eof())
            return;
        for (std::size_t i = 0; i < max; ++i, ++re.m_rank)
        {
            f.getline(linebuf, 256);

            if (f.eof())
                break;

            parseLineTo(re, linebuf);
            out.push_back(re);
        }
    }
    catch (const std::ios_base::failure& e)
    {
        Log::error("SoccerRanking", "Failed to read ranking data: %s",
                e.what());
        return;
    }
} // readRankings
//--------------------------------------------------------------------
SoccerRanking::RankingEntry SoccerRanking::getRankOf(
        const std::string &playername)
{
    const std::string path = ServerConfig::m_soccer_ranking_file;
    RankingEntry re = {.m_rank = 0};

    if (path.empty())
        return re;

    std::ifstream f(path, std::ios_base::in);
    f.exceptions(
            std::ifstream::failbit |
            std::ifstream::badbit);
    try
    {
        char linebuf[256];
        for (; !f.eof(); ++re.m_rank)
        {
            parseLineTo(re, linebuf);

            if (re.m_name == playername)
                return re;
        }
    }
    catch (const std::ios_base::failure& e)
    {
        Log::error("SoccerRanking", "Failed to read ranking data: %s",
                e.what());
    }

    return {.m_rank = 0};
} // getRankOf
