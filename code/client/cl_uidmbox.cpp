/*
===========================================================================
Copyright (C) 2024 the OpenMoHAA team

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

#include "cl_ui.h"
#include "cl_uirender.h"
#include "cl_messages_host.h"
#include "cl_killfeed.h"
#include "../qcommon/localization.h"

Event EV_DMBox_Goin
(
	"_dmbox_goin",
	EV_DEFAULT,
	NULL,
	NULL,
	"Event to make the dmbox disappear"
);

Event EV_DMBox_Decay
(
	"_dmbox_decay",
	EV_DEFAULT,
	NULL,
	NULL,
	"Event to make the dmbox console line decay"
);

static float s_dmboxWidth   = 384.0;
static float s_dmboxOffsetX = 3.0f;
static float s_dmboxOffsetY = 8.0f;

CLASS_DECLARATION(UIWidget, UIDMBox, NULL) {
    {&W_SizeChanged,  &UIDMBox::OnSizeChanged},
    {&EV_DMBox_Goin,  &UIDMBox::MoveInEvent  },
    {&EV_DMBox_Decay, &UIDMBox::DecayEvent   },
    {NULL,            NULL                   }
};

UIDMBox::UIDMBox()
{
    m_numitems    = 0;
    m_reallyshown = true;
    m_fontbold    = NULL;
    m_boxstate    = boxstate_t::box_out;
    m_iBeginDecay = 0;
    m_boxtime     = uid.time;
    m_movespeed   = 500;
    m_drawoutline = com_target_game->integer >= target_game_e::TG_MOHTA;
}

void UIDMBox::VerifyBoxOut(void)
{
    PostMoveinEvent();
    if (m_boxstate != boxstate_t::box_moving_out && m_boxstate != boxstate_t::box_out) {
        ChangeBoxState(boxstate_t::box_moving_out);
    }
}

void UIDMBox::ChangeBoxState(boxstate_t state)
{
    m_boxstate = state;
    m_boxtime  = uid.time;
    setShowState();

    if (state == box_out) {
        PostMoveinEvent();
    }
}

void UIDMBox::HandleBoxMoving(void)
{
    int      delta;
    UIRect2D newRect;

    if (m_boxstate != boxstate_t::box_moving_out && m_boxstate != boxstate_t::box_moving_in) {
        return;
    }

    delta     = m_movespeed * (uid.time - m_boxtime) / 1000;
    m_boxtime = 1000 * delta / m_movespeed + m_boxtime;
    if (m_boxstate == boxstate_t::box_moving_out) {
        newRect.size.width  = m_frame.size.width;
        newRect.size.height = m_frame.size.height;
        newRect.pos.x       = m_frame.pos.x;
        newRect.pos.y       = delta + m_frame.pos.y;

        if (newRect.pos.y <= 0.0) {
            newRect.pos.y = 0.0;
            ChangeBoxState(boxstate_t::box_out);
        }
    } else if (m_boxstate == boxstate_t::box_moving_in) {
        newRect.size.width  = m_frame.size.width;
        newRect.size.height = m_frame.size.height;
        newRect.pos.x       = m_frame.pos.x;
        newRect.pos.y       = delta - m_frame.pos.y;

        if (newRect.pos.y <= -newRect.size.height) {
            newRect.pos.y = -newRect.size.height;
            ChangeBoxState(boxstate_t::box_in);
        }
    } else {
        newRect = m_frame;
    }

    setFrame(newRect);
}

void UIDMBox::PostMoveinEvent(void)
{
    if (m_boxstate != boxstate_t::box_out) {
        return;
    }

    if (!EventPending(EV_DMBox_Goin)) {
        PostEvent(EV_DMBox_Goin, 10.0);
    } else {
        PostponeEvent(EV_DMBox_Goin, 10.0);
    }
}

void UIDMBox::PostDecayEvent(void)
{
    /* Changed in OPM: modern HUD foreach lifetime owns expiry; capacity trim only. */
    if (CL_UIR_UseModernHudPack()) {
        return;
    }

    if (!EventPending(EV_DMBox_Decay)) {
        float       fDelayTime;
        int         iNumLines;
        int         i;
        const char *pszString = m_items[0].string.c_str();

        //
        // Calculate the number of lines
        //
        iNumLines = 1;
        for (i = 0; pszString[i]; i++) {
            if (pszString[i] == '\n') {
                iNumLines++;
            }
        }

        if (m_items[0].flags & DMBOX_ITEM_FLAG_BOLD) {
            fDelayTime = iNumLines * 8.0;
        }
        //
        // Bold as twice more decay
        //
        else if (m_items[0].flags & DMBOX_ITEM_FLAG_DEATH) {
            fDelayTime = iNumLines * 6.0;
        } else {
            fDelayTime = iNumLines * 5.0;
        }

        /* Fixed in OPM: tiny wrap widths create huge line counts and multi-minute
         * decay timers that freeze the modern kill/chat feed at capacity. */
        if (fDelayTime < 1.0f) {
            fDelayTime = 1.0f;
        } else if (fDelayTime > 20.0f) {
            fDelayTime = 20.0f;
        }

        m_iBeginDecay = cls.realtime;
        m_iEndDecay   = (int)(fDelayTime * 1000.0);

        PostEvent(EV_DMBox_Decay, fDelayTime);
    }
}

