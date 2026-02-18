/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "solo3v3.h"
#include "ArenaTeamMgr.h"
#include "BattlegroundMgr.h"
#include "Config.h"
#include "Log.h"
#include "ScriptMgr.h"
#include "Chat.h"
#include "DisableMgr.h"

Solo3v3* Solo3v3::instance()
{
    static Solo3v3 instance;
    return &instance;
}

uint32 Solo3v3::GetAverageMMR(ArenaTeam* team)
{
    if (!team)
        return 0;

    // this could be improved with a better balanced calculation
    uint32 matchMakerRating = team->GetStats().Rating;

    return matchMakerRating;
}

void Solo3v3::CountAsLoss(Player* player, bool isInProgress)
{
    if (player->IsSpectator())
        return;

    ArenaTeam* plrArenaTeam = sArenaTeamMgr->GetArenaTeamById(player->GetArenaTeamId(ARENA_SLOT_SOLO_3v3));

    if (!plrArenaTeam)
        return;

    int32 ratingLoss = 0;

    // leave while arena is in progress
    if (isInProgress)
    {
        ratingLoss = sConfigMgr->GetOption<int32>("Solo.3v3.RatingPenalty.LeaveDuringMatch", 24);
    }
    // leave while arena is in preparation || don't accept queue || logout while invited
    else
    {
        ratingLoss = sConfigMgr->GetOption<int32>("Solo.3v3.RatingPenalty.LeaveBeforeMatchStart", 50);
    }

    ArenaTeamStats atStats = plrArenaTeam->GetStats();

    if (int32(atStats.Rating) - ratingLoss < 0)
        atStats.Rating = 0;
    else
        atStats.Rating -= ratingLoss;

    atStats.SeasonGames += 1;
    atStats.WeekGames += 1;
    atStats.Rank = 1;

    // Update team's rank, start with rank 1 and increase until no team with more rating was found
    ArenaTeamMgr::ArenaTeamContainer::const_iterator i = sArenaTeamMgr->GetArenaTeamMapBegin();
    for (; i != sArenaTeamMgr->GetArenaTeamMapEnd(); ++i) {
        if (i->second->GetType() == ARENA_TEAM_SOLO_3v3 && i->second->GetStats().Rating > atStats.Rating)
            ++atStats.Rank;
    }

    for (ArenaTeam::MemberList::iterator itr = plrArenaTeam->GetMembers().begin(); itr != plrArenaTeam->GetMembers().end(); ++itr) {
        if (itr->Guid == player->GetGUID()) {
            itr->WeekGames += 1;
            itr->SeasonGames += 1;
            itr->PersonalRating = atStats.Rating;

            if (int32(itr->MatchMakerRating) - ratingLoss < 0)
                itr->MatchMakerRating = 0;
            else
                itr->MatchMakerRating -= ratingLoss;

            break;
        }
    }

    plrArenaTeam->SetArenaTeamStats(atStats);
    plrArenaTeam->NotifyStatsChanged();
    plrArenaTeam->SaveToDB(true);
}

void Solo3v3::CleanUp3v3SoloQ(Battleground* bg)
{
    // Cleanup temp arena teams for solo 3v3
    if (bg->isArena() && bg->GetArenaType() == ARENA_TYPE_3v3_SOLO)
    {
        ArenaTeam* tempAlliArenaTeam = sArenaTeamMgr->GetArenaTeamById(bg->GetArenaTeamIdForTeam(TEAM_ALLIANCE));
        ArenaTeam* tempHordeArenaTeam = sArenaTeamMgr->GetArenaTeamById(bg->GetArenaTeamIdForTeam(TEAM_HORDE));

        if (tempAlliArenaTeam && tempAlliArenaTeam->GetId() >= MAX_ARENA_TEAM_ID)
        {
            sArenaTeamMgr->RemoveArenaTeam(tempAlliArenaTeam->GetId());
            delete tempAlliArenaTeam;
        }

        if (tempHordeArenaTeam && tempHordeArenaTeam->GetId() >= MAX_ARENA_TEAM_ID)
        {
            sArenaTeamMgr->RemoveArenaTeam(tempHordeArenaTeam->GetId());
            delete tempHordeArenaTeam;
        }
    }
}

