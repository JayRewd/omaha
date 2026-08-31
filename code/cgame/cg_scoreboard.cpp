/*
===========================================================================
Copyright (C) 2023 the OpenMoHAA team

This file is part of OpenMoHAA source code.

OpenMoHAA source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

OpenMoHAA source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenMoHAA source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

// DESCRIPTION:
// Scoreboard

#include "cg_local.h"

static qboolean CG_Scoreboard_UseModern(void)
{
    return CG_UseModernHudPack();
}

/*
 * Added in OPM: publish Allied/Axis team totals for the modern HUD score strip.
 * scoreField is the mode's real score (round wins or team kills); atoi skips a
 * trailing " Wins" suffix used on round-mode headers.
 */
static void CG_Hud_SetTeamScoreCvar(int team, const char *scoreField)
{
    char buf[32];

    if (!scoreField) {
        scoreField = "0";
    }
    Com_sprintf(buf, sizeof(buf), "%d", atoi(scoreField));
    if (team == TEAM_ALLIES) {
        cgi.Cvar_Set("ui_om_hud_allied_score", buf);
    } else if (team == TEAM_AXIS) {
        cgi.Cvar_Set("ui_om_hud_axis_score", buf);
    }
}

/*
 * Added in OPM: request scores without opening the scoreboard. Reuses the retail
 * 2s throttle on cg.scoresRequestTime so TAB and the HUD strip share one budget.
 */
void CG_RequestHudTeamScoresSilent(void)
{
    if (cgs.gametype <= GT_FFA) {
        return;
    }
    if (cg.scoresRequestTime && cg.scoresRequestTime + 2000 >= cg.time) {
        return;
    }
    cg.scoresRequestTime = cg.time;
    cgi.SendClientCommand("score");
}

static void CG_Scoreboard_FormatKd(const char *killsStr, const char *deathsStr, char *out, int outSize)
{
    int kills;
    int deaths;

    if (!out || outSize <= 0) {
        return;
    }
    kills  = killsStr ? atoi(killsStr) : 0;
    deaths = deathsStr ? atoi(deathsStr) : 0;
    if (deaths < 1) {
        deaths = 1;
    }
    Com_sprintf(out, outSize, "%.2f", (float)kills / (float)deaths);
}

static void CG_Scoreboard_ColorToHex(const float *rgba, char *out, int outSize)
{
    int r, g, b, a;

    if (!out || outSize < 10 || !rgba) {
        if (out && outSize > 0) {
            out[0] = 0;
        }
        return;
    }
    r = (int)(rgba[0] * 255.0f + 0.5f);
    g = (int)(rgba[1] * 255.0f + 0.5f);
    b = (int)(rgba[2] * 255.0f + 0.5f);
    a = (int)(rgba[3] * 255.0f + 0.5f);
    Com_sprintf(out, outSize, "#%02X%02X%02X%02X", r, g, b, a);
}

/*
 * Added in OPM: modern scoreboard row fill from parse backColor only.
 * Team chrome (Allies/Axis banners) is colored in XML, not per-player fills.
 */
static void CG_Scoreboard_ModernRowFill(char *out, int outSize, int clientTeam, const float *backColor)
{
    (void)clientTeam;
    CG_Scoreboard_ColorToHex(backColor, out, outSize);
}


static void CG_Scoreboard_BeginModernParse(void)
{
    uir_scoreboard_meta_t meta;
    int                   i;

    if (!CG_Scoreboard_UseModern() || !cgi.UIR_Scoreboard_Clear) {
        return;
    }

    memset(&meta, 0, sizeof(meta));
    meta.gametype  = cgs.gametype;
    meta.teamMode  = cgs.gametype > GT_FFA ? qtrue : qfalse;
    meta.roundMode = cgs.gametype >= GT_TEAM_ROUNDS ? qtrue : qfalse;
    Q_strncpyz(meta.deathsColLabel, cgs.gametype > GT_TEAM ? "Total" : "Deaths", sizeof(meta.deathsColLabel));

    if (cgs.gametype == GT_TOW) {
        for (i = 0; i < 5; i++) {
            cvar_t *cv;
            cv = cgi.Cvar_Find(va("tow_allied_obj%i", i + 1));
            meta.towAlliedObj[i] = cv ? cv->integer : 0;
            cv = cgi.Cvar_Find(va("tow_axis_obj%i", i + 1));
            meta.towAxisObj[i] = cv ? cv->integer : 0;
        }
    }
    if (cgs.gametype == GT_LIBERATION) {
        cvar_t *cv;
        cv = cgi.Cvar_Find("scoreboard_toggle1");
        meta.libToggle1 = cv ? cv->integer : 0;
        cv = cgi.Cvar_Find("scoreboard_toggle2");
        meta.libToggle2 = cv ? cv->integer : 0;
    }

    cgi.UIR_Scoreboard_Clear();
    cgi.UIR_Scoreboard_SetMeta(&meta);
}