void UIDMBox::setShowState(void)
{
    if (m_reallyshown) {
        setShow(m_boxstate != box_in);
    } else {
        setShow(false);
    }
}

void UIDMBox::RemoveTopItem(void)
{
    int i;

    if (m_numitems > 0) {
        for (i = 0; i < m_numitems - 1; i++) {
            m_items[i] = m_items[i + 1];
        }

        m_numitems--;
    }
}

str UIDMBox::CalculateBreaks(UIFont *font, str text, float max_width)
{
    str         newText;
    float       fX;
    float       fwX;
    const char *current;
    int         count;

    current = text;
    fX      = 0.0;

    for (count = font->DBCSGetWordBlockCount(current, -1); count;
         current += count, count = font->DBCSGetWordBlockCount(current, -1)) {
        fwX = font->getWidth(current, count);

        if (fX + fwX > max_width) {
            newText += "\n" + str(current, 0, count);
            fX = 0;
        } else {
            newText += str(current, 0, count);
        }

        fX += fwX;
    }

    return newText;
}

float UIDMBox::PrintWrap(UIFont *font, float x, float y, str text)
{
    const char *p1, *p2;
    size_t      n, l;
    float       fY;

    fY = y;
    p1 = text.c_str();
    l  = text.length();

    for (;;) {
        p2 = strchr(p1, '\n');
        if (!p2) {
            break;
        }

        n = p2 - p1;
        if (n >= l) {
            break;
        }

        font->Print(x, fY, p1, p2 - p1, getHighResScale());
        p1 = p2 + 1;
        l -= n;
        fY += font->getHeight();
    }

    if (*p1) {
        font->Print(x, fY, p1, l, getHighResScale());
        fY += font->getHeight();
    }

    return fY - y;
}

float UIDMBox::DrawItem(item_t *in, float x, float y, float alpha)
{
    if (m_drawoutline) {
        //
        // Draw an outline
        //

        in->font->setColor(UBlack);
        in->font->setAlpha(alpha);

        PrintWrap(in->font, x + 1, y + 2, in->string);
        PrintWrap(in->font, x + 2, y + 1, in->string);
        PrintWrap(in->font, x - 1, y + 2, in->string);
        PrintWrap(in->font, x - 2, y + 1, in->string);
        PrintWrap(in->font, x - 1, y - 2, in->string);
        PrintWrap(in->font, x - 2, y - 1, in->string);
        PrintWrap(in->font, x + 1, y - 2, in->string);
        PrintWrap(in->font, x + 2, y - 1, in->string);
        PrintWrap(in->font, x + 2, y, in->string);
        PrintWrap(in->font, x - 2, y, in->string);
        PrintWrap(in->font, x, y + 2, in->string);
        PrintWrap(in->font, x, y - 2, in->string);
    }

    in->font->setColor(in->color);
    in->font->setAlpha(alpha);

    return PrintWrap(in->font, x, y, in->string);
}