void Solo3v3::CheckStartSolo3v3Arena(Battleground* bg)
{
    bool someoneNotInArena = false;
    uint32 PlayersInArena = 0;

    for (const auto& playerPair : bg->GetPlayers())
    {
        Player* player = playerPair.second;

        if (!player)
            continue;

        // prevent crash with Arena Replay module
        if (player->IsSpectator())
            return;

        PlayersInArena++;
    }

    uint32 AmountPlayersSolo3v3 = 6;
    if (PlayersInArena < AmountPlayersSolo3v3)
    {
        someoneNotInArena = true;
    }

    // if one player didn't enter arena and StopGameIncomplete is true, then end arena
    if (someoneNotInArena && sConfigMgr->GetOption<bool>("Solo.3v3.StopGameIncomplete", true))
    {
        bg->SetRated(false);
        bg->EndBattleground(TEAM_NEUTRAL);
    }
}

bool Solo3v3::CheckSolo3v3Arena(BattlegroundQueue* queue, BattlegroundBracketId bracketId)
{
    queue->m_SelectionPools[TEAM_ALLIANCE].Init();
    queue->m_SelectionPools[TEAM_HORDE].Init();

    //uint32 MinPlayersPerTeam = sBattlegroundMgr->isArenaTesting() ? 1 : 3;
    //bool filterTalents = sConfigMgr->GetOption<bool>("Solo.3v3.FilterTalents", false);

    std::vector<GroupQueueInfo*> dpsPlayers;
    std::vector<GroupQueueInfo*> healerPlayers;

    for (auto const& group : queue->m_QueuedGroups[bracketId][TEAM_ALLIANCE])
    {
        if (group->IsInvitedToBGInstanceGUID)
            continue;

        if (group->Players.size() != 1)
            continue;

        ObjectGuid const& playerGuid = *group->Players.begin();
        Player* player = ObjectAccessor::FindPlayer(playerGuid);
        if (!player)
            continue;

        Solo3v3TalentCat talentSlot = GetTalentCatForSolo3v3(player);

        switch (talentSlot)
        {
            case MELEE:
            case RANGE:
                if (dpsPlayers.size() < 4)
                    dpsPlayers.push_back(group);
                continue;
            case HEALER:
                if (healerPlayers.size() < 2)
                    healerPlayers.push_back(group);
                continue;
            default:
                continue;
        }
    }

    // TODO: Add them randomly to each side
    if (dpsPlayers.size() == 4 && healerPlayers.size() == 2)
    {
        queue->m_SelectionPools[TEAM_ALLIANCE].AddGroup(dpsPlayers[0], 3);
        queue->m_SelectionPools[TEAM_ALLIANCE].AddGroup(dpsPlayers[1], 3);
        queue->m_SelectionPools[TEAM_ALLIANCE].AddGroup(healerPlayers[0], 3);

        queue->m_SelectionPools[TEAM_HORDE].AddGroup(SwitchTeam(dpsPlayers[2]), 3);
        queue->m_SelectionPools[TEAM_HORDE].AddGroup(SwitchTeam(dpsPlayers[3]), 3);
        queue->m_SelectionPools[TEAM_HORDE].AddGroup(SwitchTeam(healerPlayers[1]), 3);

        int allianceCount = queue->m_SelectionPools[TEAM_ALLIANCE].GetPlayerCount();
        int hordeCount = queue->m_SelectionPools[TEAM_HORDE].GetPlayerCount();

        if (allianceCount + hordeCount == 6)
            return true;
    }

    return false;
}

GroupQueueInfo* Solo3v3::SwitchTeam(GroupQueueInfo* group)
{
    group->teamId = TEAM_HORDE;
    // FIX: It's BG_QUEUE_NORMAL_HORDE for unrated
    group->GroupType = BG_QUEUE_PREMADE_HORDE;

    return group;
}