static void CG_Scoreboard_EndModernParse(int entryCount)
{
    (void)entryCount;
    if (!CG_Scoreboard_UseModern() || !cgi.UIR_Scoreboard_NotifyChanged) {
        return;
    }
    cgi.UIR_Scoreboard_NotifyChanged();
}

static void CG_Scoreboard_AddModernRow(
    int         clientNum,
    int         clientTeam,
    qboolean    isHeader,
    qboolean    isSpectator,
    qboolean    isDead,
    const char *slot,
    const char *name,
    const char *kills,
    const char *deaths,
    const char *time,
    const char *ping,
    const float *textColor,
    const float *backColor
)
{
    uir_scoreboard_row_t row;

    if (!CG_Scoreboard_UseModern() || !cgi.UIR_Scoreboard_AddRow) {
        return;
    }

    if (clientNum == -2) {
        return;
    }
    if (isSpectator && isHeader) {
        return;
    }

    memset(&row, 0, sizeof(row));
    row.clientNum = clientNum;
    row.isHeader  = isHeader;
    row.isSpectator = isSpectator && !isHeader;
    /* Added in Omaha: expose dead flag for modern XML (not retail textColor). */
    row.isDead = isDead && !isHeader && !row.isSpectator;

    if (isHeader || clientNum < 0) {
        row.kind = UIR_SCORE_ROW_HEADER;
    } else {
        row.kind = UIR_SCORE_ROW_PLAYER;
    }

    if (slot) {
        Q_strncpyz(row.slot, slot, sizeof(row.slot));
    }
    if (name) {
        Q_strncpyz(row.name, name, sizeof(row.name));
    } else if (isHeader && slot && slot[0]) {
        Q_strncpyz(row.name, slot, sizeof(row.name));
    }
    if (kills) {
        Q_strncpyz(row.kills, kills, sizeof(row.kills));
    }
    if (deaths) {
        Q_strncpyz(row.deaths, deaths, sizeof(row.deaths));
    }
    if (time) {
        Q_strncpyz(row.time, time, sizeof(row.time));
    }
    if (ping) {
        Q_strncpyz(row.ping, ping, sizeof(row.ping));
    }

    if (row.kind == UIR_SCORE_ROW_PLAYER) {
        CG_Scoreboard_FormatKd(row.kills, row.deaths, row.kd, sizeof(row.kd));
    }

    if (clientTeam == TEAM_ALLIES) {
        Q_strncpyz(row.team, "allies", sizeof(row.team));
    } else if (clientTeam == TEAM_AXIS) {
        Q_strncpyz(row.team, "axis", sizeof(row.team));
    } else if (row.isSpectator || clientTeam == TEAM_SPECTATOR) {
        Q_strncpyz(row.team, "spectator", sizeof(row.team));
    } else {
        Q_strncpyz(row.team, "none", sizeof(row.team));
    }

    CG_Scoreboard_ColorToHex(textColor, row.textColor, sizeof(row.textColor));
    CG_Scoreboard_ModernRowFill(row.rowFill, sizeof(row.rowFill), clientTeam, backColor);

    cgi.UIR_Scoreboard_AddRow(&row);
}

void CG_GetScoreBoardColor(float *fR, float *fG, float *fB, float *fA)
{
    *fR = 0.0f;
    *fG = 0.0f;
    *fB = 0.0f;
    *fA = 0.7f;
}

void CG_GetScoreBoardFontColor(float *fR, float *fG, float *fB, float *fA)
{
    *fR = 1.0f;
    *fG = 1.0f;
    *fB = 1.0f;
    *fA = 1.0f;
}

void CG_GetScoreBoardPosition(float *fX, float *fY, float *fW, float *fH)
{
    *fX = 32.0;
    *fY = 56.0;
    *fW = 384.0;
    *fH = 392.0;
}

int CG_GetScoreBoardDrawHeader()
{
    return 0;
}