void UIDMBox::Print(const char *text)
{
    const char *text1 = text;

    /* Changed in OPM: capacity aligned with UIR_HUD_MESSAGES_MAX_ROWS. */
    if (m_numitems >= UIR_HUD_MESSAGES_MAX_ROWS) {
        RemoveTopItem();
    }

    m_items[m_numitems].flags = 0;

    if (*text == MESSAGE_CHAT_WHITE) {
        m_items[m_numitems].color = UWhiteChatMessageColor;
        m_items[m_numitems].font  = m_fontbold;
        m_items[m_numitems].flags |= DMBOX_ITEM_FLAG_BOLD;

        text1 = text + 1;
    } else if (*text == MESSAGE_CHAT_RED) {
        m_items[m_numitems].color = URedChatMessageColor;
        m_items[m_numitems].font  = m_fontbold;
        m_items[m_numitems].flags |= DMBOX_ITEM_FLAG_DEATH;

        text1 = text + 1;
    } else if (*text == MESSAGE_CHAT_GREEN) {
        m_items[m_numitems].color = UGreenChatMessageColor;
        m_items[m_numitems].font  = m_fontbold;
        m_items[m_numitems].flags |= DMBOX_ITEM_FLAG_DEATH;

        text1 = text + 1;
    } else {
        m_items[m_numitems].color = m_foreground_color;
        m_items[m_numitems].font  = m_font;
    }
    /* Added in OPM: Base (protocol < TA) deaths are print→dmbox only; feed kill-feed here.
     * TA uses printdeathmsg (CL_KillFeed_HandlePrintDeathMsg) then also Printf into dmbox —
     * skip here to avoid duplicate rows. */
    if ((m_items[m_numitems].flags & DMBOX_ITEM_FLAG_DEATH) && com_protocol
        && com_protocol->integer < PROTOCOL_MOHTA_MIN) {
        CL_KillFeed_HandleDeathPrint(text1, (*text == MESSAGE_CHAT_GREEN) ? 1 : 0);
    }

    m_items[m_numitems].string =
        CalculateBreaks(m_items[m_numitems].font, Sys_LV_CL_ConvertString(text1),
                        s_dmboxWidth < 64.0f ? 64.0f : s_dmboxWidth);

    /* Added in OPM: monotonic id for hud-messages foreach lifetime. */
    {
        static uint64_t s_nextHudMessageId = 1;
        m_items[m_numitems].stableId = s_nextHudMessageId++;
    }

    m_numitems++;
    VerifyBoxOut();
    PostDecayEvent();
}

void UIDMBox::OnSizeChanged(Event *ev)
{
    s_dmboxWidth = m_frame.size.width;
}

void UIDMBox::Create(const UIRect2D& rect, const UColor& fore, const UColor& back, float alpha)
{
    InitFrame(NULL, rect, 0, "facfont-20");

    if (!m_fontbold) {
        m_fontbold = new UIFont("facfont-20");
    }

    m_fontbold->setColor(URed);
    setBackgroundColor(back, true);
    setForegroundColor(fore);
    setBackgroundAlpha(alpha);

    Connect(this, W_SizeChanged, W_SizeChanged);
    OnSizeChanged(NULL);

    m_movespeed = rect.size.height * 3.0;

    setShowState();
}

void UIDMBox::MoveInEvent(Event *ev) {}

void UIDMBox::DecayEvent(Event *ev)
{
    RemoveTopItem();
    if (m_numitems) {
        PostDecayEvent();
    }
}

/*
====================
UIDMBox::ForceDueDecay
Fixed in OPM: modern HUD publishes via Draw and never runs the legacy overflow
force-decay. If the scheduled EV_DMBox_Decay is lost/stuck, rows freeze at max.
Drive the same RemoveTopItem path from Draw when the deadline has elapsed.
====================
*/
void UIDMBox::ForceDueDecay(void)
{
    /* Changed in OPM: modern HUD skips timer decay (foreach lifetime). */
    if (CL_UIR_UseModernHudPack()) {
        return;
    }

    if (m_numitems <= 0 || m_iEndDecay <= 0 || m_iBeginDecay <= 0) {
        return;
    }
    if (cls.realtime - m_iBeginDecay < m_iEndDecay) {
        return;
    }
    if (EventPending(EV_DMBox_Decay)) {
        CancelEventsOfType(EV_DMBox_Decay);
    }
    RemoveTopItem();
    if (m_numitems) {
        PostDecayEvent();
    }
}