void Solo3v3::CreateTempArenaTeamForQueue(BattlegroundQueue* queue, ArenaTeam* arenaTeams[])
{
    // Create temp arena team
    for (uint32 i = 0; i < BG_TEAMS_COUNT; i++)
    {
        ArenaTeam* tempArenaTeam = new ArenaTeam();  // delete it when all players have left the arena match. Stored in sArenaTeamMgr
        std::vector<Player*> playersList;
        uint32 atPlrItr = 0;

        for (auto const& itr : queue->m_SelectionPools[TEAM_ALLIANCE + i].SelectedGroups)
        {
            if (atPlrItr >= 3)
                break; // Should never happen

            for (auto const& itr2 : itr->Players)
            {
                auto _PlayerGuid = itr2;
                if (Player* _player = ObjectAccessor::FindPlayer(_PlayerGuid))
                {
                    playersList.push_back(_player);
                    atPlrItr++;
                }

                break;
            }
        }

        std::stringstream ssTeamName;
        ssTeamName << "Solo Team - " << (i + 1);

        tempArenaTeam->CreateTempArenaTeam(playersList, ARENA_TYPE_3v3_SOLO, ssTeamName.str());
        sArenaTeamMgr->AddArenaTeam(tempArenaTeam);
        arenaTeams[i] = tempArenaTeam;
    }
}

bool Solo3v3::Arena3v3CheckTalents(Player* player)
{
    if (!player)
        return false;

    if (!sConfigMgr->GetOption<bool>("Arena.3v3.BlockForbiddenTalents", false))
        return true;

    uint32 count = 0;
    for (uint32 talentId = 0; talentId < sTalentStore.GetNumRows(); ++talentId)
    {
        TalentEntry const* talentInfo = sTalentStore.LookupEntry(talentId);

        if (!talentInfo)
            continue;

        for (int8 rank = MAX_TALENT_RANK - 1; rank >= 0; --rank)
        {
            if (talentInfo->RankID[rank] == 0)
                continue;

            if (player->HasTalent(talentInfo->RankID[rank], player->GetActiveSpec()))
            {
                for (int8 i = 0; FORBIDDEN_TALENTS_IN_1V1_ARENA[i] != 0; i++)
                    if (FORBIDDEN_TALENTS_IN_1V1_ARENA[i] == talentInfo->TalentTab)
                        count += rank + 1;
            }
        }
    }

    if (count >= 36)
    {
        ChatHandler(player->GetSession()).SendSysMessage("You can't join, because you have invested to much points in a forbidden talent. Please edit your talents.");
        return false;
    }

    return true;
}

Solo3v3TalentCat Solo3v3::GetTalentCatForSolo3v3(Player* player)
{
    uint32 count[MAX_TALENT_CAT];

    for (int i = 0; i < MAX_TALENT_CAT; i++)
        count[i] = 0;

    for (uint32 talentId = 0; talentId < sTalentStore.GetNumRows(); ++talentId)
    {
        TalentEntry const* talentInfo = sTalentStore.LookupEntry(talentId);

        if (!talentInfo)
            continue;

        for (int8 rank = MAX_TALENT_RANK - 1; rank >= 0; --rank)
        {
            if (talentInfo->RankID[rank] == 0)
                continue;

            if (player->HasTalent(talentInfo->RankID[rank], player->GetActiveSpec()))
            {
                for (int8 i = 0; SOLO_3V3_TALENTS_MELEE[i] != 0; i++)
                    if (SOLO_3V3_TALENTS_MELEE[i] == talentInfo->TalentTab)
                        count[MELEE] += rank + 1;

                for (int8 i = 0; SOLO_3V3_TALENTS_RANGE[i] != 0; i++)
                    if (SOLO_3V3_TALENTS_RANGE[i] == talentInfo->TalentTab)
                        count[RANGE] += rank + 1;

                for (int8 i = 0; SOLO_3V3_TALENTS_HEAL[i] != 0; i++)
                    if (SOLO_3V3_TALENTS_HEAL[i] == talentInfo->TalentTab)
                        count[HEALER] += rank + 1;
            }
        }
    }

    uint32 prevCount = 0;

    Solo3v3TalentCat talCat = MELEE; // Default MELEE (if no talent points set)

    for (int i = 0; i < MAX_TALENT_CAT; i++)
    {
        if (count[i] > prevCount)
        {
            talCat = (Solo3v3TalentCat)i;
            prevCount = count[i];
        }
    }

    return talCat;
}

//Solo3v3TalentCat Solo3v3::GetFirstAvailableSlot(bool soloTeam[][MAX_TALENT_CAT]) {
//    if (!soloTeam[0][MELEE] || !soloTeam[1][MELEE])
//        return MELEE;
//
//    if (!soloTeam[0][RANGE] || !soloTeam[1][RANGE])
//        return RANGE;
//
//    if (!soloTeam[0][HEALER] || !soloTeam[1][HEALER])
//        return HEALER;
//
//    return MELEE;
//}
