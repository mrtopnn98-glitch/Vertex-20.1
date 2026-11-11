#include "krx_visuals_pre_tas.h"

#include <base/dbg.h>
#include <base/detect.h>
#include <base/log.h>
#include <base/math.h>
#include <base/mem.h>
#include <base/str.h>
#include <base/time.h>
#include <base/io.h>
#include <base/fs.h>
#include <base/time.h>
#include <engine/shared/config.h>

#include <game/client/gameclient.h>
#include <game/client/components/controls.h>
#include <game/collision.h>

namespace OpenKrx
{
        bool CPreTasVisuals::ShouldSuspendAvoid()
        {
                // Original: 0x1402cc7a0; grounded helper 0x1402b4540. Preserve strict
                // comparison, timer rearming, and the original two-direction-key gate.
                // (Портировано дословно из OpenKRX; GameClient()/Collision() заменены
                // на сохранённый указатель, т.к. это не CComponent.)
                if(!g_Config.m_AvoidAfkProtection)
                        return false;
                if(!m_pGameClient)
                        return false;
                bool Expired = false;
                if(g_Config.m_AvoidAfkTime != 0)
                {
                        const int64_t Period = (int64_t)g_Config.m_AvoidAfkTime * time_freq();
                        if(m_AfkLastActivity < time_get() - Period)
                        {
                                Expired = true;
                                m_AfkLastActivity = time_get() - Period - 1;
                        }
                }
                const int Slot = g_Config.m_ClDummy;
                const int Id = m_pGameClient->m_aLocalIds[Slot];
                bool Grounded = false;
                if(Id >= 0 && Id < MAX_CLIENTS)
                {
                        const vec2 Pos = m_pGameClient->m_aClients[Id].m_RenderPos;
                        Grounded = m_pGameClient->Collision()->CheckPoint(Pos + vec2(14, 19)) ||
                                   m_pGameClient->Collision()->CheckPoint(Pos + vec2(-14, 19)) ||
                                   (m_pGameClient->Collision()->GetMoveRestrictions(Pos + vec2(0, 18), 0.0f) & CANTMOVE_DOWN);
                }
                const auto &Controls = m_pGameClient->m_Controls;
                const vec2 Mouse = Controls.m_aMousePos[Slot];
                if((Grounded || Expired) && Controls.m_aInputDirectionLeft[Slot] && Controls.m_aInputDirectionRight[Slot] &&
                        (m_AfkMouse.x != 0 || m_AfkMouse.y != 0) && m_AfkMouse == Mouse)
                        return Expired;
                m_AfkMouse = Mouse;
                m_AfkLastActivity = time_get();
                return false;
        }
}