void UIDMBox::Draw(void)
{
    float fsY;
    int   i;
    float alpha;
    float alphaScale;

    alphaScale = 0.8f;
    HandleBoxMoving();

    if (!m_numitems) {
        if (CL_UIR_UseModernHudPack()) {
            /* Fixed in OPM: only publish an empty snapshot when rows were present.
             * Calling Clear+NotifyChanged every idle frame bumped collection revision
             * and forced classic HUD foreach/layout thrash. */
            if (UIR_HudMessages_GetRowCount() > 0) {
                UIR_HudMessages_Clear();
                UIR_HudMessages_NotifyChanged();
            }
            /* Added in OPM: chat-only collection mirrors mixed clear. */
            if (UIR_HudChat_GetRowCount() > 0) {
                UIR_HudChat_Clear();
                UIR_HudChat_NotifyChanged();
            }
        }
        return;
    }

    /* Added in OPM: modern HUD pack paints chat via hud-messages collection. */
    if (CL_UIR_UseModernHudPack()) {
        uir_hud_message_input_t row;

        if (!m_numitems) {
            if (UIR_HudMessages_GetRowCount() > 0) {
                UIR_HudMessages_Clear();
                UIR_HudMessages_NotifyChanged();
            }
            if (UIR_HudChat_GetRowCount() > 0) {
                UIR_HudChat_Clear();
                UIR_HudChat_NotifyChanged();
            }
            return;
        }

        alphaScale = 0.8f;
        if (cge) {
            alphaScale = static_cast<float>(1.0 - cge->CG_GetObjectiveAlpha());
        }

        UIR_HudMessages_Clear();
        UIR_HudMessages_SetAlphaScale(alphaScale);
        UIR_HudChat_Clear();
        UIR_HudChat_SetAlphaScale(alphaScale);
        for (i = 0; i < m_numitems; i++) {
            std::memset(&row, 0, sizeof(row));
            row.text = m_items[i].string.c_str();
            row.colorR = m_items[i].color.r;
            row.colorG = m_items[i].color.g;
            row.colorB = m_items[i].color.b;
            row.colorA = m_items[i].color.a;
            row.bold = (m_items[i].flags & DMBOX_ITEM_FLAG_BOLD) ? 1 : 0;
            row.stableId = m_items[i].stableId;
            UIR_HudMessages_AddRow(&row);
            /* Added in OPM: hud-chat excludes death/kill lines. */
            if (!(m_items[i].flags & DMBOX_ITEM_FLAG_DEATH)) {
                UIR_HudChat_AddRow(&row);
            }
        }
        UIR_HudMessages_NotifyChanged();
        UIR_HudChat_NotifyChanged();
        return;
    }

    m_font->setColor(m_foreground_color);
    alpha = (float)(cls.realtime - m_iBeginDecay) / (float)m_iEndDecay;
    if (alpha > 1.0) {
        alpha = 1.0;
    }

    alpha = (1.0 - alpha) * 4.0;
    if (alpha > 1.0) {
        alpha = 1.0;
    }

    if (cge) {
        alphaScale = 1.0 - cge->CG_GetObjectiveAlpha();
    }

    fsY = DrawItem(m_items, s_dmboxOffsetX, s_dmboxOffsetY, alpha * alphaScale);
    fsY = alpha <= 0.2 ? s_dmboxOffsetY : fsY + s_dmboxOffsetY;

    for (i = 1; i < m_numitems; i++) {
        fsY += DrawItem(&m_items[i], s_dmboxOffsetX, fsY, alphaScale);
        if (fsY > m_frame.size.height) {
            if (EventPending(EV_DMBox_Decay)) {
                CancelEventsOfType(EV_DMBox_Decay);
            }

            PostEvent(EV_DMBox_Decay, 0.0);
            break;
        }
    }
}

void UIDMBox::setRealShow(bool b)
{
    this->m_reallyshown = b;
    setShowState();
}

void UIDMBox::Clear(void)
{
    m_numitems = 0;
}