void CG_PrepScoreBoardInfo()
{
    switch (cgs.gametype) {
    case GT_TEAM_ROUNDS:
        Q_strncpyz(cg.scoresMenuName, "DM_Round_Scoreboard", sizeof(cg.scoresMenuName));
        break;
    case GT_OBJECTIVE:
        Q_strncpyz(cg.scoresMenuName, "Obj_Scoreboard", sizeof(cg.scoresMenuName));
        break;
    case GT_TOW:
        Q_strncpyz(cg.scoresMenuName, "Tow_Scoreboard", sizeof(cg.scoresMenuName));
        break;
    case GT_LIBERATION:
        Q_strncpyz(cg.scoresMenuName, "Lib_Scoreboard", sizeof(cg.scoresMenuName));
        break;
    default:
        Q_strncpyz(cg.scoresMenuName, "DM_Scoreboard", sizeof(cg.scoresMenuName));
        break;
    }
}

const char *CG_GetColumnName_ver_15(int iColumnNum, int *iColumnWidth)
{
    int         iReturnWidth;
    const char *pszReturnString;

    switch (iColumnNum) {
    case 0:
        iReturnWidth    = 24;
        pszReturnString = "#";
        break;
    case 1:
        iReturnWidth    = 128;
        pszReturnString = "Name";
        break;
    case 2:
        iReturnWidth    = 64;
        pszReturnString = "Kills";
        break;
    case 3:
        iReturnWidth    = 64;
        pszReturnString = "Deaths";
        if (cgs.gametype > GT_TEAM) {
            pszReturnString = "Total";
        }
        break;
    case 4:
        iReturnWidth    = 64;
        pszReturnString = "Time";
        break;
    case 5:
        iReturnWidth    = 64;
        pszReturnString = "Ping";
        break;
    default:
        iReturnWidth    = 0;
        pszReturnString = 0;
        break;
    }

    if (iColumnWidth) {
        *iColumnWidth = iReturnWidth;
    }

    return pszReturnString;
}

const char *CG_GetColumnName_ver_6(int iColumnNum, int *iColumnWidth)
{
    int         iReturnWidth;
    const char *pszReturnString;

    switch (iColumnNum) {
    case 0:
        iReturnWidth    = 128;
        pszReturnString = "Name";
        break;
    case 1:
        iReturnWidth    = 64;
        pszReturnString = "Kills";
        break;
    case 2:
        iReturnWidth    = 64;
        pszReturnString = "Deaths";
        if (cgs.gametype > GT_TEAM) {
            pszReturnString = "Total";
        }
        break;
    case 3:
        iReturnWidth    = 64;
        pszReturnString = "Time";
        break;
    case 4:
        iReturnWidth    = 64;
        pszReturnString = "Ping";
        break;
    default:
        iReturnWidth    = 0;
        pszReturnString = 0;
        break;
    }

    if (iColumnWidth) {
        *iColumnWidth = iReturnWidth;
    }

    return pszReturnString;
}

void CG_ParseScores_ver_15()
{
    int      i;
    int      iEntryCount;
    int      iClientTeam, iClientNum;
    int      iDatumCount;
    int      iMatchTeam;
    int      iCurrentEntry;
    qboolean bIsDead, bIsHeader;
    char     szString2[MAX_STRING_TOKENS];
    char     szString3[MAX_STRING_TOKENS];
    char     szString4[MAX_STRING_TOKENS];
    char     szString5[MAX_STRING_TOKENS];
    char     szString6[MAX_STRING_TOKENS];
    char     szString7[MAX_STRING_TOKENS];
    float    vSameTeamTextColor[4];
    float    vSameTeamBackColor[4];
    float    vOtherTeamTextColor[4];
    float    vOtherTeamBackColor[4];
    float    vNoTeamTextColor[4];
    float    vNoTeamBackColor[4];
    float    vThisClientTextColor[4];
    float    vThisClientBackColor[4];
    float    vDeadTextColorDead[4];
    float   *pItemTextColor;
    float   *pItemBackColor;

    iMatchTeam             = -1;
    vSameTeamTextColor[0]  = 1.0f;
    vSameTeamTextColor[1]  = 1.0f;
    vSameTeamTextColor[2]  = 1.0f;
    vSameTeamTextColor[3]  = 1.0f;
    vSameTeamBackColor[0]  = 0.1f;
    vSameTeamBackColor[1]  = 0.5f;
    vSameTeamBackColor[2]  = 0.1f;
    vSameTeamBackColor[3]  = 0.4f;
    vOtherTeamTextColor[0] = 1.0f;
    vOtherTeamTextColor[1] = 1.0f;
    vOtherTeamTextColor[2] = 1.0f;
    vOtherTeamTextColor[3] = 1.0f;
    vOtherTeamBackColor[0] = 0.5f;
    vOtherTeamBackColor[1] = 0.1f;
    vOtherTeamBackColor[2] = 0.1f;
    vOtherTeamBackColor[3] = 0.4f;
    vNoTeamTextColor[0]    = 1.0f;
    vNoTeamTextColor[1]    = 1.0f;
    vNoTeamTextColor[2]    = 1.0f;
    vNoTeamTextColor[3]    = 1.0f;
    vNoTeamBackColor[0]    = 0.1f;
    vNoTeamBackColor[1]    = 0.1f;
    vNoTeamBackColor[2]    = 0.1f;
    vNoTeamBackColor[3]    = 0.4f;
    vDeadTextColorDead[0]  = 1.0f;
    vDeadTextColorDead[1]  = 0.1f;
    vDeadTextColorDead[2]  = 0.1f;
    vDeadTextColorDead[3]  = 1.0f;

    vThisClientTextColor[0] = 0.0f;
    vThisClientTextColor[1] = 0.0f;
    vThisClientTextColor[2] = 0.0f;
    vThisClientTextColor[3] = 1.0f;

    if (cgs.gametype > GT_FFA) {
        vThisClientBackColor[0] = 0.5f;
        vThisClientBackColor[1] = 0.75f;
        vThisClientBackColor[2] = 0.5f;
    } else {
        vThisClientBackColor[0] = 0.75f;
        vThisClientBackColor[1] = 0.75f;
        vThisClientBackColor[2] = 0.75f;
    }

    vThisClientBackColor[3] = 0.8f;

    iCurrentEntry = 1;
    if (cgs.gametype > GT_FFA) {
        iDatumCount = 6;
        iMatchTeam  = cg.snap->ps.stats[STAT_TEAM];
        if (iMatchTeam != TEAM_ALLIES && iMatchTeam != TEAM_AXIS) {
            iMatchTeam              = TEAM_ALLIES;
            vThisClientTextColor[0] = 0.0f;
            vThisClientTextColor[1] = 0.0f;
            vThisClientTextColor[2] = 0.0f;
            vThisClientBackColor[0] = 0.75f;
            vThisClientBackColor[1] = 0.75f;
            vThisClientBackColor[2] = 0.75f;
        }
    } else {
        // free-for-all
        iDatumCount = 5;
    }

    iEntryCount = atoi(cgi.Argv(iCurrentEntry++));
    if (iEntryCount > MAX_CLIENTS) {
        iEntryCount = MAX_CLIENTS;
    }

    if (cgs.gametype == GT_TOW) {
        cgi.Cvar_Set("tow_allied_obj1", va("%i", (int)atof(cgi.Argv(iCurrentEntry++))));
        cgi.Cvar_Set("tow_allied_obj2", va("%i", (int)atof(cgi.Argv(iCurrentEntry++))));
        cgi.Cvar_Set("tow_allied_obj3", va("%i", (int)atof(cgi.Argv(iCurrentEntry++))));
        cgi.Cvar_Set("tow_allied_obj4", va("%i", (int)atof(cgi.Argv(iCurrentEntry++))));
        cgi.Cvar_Set("tow_allied_obj5", va("%i", (int)atof(cgi.Argv(iCurrentEntry++))));
        cgi.Cvar_Set("tow_axis_obj1", va("%i", (int)atof(cgi.Argv(iCurrentEntry++))));
        cgi.Cvar_Set("tow_axis_obj2", va("%i", (int)atof(cgi.Argv(iCurrentEntry++))));
        cgi.Cvar_Set("tow_axis_obj3", va("%i", (int)atof(cgi.Argv(iCurrentEntry++))));
        cgi.Cvar_Set("tow_axis_obj4", va("%i", (int)atof(cgi.Argv(iCurrentEntry++))));
        cgi.Cvar_Set("tow_axis_obj5", va("%i", (int)atof(cgi.Argv(iCurrentEntry++))));
    }

    if (cgs.gametype == GT_LIBERATION) {
        cgi.Cvar_Set("scoreboard_toggle1", va("%i", (int)atof(cgi.Argv(iCurrentEntry++))));
        cgi.Cvar_Set("scoreboard_toggle2", va("%i", (int)atof(cgi.Argv(iCurrentEntry++))));
    }

    CG_Scoreboard_BeginModernParse();

    {
        qboolean inSpectatorSection = qfalse;

        for (i = 0; i < iEntryCount; ++i) {
            qboolean isSpectator = qfalse;
            bIsHeader = qfalse;
            bIsDead   = qfalse;
        if (cgs.gametype > GT_FFA) {
            iClientNum  = atoi(cgi.Argv(iCurrentEntry + iDatumCount * i));
            iClientTeam = atoi(cgi.Argv(1 + iCurrentEntry + iDatumCount * i));
            if (iClientTeam >= 0) {
                bIsDead = qfalse;
            } else {
                bIsDead     = qtrue;
                iClientTeam = -iClientTeam;
            }

            if (iClientNum == -1) {
                szString2[0] = 0;
                bIsHeader    = qtrue;

                switch (iClientTeam) {
                case 1:
                    Q_strncpyz(szString3, cgi.LV_ConvertString("Spectators"), sizeof(szString3));
                    break;
                case 2:
                    Q_strncpyz(szString3, cgi.LV_ConvertString("Free-For-Allers"), sizeof(szString3));
                    break;
                case 3:
                    Com_sprintf(
                        szString3,
                        sizeof(szString3),
                        "%s - %d %s",
                        cgi.LV_ConvertString("Allies"),
                        atoi(cgi.Argv(2 + iCurrentEntry + iDatumCount * i)),
                        cgi.LV_ConvertString("Players")
                    );
                    iCurrentEntry++;
                    break;
                case 4:
                    Com_sprintf(
                        szString3,
                        sizeof(szString3),
                        "%s - %d %s",
                        cgi.LV_ConvertString("Axis"),
                        atoi(cgi.Argv(2 + iCurrentEntry + iDatumCount * i)),
                        cgi.LV_ConvertString("Players")
                    );
                    iCurrentEntry++;
                    break;
                default:
                    Q_strncpyz(szString3, cgi.LV_ConvertString("No Team"), sizeof(szString3));
                    break;
                }
            } else if (iClientNum == -2) {
                // spectating
                szString2[0] = 0;
                szString3[0] = 0;
            } else {
                Q_strncpyz(szString2, va("%i", iClientNum), sizeof(szString2));
                Q_strncpyz(szString3, cg.clientinfo[iClientNum].name, sizeof(szString3));
            }

            Q_strncpyz(szString4, cgi.Argv(2 + iCurrentEntry + iDatumCount * i), sizeof(szString4));
            Q_strncpyz(szString5, cgi.Argv(3 + iCurrentEntry + iDatumCount * i), sizeof(szString5));
            Q_strncpyz(szString6, cgi.Argv(4 + iCurrentEntry + iDatumCount * i), sizeof(szString6));
            Q_strncpyz(szString7, cgi.Argv(5 + iCurrentEntry + iDatumCount * i), sizeof(szString7));

            /* Added in OPM: cache real team score (m_teamwins) before " Wins" suffix. */
            if (bIsHeader && (iClientTeam == TEAM_ALLIES || iClientTeam == TEAM_AXIS)) {
                CG_Hud_SetTeamScoreCvar(iClientTeam, szString4);
            }

            if (cgs.gametype >= GT_TEAM_ROUNDS && iClientNum == -1
                && (iClientTeam == TEAM_ALLIES || iClientTeam == TEAM_AXIS)) {
                strcat(szString4, va(" %s", cgi.LV_ConvertString("Wins")));
                szString5[0] = 0;
            }

            if (iClientNum == cg.snap->ps.clientNum) {
                pItemTextColor = vThisClientTextColor;
                pItemBackColor = vThisClientBackColor;
            } else if (iClientNum == -2) {
                pItemTextColor = vNoTeamTextColor;
                pItemBackColor = vNoTeamBackColor;
            } else if (iClientTeam == TEAM_ALLIES || iClientTeam == TEAM_AXIS) {
                if (iClientTeam == iMatchTeam) {
                    pItemTextColor = vSameTeamTextColor;
                    pItemBackColor = vSameTeamBackColor;
                } else {
                    pItemTextColor = vOtherTeamTextColor;
                    pItemBackColor = vOtherTeamBackColor;
                }
            } else {
                pItemTextColor = vNoTeamTextColor;
                pItemBackColor = vNoTeamBackColor;
            }

            if (bIsDead) {
                pItemTextColor = vDeadTextColorDead;
            }

            if (iClientNum == -1 && iClientTeam == TEAM_SPECTATOR) {
                inSpectatorSection = qtrue;
            }
            isSpectator = inSpectatorSection && iClientNum >= 0;
        } else {
            iClientTeam = TEAM_FREEFORALL;
            iClientNum = atoi(cgi.Argv(iCurrentEntry + iDatumCount * i));
            if (iClientNum >= 0) {
                Q_strncpyz(szString2, va("%i", iClientNum), sizeof(szString2));
                Q_strncpyz(szString3, cg.clientinfo[iClientNum].name, sizeof(szString3));
                Q_strncpyz(szString4, cgi.Argv(1 + iCurrentEntry + iDatumCount * i), sizeof(szString4));
                Q_strncpyz(szString5, cgi.Argv(2 + iCurrentEntry + iDatumCount * i), sizeof(szString5));
                Q_strncpyz(szString6, cgi.Argv(3 + iCurrentEntry + iDatumCount * i), sizeof(szString6));
                Q_strncpyz(szString7, cgi.Argv(4 + iCurrentEntry + iDatumCount * i), sizeof(szString7));
            } else {
                szString2[0] = 0;
                if (iClientNum == -3) {
                    Q_strncpyz(szString2, cgi.LV_ConvertString("Players"), sizeof(szString2));
                    bIsHeader = qtrue;
                    inSpectatorSection = qfalse;
                } else if (iClientNum == -2) {
                    Q_strncpyz(szString2, cgi.LV_ConvertString("Spectators"), sizeof(szString2));
                    bIsHeader = qtrue;
                    inSpectatorSection = qtrue;
                } else {
                    // unknown
                    szString3[0] = 0;
                }
                szString4[0] = 0;
                szString5[0] = 0;
                szString6[0] = 0;
                szString7[0] = 0;
            }

            if (iClientNum == cg.snap->ps.clientNum) {
                pItemTextColor = vThisClientTextColor;
                pItemBackColor = vThisClientBackColor;
            } else {
                pItemTextColor = vNoTeamTextColor;
                pItemBackColor = vNoTeamBackColor;
            }

            isSpectator = inSpectatorSection && iClientNum >= 0;
        }

        cgi.UI_SetScoreBoardItem(
            i,
            szString2,
            szString3,
            szString4,
            szString5,
            szString6,
            szString7,
            NULL,
            NULL,
            pItemTextColor,
            pItemBackColor,
            bIsHeader
        );

        CG_Scoreboard_AddModernRow(
            iClientNum,
            iClientTeam,
            bIsHeader,
            isSpectator,
            bIsDead,
            szString2,
            szString3,
            szString4,
            szString5,
            szString6,
            szString7,
            pItemTextColor,
            pItemBackColor
        );
        }
    }

    cgi.UI_DeleteScoreBoardItems(iEntryCount);
    CG_Scoreboard_EndModernParse(iEntryCount);
}

void CG_ParseScores_ver_6()
{
    int      i;
    int      iEntryCount;
    int      iClientTeam, iClientNum;
    int      iDatumCount;
    int      iMatchTeam;
    int      iCurrentEntry;
    qboolean bIsDead, bIsHeader;
    char     szString2[MAX_STRING_TOKENS];
    char     szString3[MAX_STRING_TOKENS];
    char     szString4[MAX_STRING_TOKENS];
    char     szString5[MAX_STRING_TOKENS];
    char     szString6[MAX_STRING_TOKENS];
    float    vSameTeamTextColor[4];
    float    vSameTeamBackColor[4];
    float    vOtherTeamTextColor[4];
    float    vOtherTeamBackColor[4];
    float    vNoTeamTextColor[4];
    float    vNoTeamBackColor[4];
    float    vThisClientTextColor[4];
    float    vThisClientBackColor[4];
    float    vDeadTextColorDead[4];
    float   *pItemTextColor;
    float   *pItemBackColor;

    iMatchTeam             = -1;
    vSameTeamTextColor[0]  = 1.0f;
    vSameTeamTextColor[1]  = 1.0f;
    vSameTeamTextColor[2]  = 1.0f;
    vSameTeamTextColor[3]  = 1.0f;
    vSameTeamBackColor[0]  = 0.1f;
    vSameTeamBackColor[1]  = 0.5f;
    vSameTeamBackColor[2]  = 0.1f;
    vSameTeamBackColor[3]  = 0.4f;
    vOtherTeamTextColor[0] = 1.0f;
    vOtherTeamTextColor[1] = 1.0f;
    vOtherTeamTextColor[2] = 1.0f;
    vOtherTeamTextColor[3] = 1.0f;
    vOtherTeamBackColor[0] = 0.5f;
    vOtherTeamBackColor[1] = 0.1f;
    vOtherTeamBackColor[2] = 0.1f;
    vOtherTeamBackColor[3] = 0.4f;
    vNoTeamTextColor[0]    = 1.0f;
    vNoTeamTextColor[1]    = 1.0f;
    vNoTeamTextColor[2]    = 1.0f;
    vNoTeamTextColor[3]    = 1.0f;
    vNoTeamBackColor[0]    = 0.1f;
    vNoTeamBackColor[1]    = 0.1f;
    vNoTeamBackColor[2]    = 0.1f;
    vNoTeamBackColor[3]    = 0.4f;
    vDeadTextColorDead[0]  = 1.0f;
    vDeadTextColorDead[1]  = 0.1f;
    vDeadTextColorDead[2]  = 0.1f;
    vDeadTextColorDead[3]  = 1.0f;

    vThisClientTextColor[0] = 0.0f;
    vThisClientTextColor[1] = 0.0f;
    vThisClientTextColor[2] = 0.0f;
    vThisClientTextColor[3] = 1.0f;

    if (cgs.gametype > GT_FFA) {
        vThisClientBackColor[0] = 0.5f;
        vThisClientBackColor[1] = 0.75f;
        vThisClientBackColor[2] = 0.5f;
    } else {
        vThisClientBackColor[0] = 0.75f;
        vThisClientBackColor[1] = 0.75f;
        vThisClientBackColor[2] = 0.75f;
    }

    vThisClientBackColor[3] = 0.8f;

    iCurrentEntry = 1;
    if (cgs.gametype > GT_FFA) {
        iDatumCount = 6;
        iMatchTeam  = cg.snap->ps.stats[STAT_TEAM];
        if (iMatchTeam != TEAM_ALLIES && iMatchTeam != TEAM_AXIS) {
            iMatchTeam              = TEAM_ALLIES;
            vThisClientTextColor[0] = 0.0f;
            vThisClientTextColor[1] = 0.0f;
            vThisClientTextColor[2] = 0.0f;
            vThisClientBackColor[0] = 0.75f;
            vThisClientBackColor[1] = 0.75f;
            vThisClientBackColor[2] = 0.75f;
        }
    } else {
        // free-for-all
        iDatumCount = 5;
    }

    iEntryCount = atoi(cgi.Argv(iCurrentEntry++));
    if (iEntryCount > MAX_CLIENTS) {
        iEntryCount = MAX_CLIENTS;
    }

    CG_Scoreboard_BeginModernParse();

    {
        qboolean inSpectatorSection = qfalse;

        for (i = 0; i < iEntryCount; ++i) {
            qboolean isSpectator = qfalse;
            bIsHeader = qfalse;
            bIsDead   = qfalse;
        if (cgs.gametype > GT_FFA) {
            iClientNum  = atoi(cgi.Argv(iCurrentEntry + iDatumCount * i));
            iClientTeam = atoi(cgi.Argv(1 + iCurrentEntry + iDatumCount * i));
            if (iClientTeam >= 0) {
                bIsDead = qfalse;
            } else {
                bIsDead     = qtrue;
                iClientTeam = -iClientTeam;
            }

            if (iClientNum == -1) {
                bIsHeader = qtrue;

                switch (iClientTeam) {
                case 1:
                    Q_strncpyz(szString2, cgi.LV_ConvertString("Spectators"), sizeof(szString2));
                    break;
                case 2:
                    Q_strncpyz(szString2, cgi.LV_ConvertString("Free-For-Allers"), sizeof(szString2));
                    break;
                case 3:
                    Q_strncpyz(szString2, cgi.LV_ConvertString("Allies"), sizeof(szString2));
                    break;
                case 4:
                    Q_strncpyz(szString2, cgi.LV_ConvertString("Axis"), sizeof(szString2));
                    break;
                default:
                    Q_strncpyz(szString2, cgi.LV_ConvertString("No Team"), sizeof(szString2));
                    break;
                }
            } else if (iClientNum == -2) {
                // spectating
                szString2[0] = 0;
            } else {
                Q_strncpyz(szString2, cg.clientinfo[iClientNum].name, sizeof(szString2));
            }

            Q_strncpyz(szString3, cgi.Argv(2 + iCurrentEntry + iDatumCount * i), sizeof(szString3));
            Q_strncpyz(szString4, cgi.Argv(3 + iCurrentEntry + iDatumCount * i), sizeof(szString4));
            Q_strncpyz(szString5, cgi.Argv(4 + iCurrentEntry + iDatumCount * i), sizeof(szString5));
            Q_strncpyz(szString6, cgi.Argv(5 + iCurrentEntry + iDatumCount * i), sizeof(szString6));

            /*
             * Added in OPM: ver6 round headers put m_teamwins in deaths (szString4);
             * TDM headers put team kills in kills (szString3).
             */
            if (bIsHeader && (iClientTeam == TEAM_ALLIES || iClientTeam == TEAM_AXIS)) {
                if (cgs.gametype >= GT_TEAM_ROUNDS) {
                    CG_Hud_SetTeamScoreCvar(iClientTeam, szString4);
                } else {
                    CG_Hud_SetTeamScoreCvar(iClientTeam, szString3);
                }
            }

            if (iClientNum == cg.snap->ps.clientNum) {
                pItemTextColor = vThisClientTextColor;
                pItemBackColor = vThisClientBackColor;
            } else if (iClientNum == -2) {
                pItemTextColor = vNoTeamTextColor;
                pItemBackColor = vNoTeamBackColor;
            } else if (iClientTeam == TEAM_ALLIES || iClientTeam == TEAM_AXIS) {
                if (iClientTeam == iMatchTeam) {
                    pItemTextColor = vSameTeamTextColor;
                    pItemBackColor = vSameTeamBackColor;
                } else {
                    pItemTextColor = vOtherTeamTextColor;
                    pItemBackColor = vOtherTeamBackColor;
                }
            } else {
                pItemTextColor = vNoTeamTextColor;
                pItemBackColor = vNoTeamBackColor;
            }

            if (bIsDead) {
                pItemTextColor = vDeadTextColorDead;
            }

            if (iClientNum == -1 && iClientTeam == TEAM_SPECTATOR) {
                inSpectatorSection = qtrue;
            }
            isSpectator = inSpectatorSection && iClientNum >= 0;
        } else {
            iClientTeam = TEAM_FREEFORALL;
            iClientNum = atoi(cgi.Argv(iCurrentEntry + iDatumCount * i));
            if (iClientNum >= 0) {
                Q_strncpyz(szString2, cg.clientinfo[iClientNum].name, sizeof(szString2));
                Q_strncpyz(szString3, cgi.Argv(1 + iCurrentEntry + iDatumCount * i), sizeof(szString3));
                Q_strncpyz(szString4, cgi.Argv(2 + iCurrentEntry + iDatumCount * i), sizeof(szString4));
                Q_strncpyz(szString5, cgi.Argv(3 + iCurrentEntry + iDatumCount * i), sizeof(szString5));
                Q_strncpyz(szString6, cgi.Argv(4 + iCurrentEntry + iDatumCount * i), sizeof(szString6));
            } else {
                if (iClientNum == -3) {
                    Q_strncpyz(szString2, cgi.LV_ConvertString("Players"), sizeof(szString2));
                    bIsHeader = qtrue;
                    inSpectatorSection = qfalse;
                } else if (iClientNum == -2) {
                    Q_strncpyz(szString2, cgi.LV_ConvertString("Spectators"), sizeof(szString2));
                    bIsHeader = qtrue;
                    inSpectatorSection = qtrue;
                } else {
                    // unknown
                    szString2[0] = 0;
                }
                szString3[0] = 0;
                szString4[0] = 0;
                szString5[0] = 0;
                szString6[0] = 0;
            }

            if (iClientNum == cg.snap->ps.clientNum) {
                pItemTextColor = vThisClientTextColor;
                pItemBackColor = vThisClientBackColor;
            } else {
                pItemTextColor = vNoTeamTextColor;
                pItemBackColor = vNoTeamBackColor;
            }

            isSpectator = inSpectatorSection && iClientNum >= 0;
        }

        cgi.UI_SetScoreBoardItem(
            i,
            szString2,
            szString3,
            szString4,
            szString5,
            szString6,
            NULL,
            NULL,
            NULL,
            pItemTextColor,
            pItemBackColor,
            bIsHeader
        );

        CG_Scoreboard_AddModernRow(
            iClientNum,
            iClientTeam,
            bIsHeader,
            isSpectator,
            bIsDead,
            "",
            szString2,
            szString3,
            szString4,
            szString5,
            szString6,
            pItemTextColor,
            pItemBackColor
        );
        }
    }

    cgi.UI_DeleteScoreBoardItems(iEntryCount);
    CG_Scoreboard_EndModernParse(iEntryCount);
}

void CG_ParseScores()
{
    if (cg_protocol >= PROTOCOL_MOHTA_MIN) {
        CG_ParseScores_ver_15();
    } else {
        CG_ParseScores_ver_6();
    }
}

void CG_InitScoresAPI(clientGameExport_t *cge)
{
    cge->CG_GetScoreBoardColor      = CG_GetScoreBoardColor;
    cge->CG_GetScoreBoardFontColor  = CG_GetScoreBoardFontColor;
    cge->CG_GetScoreBoardPosition   = CG_GetScoreBoardPosition;
    cge->CG_GetScoreBoardDrawHeader = CG_GetScoreBoardDrawHeader;

    if (cg_protocol >= PROTOCOL_MOHTA_MIN) {
        cge->CG_GetColumnName = &CG_GetColumnName_ver_15;
    } else {
        cge->CG_GetColumnName = &CG_GetColumnName_ver_6;
    }
}
