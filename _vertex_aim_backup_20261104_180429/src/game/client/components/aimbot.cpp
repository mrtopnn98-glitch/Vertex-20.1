/* Vertex client — Vertex Aim System. Документация модуля — aimbot.h. */
#include "aimbot.h"
#include "aled.h"

#include <base/color.h>
#include <base/dbg.h>
#include <base/system.h>

#include <engine/client.h>
#include <engine/graphics.h>
#include <engine/input.h>
#include <engine/shared/config.h>

#include <generated/protocol.h>

#include <game/collision.h>
#include <game/gamecore.h>
#include <game/mapitems.h>
#include <game/tuning.h>

#include <game/client/components/controls.h>
#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/prediction/gameworld.h>

#include <algorithm>
#include <cmath>

// ============================================================================
// константы (как в реальной физике DDNet)
// ============================================================================
static constexpr float AB_PI = 3.14159265f;
static constexpr float AB_PHYS = 28.0f; // CCharacterCore::PhysicalSize()
static constexpr float AB_HAMMER_HIT = 74.0f; // дистанция удара молота (vanilla)
static constexpr float AB_BULLET_HIT = 22.0f; // пуля: центр цели + запас хитбокса
static constexpr float AB_GRENADE_HIT = 48.0f; // граната: ~половина радиуса взрыва
static constexpr float AB_LASER_HIT = 40.0f; // лазерный луч
static constexpr float AB_AIM_LEN = 256.0f; // длина вектора прицела в input
static constexpr float AB_BUDGET_MS = 2.5f; // бюджет расчётов на тик
static constexpr int AB_MAX_PRED = 30; // предел глубины предикта
static constexpr int AB_TRAJ_MAX = 96; // предел точек траектории для визуала

// ============================================================================
// мелкие помощники
// ============================================================================
static float AbAngleDev(float A, float B)
{
        float D = A - B;
        while(D > AB_PI)
                D -= 2.0f * AB_PI;
        while(D < -AB_PI)
                D += 2.0f * AB_PI;
        return std::fabs(D);
}
static float AbAngleDelta(float From, float To)
{
        float D = To - From;
        while(D > AB_PI)
                D -= 2.0f * AB_PI;
        while(D < -AB_PI)
                D += 2.0f * AB_PI;
        return D;
}
// вход из списка "nick1, nick2" содержится в имени цели
static bool AbNameMatch(const char *pList, const char *pName)
{
        if(!pList || !pList[0] || !pName || !pName[0])
                return false;
        const char *p = pList;
        while(*p)
        {
                char aItem[64];
                int n = 0;
                while(*p && *p != ',' && *p != ';' && n < 63)
                        aItem[n++] = *p++;
                if(*p)
                        p++;
                aItem[n] = 0;
                int St = 0;
                while(aItem[St] == ' ')
                        St++;
                while(n > St && aItem[n - 1] == ' ')
                        n--;
                aItem[n] = 0;
                if(n > St && str_find_nocase(pName, aItem + St))
                        return true;
        }
        return false;
}
// тюнинги сервера через предиктивный мир (зона тюнинга на моей позиции)
static const CTuningParams *AbTuning(CGameClient *pGame, CCollision *pCol, const vec2 &Pos)
{
        const int Zone = pCol->IsTune(pCol->GetMapIndex(Pos));
        return pGame->m_PredictedWorld.GetTuning(Zone);
}

// ============================================================================
// предикт пути цели: копия предиктивного мира, цель тикает со своим вводом
// ============================================================================
struct SAbPath
{
        vec2 m_aP[AB_MAX_PRED + 1];
        vec2 m_aV[AB_MAX_PRED + 1];
        bool m_aF[AB_MAX_PRED + 1];
        int m_Len;
};
static void AbInitPath(const vec2 &Pos, const vec2 &Vel, SAbPath &Out)
{
        Out.m_aP[0] = Pos;
        Out.m_aV[0] = Vel;
        Out.m_aF[0] = false;
        Out.m_Len = 0;
}
static bool AbTargetPath(CGameClient *pGame, int TgtId, int Len, SAbPath &Out)
{
        Len = std::clamp(Len, 1, AB_MAX_PRED);
        CGameWorld SimWorld;
        SimWorld.CopyWorld(&pGame->m_PredictedWorld);
        CCharacter *pT = SimWorld.GetCharacterById(TgtId);
        if(!pT)
                return false;
        // дальние тики не влияют на полёт снаряда — выбрасываем их из копии
        for(int i = 0; i < MAX_CLIENTS; i++)
        {
                if(i == TgtId)
                        continue;
                CCharacter *pO = SimWorld.GetCharacterById(i);
                if(pO && distance(pO->m_Pos, pT->m_Pos) > 500.0f)
                        delete pO;
        }
        pT = SimWorld.GetCharacterById(TgtId);
        if(!pT)
                return false;
        AbInitPath(pT->m_Pos, pT->Core()->m_Vel, Out);
        Out.m_aF[0] = pT->m_FreezeTime > 0;
        const int StartTick = SimWorld.GameTick();
        for(int s = 1; s <= Len; s++)
        {
                SimWorld.m_GameTick = StartTick + s;
                SimWorld.Tick();
                pT = SimWorld.GetCharacterById(TgtId);
                if(!pT)
                        break; // исчез (смерть / телепорт)
                Out.m_aP[s] = pT->m_Pos;
                Out.m_aV[s] = pT->Core()->m_Vel;
                Out.m_aF[s] = pT->m_FreezeTime > 0;
                Out.m_Len = s;
        }
        return true;
}

// ============================================================================
// баллистика снарядов: пистолет / дробовик / граната.
// Снаряд летит ровно как в игре: CalcPos(pos, dir, curvature, speed, t),
// curvature и speed — реальные серверные тюнинги (m_*Curvature / m_*Speed).
// ============================================================================
struct SAbBallist
{
        float m_Curvature = 0.0f;
        float m_Speed = 0.0f;
        int m_Lifetime = 40; // тиков
        float m_HitR = 0.0f;
};
static bool AbBallistic(CGameClient *pGame, CCollision *pCol, const vec2 &Pos, int Weapon, SAbBallist &Out)
{
        const CTuningParams *pTun = AbTuning(pGame, pCol, Pos);
        switch(Weapon)
        {
        case WEAPON_GUN:
                Out.m_Curvature = pTun->m_GunCurvature;
                Out.m_Speed = pTun->m_GunSpeed;
                Out.m_HitR = AB_BULLET_HIT;
                Out.m_Lifetime = 40;
                break;
        case WEAPON_SHOTGUN:
                Out.m_Curvature = pTun->m_ShotgunCurvature;
                Out.m_Speed = pTun->m_ShotgunSpeed;
                // дробь: разлёт растёт с дистанцией — радиус поражения шире
                Out.m_HitR = AB_BULLET_HIT + 8.0f;
                Out.m_Lifetime = 30;
                break;
        case WEAPON_GRENADE:
                Out.m_Curvature = pTun->m_GrenadeCurvature;
                Out.m_Speed = pTun->m_GrenadeSpeed;
                Out.m_HitR = AB_GRENADE_HIT;
                Out.m_Lifetime = std::clamp((int)std::round(pTun->m_GrenadeLifetime * (float)pGame->m_PredictedWorld.GameTickSpeed()), 10, 100);
                break;
        default:
                return false;
        }
        return Out.m_Speed > 1.0f;
}

// результат решения по оружию
struct SAbShot
{
        bool m_Have = false;
        float m_Angle = 0.0f;
        int m_Flight = 0; // тик попадания
        float m_Score = 0.0f;
        vec2 m_Point = vec2(0.0f, 0.0f); // мировая точка попадания
};

// трассировка траектории для визуала
struct SAbTraj
{
        vec2 m_aP[AB_TRAJ_MAX];
        int m_Num = 0;
};

// симуляция выстрела по углу: ищем сближение снаряда и цели по тикам
static void AbSimShot(CCollision *pCol, const vec2 &From, const vec2 &Dir, const SAbBallist &W,
        const SAbPath &Tgt, int MaxFlight, float CursorAngle, float FovRad, bool UseFov,
        bool WallHit, SAbTraj *pTraj, SAbShot &Out)
{
        const vec2 DirN = normalize(Dir);
        const float Ang = std::atan2(DirN.y, DirN.x);
        if(UseFov && AbAngleDev(Ang, CursorAngle) > FovRad)
                return;
        vec2 Prev = From;
        vec2 Cur = From;
        int HitTick = -1;
        float BestDist = 1e30f;
        vec2 BestPoint = From;
        if(pTraj)
        {
                pTraj->m_aP[0] = From;
                pTraj->m_Num = 1;
        }
        for(int i = 1; i <= MaxFlight; i++)
        {
                const float TSec = (float)i / 50.0f;
                Cur = CalcPos(From, DirN, W.m_Curvature, W.m_Speed, TSec);
                vec2 ColPos, Before;
                const int Collide = pCol->IntersectLine(Prev, Cur, &ColPos, &Before);
                const int ti = std::min(i, Tgt.m_Len);
                const float D = distance(Cur, Tgt.m_aP[ti]);
                const float DW = distance(ColPos, Tgt.m_aP[ti]);
                if(Collide)
                {
                        // стена: граната полезна и взрывом у стены рядом с целью
                        if(WallHit && DW <= W.m_HitR * 1.2f && (HitTick < 0 || DW < BestDist))
                        {
                                HitTick = i;
                                BestDist = DW;
                                BestPoint = Tgt.m_aP[ti];
                                if(pTraj && pTraj->m_Num < AB_TRAJ_MAX)
                                        pTraj->m_aP[pTraj->m_Num++] = ColPos;
                        }
                        break;
                }
                if(pTraj && pTraj->m_Num < AB_TRAJ_MAX)
                        pTraj->m_aP[pTraj->m_Num++] = Cur;
                if(D < BestDist)
                {
                        BestDist = D;
                        BestPoint = Tgt.m_aP[ti];
                }
                if(D <= W.m_HitR && HitTick < 0)
                        HitTick = i;
                Prev = Cur;
                if(distance(From, Cur) > 1500.0f)
                        break;
        }
        if(HitTick < 0)
                return;
        const float Q = std::max(0.0f, 1.0f - BestDist / std::max(1.0f, W.m_HitR));
        const float S = 0.5f + 0.5f * Q - (float)HitTick / 300.0f;
        if(!Out.m_Have || S > Out.m_Score)
        {
                Out.m_Have = true;
                Out.m_Angle = Ang;
                Out.m_Flight = HitTick;
                Out.m_Score = S;
                Out.m_Point = BestPoint;
        }
}

// лазер: луч с отражениями от стен (для превью и стрельбы отскоком)
static void AbLaserShot(CCollision *pCol, const vec2 &From, const vec2 &Dir, const SAbPath &Tgt,
        float RangeLen, int MaxBounce, float CursorAngle, float FovRad, bool UseFov, SAbTraj *pTraj, SAbShot &Out)
{
        vec2 DirN = normalize(Dir);
        const float Ang = std::atan2(DirN.y, DirN.x);
        if(UseFov && AbAngleDev(Ang, CursorAngle) > FovRad)
                return;
        vec2 Pos = From;
        float Left = RangeLen;
        int Bounce = 0;
        float BestDist = 1e30f;
        vec2 BestPoint = From;
        if(pTraj)
        {
                pTraj->m_aP[0] = From;
                pTraj->m_Num = 1;
        }
        while(Left > 8.0f && Bounce <= MaxBounce)
        {
                const float Step = std::min(64.0f, Left);
                const vec2 End = Pos + DirN * Step;
                vec2 ColPos, Before;
                const int Collide = pCol->IntersectLine(Pos, End, &ColPos, &Before);
                const vec2 Next = Collide ? Before : End;
                for(int ti = 0; ti <= Tgt.m_Len; ti++)
                {
                        const float D = distance(Next, Tgt.m_aP[ti]);
                        if(D < BestDist)
                        {
                                BestDist = D;
                                BestPoint = Tgt.m_aP[ti];
                        }
                }
                if(pTraj && pTraj->m_Num < AB_TRAJ_MAX)
                        pTraj->m_aP[pTraj->m_Num++] = Next;
                Left -= Step;
                if(Collide)
                {
                        Bounce++;
                        // нормаль стены — пробой вокруг точки удара
                        const int ix = (int)std::round(Before.x), iy = (int)std::round(Before.y);
                        vec2 N((pCol->IsSolid(ix - 4, iy) ? 1.0f : 0.0f) - (pCol->IsSolid(ix + 4, iy) ? 1.0f : 0.0f),
                                (pCol->IsSolid(ix, iy - 4) ? 1.0f : 0.0f) - (pCol->IsSolid(ix, iy + 4) ? 1.0f : 0.0f));
                        if(N == vec2(0.0f, 0.0f))
                                N = -DirN;
                        N = normalize(N);
                        DirN = normalize(DirN - N * dot(DirN, N) * 2.0f);
                        Pos = Before + N * 2.0f;
                }
                else
                        Pos = End;
        }
        if(BestDist > AB_LASER_HIT)
                return;
        const float Q = std::max(0.0f, 1.0f - BestDist / AB_LASER_HIT);
        const float S = 0.5f + 0.5f * Q - (float)Bounce * 0.15f;
        if(!Out.m_Have || S > Out.m_Score)
        {
                Out.m_Have = true;
                Out.m_Angle = Ang;
                Out.m_Flight = 1;
                Out.m_Score = S;
                Out.m_Point = BestPoint;
        }
}
// ============================================================================
// молот: бьёт "сейчас" — цель должна быть в радиусе удара уже сейчас
// ============================================================================
static void AbHammerShot(const vec2 &MyPos, const SAbPath &Tgt, float CursorAngle, float FovRad, bool UseFov, SAbShot &Out)
{
        const vec2 P = Tgt.m_aP[std::min(1, Tgt.m_Len)]; // удар придёт на следующий тик сервера
        const float D = distance(P, MyPos);
        if(D > AB_HAMMER_HIT)
                return; // недостижима — не наводимся
        const vec2 DirN = normalize(P - MyPos);
        const float Ang = std::atan2(DirN.y, DirN.x);
        if(UseFov && AbAngleDev(Ang, CursorAngle) > FovRad)
                return;
        const float S = 0.9f + 0.1f * (1.0f - D / AB_HAMMER_HIT);
        if(!Out.m_Have || S > Out.m_Score)
        {
                Out.m_Have = true;
                Out.m_Angle = Ang;
                Out.m_Flight = 1;
                Out.m_Score = S;
                Out.m_Point = P;
        }
}

// ============================================================================
// крюк: игроки — успех = путь крюка чист и цель в длине крюка
// ============================================================================
static void AbHookShot(CCollision *pCol, const vec2 &MyPos, const SAbPath &Tgt, int PredTicks,
        bool Edge, float HookLen, float Range, float CursorAngle, float FovRad, bool UseFov, SAbShot &Out)
{
        const int ti = std::clamp(PredTicks, 1, std::max(1, Tgt.m_Len));
        const vec2 P0 = Tgt.m_aP[ti];
        const float D0 = distance(P0, MyPos);
        const float Reach = std::min(Range > 1.0f ? Range : HookLen, HookLen + 40.0f);
        if(D0 > Reach)
                return;
        vec2 DirN = normalize(P0 - MyPos);
        vec2 P = Edge ? P0 - DirN * (AB_PHYS * 0.4f) : P0; // edge scanner: ближний край хитбокса
        DirN = normalize(P - MyPos);
        const float Ang = std::atan2(DirN.y, DirN.x);
        if(UseFov && AbAngleDev(Ang, CursorAngle) > FovRad)
                return;
        vec2 Hit, Before;
        bool Blocked = false;
        if(pCol->IntersectLine(MyPos, P, &Hit, &Before))
                Blocked = distance(MyPos, Hit) < distance(P, MyPos) - 20.0f;
        const float S = Blocked ? 0.35f : 0.9f - std::min(0.2f, D0 / 2000.0f);
        if(!Out.m_Have || S > Out.m_Score)
        {
                Out.m_Have = true;
                Out.m_Angle = Ang;
                Out.m_Flight = ti;
                Out.m_Score = S;
                Out.m_Point = P;
        }
}

// ============================================================================
// крюк: точки карты — цепляемся за стену на пути к курсору
// ============================================================================
static void AbHookMap(CCollision *pCol, const vec2 &MyPos, const vec2 &CursorVec, float Range, vec2 &OutPoint)
{
        vec2 Dir = CursorVec;
        if(length(Dir) < 1.0f)
                Dir = vec2(1.0f, 0.0f);
        const vec2 End = MyPos + normalize(Dir) * std::min(Range, 400.0f);
        vec2 Hit, Before;
        if(pCol->IntersectLine(MyPos, End, &Hit, &Before))
                OutPoint = Before + normalize(Dir) * 8.0f;
        else
                OutPoint = End;
}

// ============================================================================
// FOV: полу-конус в радианах; 360° = без ограничения
// ============================================================================
struct SAbFov
{
        float m_Rad = AB_PI;
        bool m_Use = false;
};
static SAbFov AbFov(int FovDeg, const vec2 &CursorVec)
{
        SAbFov F;
        F.m_Rad = (float)std::clamp(FovDeg, 1, 360) * AB_PI / 360.0f;
        F.m_Use = FovDeg > 0 && FovDeg < 360 && length(CursorVec) > 1.0f;
        return F;
}

// ============================================================================
// выбор цели
// ============================================================================
bool CAimBot::PickTarget(CGameClient *pGame, CCharacter *pMe, int Own, const vec2 &MyPos, const vec2 &CursorVec, int &OutId)
{
        const int LocalId = pGame->m_Snap.m_LocalClientId;
        const int DummyId = pGame->m_aLocalIds[Own ^ 1];
        const float Radius = g_Config.m_VxAimBotRadius > 0 ? (float)g_Config.m_VxAimBotRadius : 3000.0f;
        const bool UseAng = length(CursorVec) > 1.0f;
        const float CursorAng = UseAng ? std::atan2(CursorVec.y, CursorVec.x) : 0.0f;
        int BestId = -1;
        float BestScore = -1e30f;
        for(int i = 0; i < MAX_CLIENTS; i++)
        {
                if(i == LocalId)
                        continue;
                CCharacter *pT = pGame->m_PredictedWorld.GetCharacterById(i);
                if(!pT)
                        continue;
                if(i == DummyId && g_Config.m_VxAimBotIgnoreDummy != 0)
                        continue;
                if(g_Config.m_VxAimBotIgnoreFriends != 0 && pGame->m_aClients[i].m_Friend)
                        continue;
                if(g_Config.m_VxAimBotIgnoreTeam != 0 && pMe->SameTeam(i))
                        continue;
                if(g_Config.m_VxAimBotIgnoreFrozen != 0 && pT->m_FreezeTime > 0)
                        continue;
                const vec2 Dv = pT->m_Pos - MyPos;
                const float Dist = length(Dv);
                if(Dist > Radius)
                        continue;
                const float AngDev = UseAng ? AbAngleDev(std::atan2(Dv.y, Dv.x), CursorAng) : 0.0f;
                float S;
                switch(std::clamp(g_Config.m_VxAimBotTarget, 0, 4))
                {
                case 1: // Closest
                        S = 100.0f - Dist / Radius * 100.0f;
                        break;
                case 2: // Lowest ping
                {
                        const CNetObj_PlayerInfo *pInfo = pGame->m_Snap.m_aCharacters[i].m_pPlayer;
                        S = 1000.0f - (float)(pInfo ? pInfo->m_Latency : 0);
                        break;
                }
                case 3: // Smart
                        S = (100.0f - AngDev / AB_PI * 100.0f) * ((float)std::clamp(g_Config.m_VxAimBotSmartAngle, 0, 100) / 100.0f) +
                            (100.0f - std::min(100.0f, Dist / 600.0f * 100.0f)) * ((float)std::clamp(g_Config.m_VxAimBotSmartDist, 0, 100) / 100.0f);
                        if(pT->m_FreezeTime > 0)
                                S -= 25.0f;
                        break;
                case 4: // Priority: ники из списка важнее остальных
                        S = AbNameMatch(g_Config.m_VxAimBotPrioNames, pGame->m_aClients[i].m_aName) ? 500.0f - Dist / Radius * 100.0f : 100.0f - Dist / Radius * 100.0f;
                        break;
                default: // Cursor: ближе к прицелу
                        S = 100.0f - AngDev / AB_PI * 100.0f;
                        break;
                }
                if(S > BestScore)
                {
                        BestScore = S;
                        BestId = i;
                }
        }
        // ручная фиксация цели (клавиша Lock) главнее рейтинга
        if(m_LockId >= 0 && m_LockId != LocalId)
        {
                CCharacter *pL = pGame->m_PredictedWorld.GetCharacterById(m_LockId);
                if(pL && distance(pL->m_Pos, MyPos) <= Radius)
                {
                        OutId = m_LockId;
                        return true;
                }
                m_LockId = -1;
        }
        OutId = BestId;
        return BestId >= 0;
}

void CAimBot::RestoreSlot(CNetObj_PlayerInput *pSlot)
{
        pSlot->m_TargetX = m_UserTargetX;
        pSlot->m_TargetY = m_UserTargetY;
        pSlot->m_WantedWeapon = m_UserWanted;
        // fire не трогаем: счётчик нажатий продолжается без фантомного выстрела
}

// ============================================================================
// главный тик
// ============================================================================
static constexpr int AB_HOOK = 100; // виртуальный "слот" крюка
static constexpr int AB_HOOKMAP = 101;

void CAimBot::Update(CGameClient *pGame, IClient *pClient, CControls *pControls, CNetObj_PlayerInput *pSlot)
{
        const bool Debug = g_Config.m_VxAimBotDebug != 0;
        const bool Enabled = g_Config.m_VxAimBotEnable != 0 &&
                             (g_Config.m_VxAimBotHoldBind <= 0 || pGame->Input()->KeyIsPressed(g_Config.m_VxAimBotHoldBind));
        const bool WasActive = m_Aiming;

        auto Deactivate = [&](bool Restore) {
                if(WasActive)
                {
                        if(Restore)
                                RestoreSlot(pSlot);
                        m_Aiming = false;
                        m_TargetId = -1;
                        m_AimWeapon = -1;
                        m_HitChance = 0.0f;
                        m_TrajNum = 0;
                        if(Debug)
                                dbg_msg("vertex-aim", "off");
                }
        };

        if(!Enabled || pClient->State() != IClient::STATE_ONLINE ||
                pGame->m_Chat.IsActive() || pGame->m_Menus.IsActive() ||
                pGame->m_AvoidFreeze.IsBusy() || VertexAutoAled().IsBusy() ||
                pControls->m_LaserUnfreezeAimActive)
        {
                Deactivate(false);
                return;
        }
        const int LocalId = pGame->m_Snap.m_LocalClientId;
        if(LocalId < 0 || pGame->m_Snap.m_SpecInfo.m_Active)
        {
                Deactivate(false);
                return;
        }
        CCharacter *pMe = pGame->m_PredictedWorld.GetCharacterById(LocalId);
        if(!pMe || pMe->m_FreezeTime > 0 || pMe->Core()->m_DeepFrozen)
        {
                Deactivate(false);
                return;
        }

        const int NowTick = pClient->GameTick(g_Config.m_ClDummy);
        if(NowTick < m_LastWorkTick - 50) // реконнект / смена карты: забыть всё
                Reset();
        if(NowTick == m_LastWorkTick) // раз в тик
                return;
        m_LastWorkTick = NowTick;

        const int Own = g_Config.m_ClDummy;
        const vec2 MyPos = pMe->m_Pos;
        const vec2 CursorVec((float)pSlot->m_TargetX, (float)pSlot->m_TargetY);
        const float CursorAngle = (pSlot->m_TargetX || pSlot->m_TargetY) ? std::atan2(CursorVec.y, CursorVec.x) : 0.0f;
        const int ActiveWeapon = pMe->Core()->m_ActiveWeapon;
        const bool Solo = pGame->m_PredictedWorld.m_WorldConfig.m_IsSolo;
        const bool Training = g_Config.m_VxAimBotMode == 3;
        if(Solo && g_Config.m_VxAimBotSolo != 0 && !Training)
        {
                Deactivate(true);
                return;
        }
        m_UserTargetX = pSlot->m_TargetX;
        m_UserTargetY = pSlot->m_TargetY;
        m_UserWanted = pSlot->m_WantedWeapon;

        const int64_t T0 = time_get();
        auto OverBudget = [&]() -> bool {
                return (double)(time_get() - T0) * 1000.0 / (double)time_freq() > AB_BUDGET_MS;
        };

        CCollision *pCol = pMe->Collision();
        const bool HookPressed = pSlot->m_Hook != 0;
        const bool HookMapMode = g_Config.m_VxAimBotHookMode == 1;
        const bool HookAuto = g_Config.m_VxAimBotHookAuto != 0;
        const bool HookAimOn = g_Config.m_ClHookAim != 0;
        const bool WantHook = HookPressed || (HookAimOn && HookAuto);

        // ---- цель ----
        int TgtId = -1;
        if(!HookMapMode)
                PickTarget(pGame, pMe, Own, MyPos, CursorVec, TgtId);
        m_TargetId = TgtId;

        // ---- предикт пути цели ----
        SAbPath TgtPath;
        TgtPath.m_Len = 0;
        TgtPath.m_aP[0] = MyPos;
        TgtPath.m_aV[0] = vec2(0.0f, 0.0f);
        if(TgtId >= 0)
        {
                CCharacter *pTgt = pGame->m_PredictedWorld.GetCharacterById(TgtId);
                if(!pTgt)
                        TgtId = -1;
                else if(g_Config.m_VxAimBotPredTicks > 0)
                {
                        const int P = std::clamp(g_Config.m_VxAimBotPredTicks, 1, AB_MAX_PRED);
                        if(!AbTargetPath(pGame, TgtId, P, TgtPath))
                                TgtId = -1;
                }
                else
                        AbInitPath(pTgt->m_Pos, pTgt->Core()->m_Vel, TgtPath);
        }
        m_TargetId = TgtId;

        // ---- доступ к настройкам конкретного оружия ----
        auto Wfov = [&](int Weapon) -> int {
                switch(Weapon)
                {
                case WEAPON_HAMMER: return g_Config.m_ClVertexAimbotHammerFov;
                case WEAPON_GUN: return g_Config.m_VxAimBotGunFov;
                case WEAPON_SHOTGUN: return g_Config.m_ClVertexAimbotShotgunFov;
                case WEAPON_GRENADE: return g_Config.m_ClVertexAimbotGrenadeFov;
                case WEAPON_LASER: return g_Config.m_ClVertexAimbotLaserFov;
                default: return g_Config.m_ClHookAimFov; // hook
                }
        };
        auto Wrange = [&](int Weapon) -> float {
                switch(Weapon)
                {
                case WEAPON_HAMMER: return (float)g_Config.m_VxAimHammerRange;
                case WEAPON_GUN: return (float)g_Config.m_VxAimBotGunRange;
                case WEAPON_SHOTGUN: return (float)g_Config.m_VxAimShotgunRange;
                case WEAPON_GRENADE: return (float)g_Config.m_VxAimGrenadeRange;
                case WEAPON_LASER: return (float)g_Config.m_VxAimLaserRange;
                default: return (float)g_Config.m_VxAimHookRange; // hook
                }
        };
        auto Wpredict = [&](int Weapon) -> bool {
                switch(Weapon)
                {
                case WEAPON_HAMMER: return g_Config.m_VxAimHammerPredict != 0;
                case WEAPON_GUN: return g_Config.m_VxAimBotGunPredict != 0;
                case WEAPON_SHOTGUN: return g_Config.m_VxAimShotgunPredict != 0;
                case WEAPON_GRENADE: return g_Config.m_VxAimGrenadePredict != 0;
                case WEAPON_LASER: return g_Config.m_VxAimLaserPredict != 0;
                default: return g_Config.m_VxAimHookPredict != 0; // hook
                }
        };
        auto Wenabled = [&](int Weapon) -> bool {
                switch(Weapon)
                {
                case WEAPON_HAMMER: return g_Config.m_ClVertexAimbotHammerAim != 0;
                case WEAPON_GUN: return g_Config.m_VxAimBotGunAim != 0;
                case WEAPON_SHOTGUN: return g_Config.m_ClVertexAimbotShotgunAim != 0;
                case WEAPON_GRENADE: return g_Config.m_ClVertexAimbotGrenadeAim != 0;
                case WEAPON_LASER: return g_Config.m_ClVertexAimbotLaserAim != 0;
                default: return g_Config.m_ClHookAim != 0; // hook
                }
        };

        // ---- решение одного оружия (можно перерешить с захватом траектории) ----
        SAbPath StaticPath;
        auto Solve = [&](int Weapon, SAbShot &Out, SAbTraj *pTraj) {
                if(TgtId < 0)
                        return; // цели нет — решать нечего (точки карты обрабатываются отдельно)
                const SAbPath *pPath = &TgtPath;
                if(!Wpredict(Weapon))
                {
                        // предикт выключен: цель стоит на предсказанной (текущей) позиции
                        AbInitPath(TgtPath.m_aP[0], TgtPath.m_aV[0], StaticPath);
                        pPath = &StaticPath;
                }
                const SAbFov F = AbFov(Wfov(Weapon), CursorVec);
                const float Range = Wrange(Weapon);
                switch(Weapon)
                {
                case WEAPON_HAMMER:
                        AbHammerShot(MyPos, *pPath, CursorAngle, F.m_Rad, F.m_Use, Out);
                        break;
                case WEAPON_GUN:
                case WEAPON_SHOTGUN:
                case WEAPON_GRENADE:
                {
                        SAbBallist W;
                        if(!AbBallistic(pGame, pCol, MyPos, Weapon, W))
                                return;
                        const int MaxFlight = std::clamp((int)(Range / std::max(1.0f, W.m_Speed / 50.0f)), 4, std::min(45, W.m_Lifetime));
                        const int Steps = 24;
                        const float Span = F.m_Use ? F.m_Rad : AB_PI;
                        for(int k = 0; k < Steps && !OverBudget(); k++)
                        {
                                const float Off = ((k + 0.5f) / Steps - 0.5f) * 2.0f * Span;
                                const vec2 Dir(std::cos(CursorAngle + Off), std::sin(CursorAngle + Off));
                                const vec2 Spawn = MyPos + normalize(Dir) * AB_PHYS; // снаряд рождается в 28px от центра
                                AbSimShot(pCol, Spawn, Dir, W, *pPath, MaxFlight, CursorAngle, F.m_Rad, F.m_Use, Weapon == WEAPON_GRENADE, pTraj, Out);
                        }
                        break;
                }
                case WEAPON_LASER:
                {
                        const int Bounces = std::clamp((int)std::round(AbTuning(pGame, pCol, MyPos)->m_LaserBounceNum), 0, 4);
                        const int Steps = 16;
                        const float Span = F.m_Use ? F.m_Rad : AB_PI;
                        for(int k = 0; k < Steps && !OverBudget(); k++)
                        {
                                const float Off = ((k + 0.5f) / Steps - 0.5f) * 2.0f * Span;
                                const vec2 Dir(std::cos(CursorAngle + Off), std::sin(CursorAngle + Off));
                                const vec2 Spawn = MyPos + normalize(Dir) * AB_PHYS;
                                AbLaserShot(pCol, Spawn, Dir, *pPath, std::min(Range, 1400.0f), Bounces, CursorAngle, F.m_Rad, F.m_Use, pTraj, Out);
                        }
                        break;
                }
                default: // hook
                        AbHookShot(pCol, MyPos, *pPath, Wpredict(Weapon) ? g_Config.m_VxAimBotPredTicks : 1,
                                g_Config.m_VxAimHookEdge != 0, AbTuning(pGame, pCol, MyPos)->m_HookLength, Range, CursorAngle, F.m_Rad, F.m_Use, Out);
                        break;
                }
        };

        // ---- перебор включённого оружия ----
        SAbShot Best;
        int BestWeapon = -100;
        static const int s_aWeapons[6] = {WEAPON_HAMMER, WEAPON_GUN, WEAPON_SHOTGUN, WEAPON_GRENADE, WEAPON_LASER, AB_HOOK};
        for(int w = 0; w < 6 && !OverBudget(); w++)
        {
                const int W = s_aWeapons[w];
                if(!Wenabled(W))
                        continue;
                if(W == AB_HOOK && (!WantHook || HookMapMode))
                        continue;
                SAbShot S;
                Solve(W, S, nullptr);
                if(!S.m_Have)
                        continue;
                float Sc = S.m_Score;
                if(W == ActiveWeapon)
                        Sc += 0.06f; // не переключать оружие без необходимости
                if(!Best.m_Have || Sc > Best.m_Score)
                {
                        Best = S;
                        Best.m_Score = Sc;
                        BestWeapon = W;
                }
        }

        // ---- режим крюка "точки карты" ----
        vec2 MapPoint(0.0f, 0.0f);
        bool HaveMap = false;
        if(HookMapMode && HookAimOn && HookPressed)
        {
                AbHookMap(pCol, MyPos, CursorVec, (float)std::clamp(g_Config.m_VxAimHookRange, 20, 1500), MapPoint);
                HaveMap = true;
        }

        m_TrajNum = 0;
        m_AimWeapon = -1;
        m_HitChance = 0.0f;
        vec2 AimDir = CursorVec;
        if(length(AimDir) < 1.0f)
                AimDir = vec2(1.0f, 0.0f);
        bool HaveAim = false;

        if(Best.m_Have)
        {
                SAbTraj Traj;
                SAbShot S2;
                Solve(BestWeapon, S2, &Traj); // повторно — забираем траекторию для визуала
                AimDir = vec2(std::cos(S2.m_Angle), std::sin(S2.m_Angle));
                m_AimWeapon = BestWeapon;
                m_HitChance = std::clamp(Best.m_Score, 0.0f, 1.0f);
                m_AimPoint = S2.m_Point;
                m_PredPoint = S2.m_Point;
                m_TargetDist = distance(S2.m_Point, MyPos);
                m_TargetSpeed = length(TgtPath.m_aV[std::min(Best.m_Flight, TgtPath.m_Len)]);
                if(g_Config.m_VxAimBotVisGren || g_Config.m_VxAimBotVisLaser || g_Config.m_VxAimBotVisHook)
                {
                        m_TrajNum = std::min(Traj.m_Num, AB_TRAJ_MAX);
                        for(int i = 0; i < m_TrajNum; i++)
                                m_aTraj[i] = Traj.m_aP[i];
                }
                HaveAim = true;
        }
        else if(HaveMap)
        {
                AimDir = normalize(MapPoint - MyPos);
                m_AimWeapon = AB_HOOKMAP;
                m_HitChance = 1.0f;
                m_AimPoint = MapPoint;
                m_PredPoint = MapPoint;
                m_TargetDist = distance(MapPoint, MyPos);
                m_TargetSpeed = 0.0f;
                HaveAim = true;
        }

        if(!HaveAim)
        {
                Deactivate(true);
                return;
        }

        // ---- пишем прицел по режиму ----
        const int Mode = std::clamp(g_Config.m_VxAimBotMode, 0, 3);
        const float MouseMax = pControls->GetMaxMouseDistance();
        if(Mode == 2) // Smooth: плавный доворот видимого курсора
        {
                vec2 &Mouse = pControls->m_aMousePos[Own];
                const float CurLen = std::max(1.0f, length(Mouse));
                const float CurAng = length(Mouse) > 1.0f ? std::atan2(Mouse.y, Mouse.x) : CursorAngle;
                const float AimAng = std::atan2(AimDir.y, AimDir.x);
                const float K = std::clamp(g_Config.m_VxAimBotSmooth, 1, 20) / 20.0f;
                float Step = AbAngleDelta(CurAng, AimAng) * K;
                if(std::fabs(AbAngleDelta(CurAng, AimAng)) < 0.01f)
                        Step = AbAngleDelta(CurAng, AimAng); // доснять остаток
                const float NewAng = CurAng + Step;
                Mouse = vec2(std::cos(NewAng), std::sin(NewAng)) * CurLen;
                pControls->ClampMousePos();
                pSlot->m_TargetX = (int)Mouse.x; // тот же каст, что в SnapInput
                pSlot->m_TargetY = (int)Mouse.y;
        }
        else if(Mode == 0 || Mode == 1) // Silent / Precise: точный вектор
        {
                const vec2 AimVec = AimDir * std::min(AB_AIM_LEN, MouseMax);
                pSlot->m_TargetX = (int)std::round(AimVec.x);
                pSlot->m_TargetY = (int)std::round(AimVec.y);
                if(Mode == 1) // Precise: курсор тоже подводим (видимый аим)
                {
                        pControls->m_aMousePos[Own] = AimVec;
                        pControls->ClampMousePos();
                }
        }
        // Training (3): ввод не трогаем — только визуал

        m_Aiming = Mode != 3;
        m_VisUntil = NowTick + 4;
        m_AimFrom = MyPos;
        m_RangeCirclePos = MyPos;
        m_RangeCircleR = g_Config.m_VxAimBotRadius > 0 ? (float)g_Config.m_VxAimBotRadius : std::max(150.0f, std::min(600.0f, Wrange(m_AimWeapon >= 0 && m_AimWeapon < 100 ? m_AimWeapon : ActiveWeapon)));

        // ---- авто-смена оружия и авто-огонь (по умолчанию выключены) ----
        if(!Training && m_AimWeapon >= 0)
        {
                if(g_Config.m_VxAimBotAutoWeapon && m_AimWeapon <= WEAPON_LASER &&
                        m_AimWeapon != ActiveWeapon && pMe->m_ReloadTimer == 0 && m_HitChance >= 0.6f)
                        pSlot->m_WantedWeapon = m_AimWeapon + 1;
                if(g_Config.m_VxAimBotFire && m_AimWeapon == ActiveWeapon &&
                        m_HitChance >= 0.55f && pMe->m_ReloadTimer <= 1)
                        pSlot->m_Fire = (pSlot->m_Fire + 1) & INPUT_STATE_MASK; // чистое нажатие каждые 2 тика
                if(m_AimWeapon == AB_HOOK && HookAuto && !HookPressed && m_HitChance >= 0.7f)
                        pSlot->m_Hook = 1;
        }

        if(Debug && NowTick - m_LogGateTick >= 50)
        {
                m_LogGateTick = NowTick;
                dbg_msg("vertex-aim", "state: tgt=%d w=%d chance=%.2f mode=%d pred=%d",
                        m_TargetId, m_AimWeapon, (double)m_HitChance, g_Config.m_VxAimBotMode, g_Config.m_VxAimBotPredTicks);
        }
}
// ============================================================================
// клавиши (раз в кадр из CControls::OnRender)
// ============================================================================
void CAimBot::RenderBinds(CGameClient *pGame)
{
        IInput *pInput = pGame->Input();
        const int64_t Now = time_get();
        // захват клавиши для бинда, запущенного из меню
        if(m_BindMode != BIND_NONE)
        {
                if(Now < m_BindBlockUntil)
                        return;
                for(int Key = 1; Key <= KEY_MOUSE_9; Key++)
                {
                        if(Key == KEY_MOUSE_1 || !pInput->KeyPress(Key))
                                continue;
                        int *pTarget = m_BindMode == BIND_HOLD ? &g_Config.m_VxAimBotHoldBind : &g_Config.m_VxAimBotLockBind;
                        *pTarget = Key == KEY_ESCAPE ? -1 : Key;
                        m_BindMode = BIND_NONE;
                        m_BindBlockUntil = Now + time_freq() / 3;
                        break;
                }
                return;
        }
        // экшн-клавиши не работают в чате / меню
        if(pGame->m_Chat.IsActive() || pGame->m_Menus.IsActive())
                return;
        if(g_Config.m_VxAimBotLockBind > 0 && pInput->KeyPress(g_Config.m_VxAimBotLockBind))
        {
                m_LockId = m_LockId >= 0 ? -1 : m_TargetId; // зафиксировать текущую цель / отпустить
                pGame->Echo(m_LockId >= 0 ? "Vertex aim: target locked" : "Vertex aim: lock released");
        }
}

// ============================================================================
// мировой оверлей (из players.cpp, рядом с остальным Vertex ESP):
// FOV-круг, линия прицела, предикт-точка, цель, траектории, шанс попадания
// ============================================================================
static void AbDrawCircle(IGraphics *pGraphics, const vec2 &C, float R, int Segments)
{
        float Px = C.x + R, Py = C.y;
        for(int i = 1; i <= Segments; i++)
        {
                const float a = (float)i / (float)Segments * 2.0f * AB_PI;
                const float Nx = C.x + std::cos(a) * R, Ny = C.y + std::sin(a) * R;
                const IGraphics::CLineItem L(Px, Py, Nx, Ny);
                pGraphics->LinesDraw(&L, 1);
                Px = Nx;
                Py = Ny;
        }
}
static void AbDrawCross(IGraphics *pGraphics, const vec2 &P, float R)
{
        const IGraphics::CLineItem aL[2] = {
                IGraphics::CLineItem(P.x - R, P.y, P.x + R, P.y),
                IGraphics::CLineItem(P.x, P.y - R, P.x, P.y + R)};
        pGraphics->LinesDraw(aL, 2);
}
static ColorRGBA AbColor(CGameClient *pGame, unsigned int Packed, float Alpha)
{
        ColorRGBA C = color_cast<ColorRGBA>(ColorHSLA(Packed));
        C.a = Alpha;
        return C;
}

void CAimBot::RenderWorld(CGameClient *pGame)
{
        if(g_Config.m_VxAimBotEnable == 0)
                return;
        const int NowTick = pGame->m_PredictedWorld.GameTick();
        IGraphics *pGraphics = pGame->Graphics();

        // ---- FOV-круг (настройки старого ESP переиспользуются) ----
        if(g_Config.m_ClVertexAimbotEnableESP && g_Config.m_ClVertexAimbotShowFov && m_RangeCircleR > 1.0f)
        {
                pGraphics->TextureClear();
                pGraphics->LinesBegin();
                pGraphics->SetColor(AbColor(pGame, g_Config.m_VxEspFovColor, 0.35f));
                AbDrawCircle(pGraphics, m_RangeCirclePos, m_RangeCircleR, 48);
                pGraphics->LinesEnd();
        }

        pGraphics->TextureClear();
        pGraphics->LinesBegin();

        // ---- траектория победившего оружия ----
        const bool WantTraj = (m_AimWeapon == WEAPON_GRENADE && g_Config.m_VxAimBotVisGren) ||
                              (m_AimWeapon == WEAPON_LASER && g_Config.m_VxAimBotVisLaser) ||
                              (m_AimWeapon >= 0 && g_Config.m_VxAimBotVisHook && m_AimWeapon != WEAPON_GRENADE && m_AimWeapon != WEAPON_LASER);
        if(WantTraj && m_VisUntil >= NowTick && m_TrajNum > 1)
        {
                pGraphics->SetColor(AbColor(pGame, g_Config.m_VxAimBotTrajColor, 0.55f));
                for(int i = 0; i + 1 < m_TrajNum; i++)
                {
                        const IGraphics::CLineItem L(m_aTraj[i].x, m_aTraj[i].y, m_aTraj[i + 1].x, m_aTraj[i + 1].y);
                        pGraphics->LinesDraw(&L, 1);
                }
        }

        // ---- линия прицела ----
        if(m_VisUntil >= NowTick && m_AimWeapon >= 0)
        {
                pGraphics->SetColor(AbColor(pGame, g_Config.m_VxAimBotTrajColor, 0.65f));
                const IGraphics::CLineItem L(m_AimFrom.x, m_AimFrom.y, m_AimPoint.x, m_AimPoint.y);
                pGraphics->LinesDraw(&L, 1);
                // ---- точка попадания / предикт ----
                if(g_Config.m_VxAimBotVisPred)
                {
                        pGraphics->SetColor(1.0f, 0.9f, 0.3f, 0.85f);
                        AbDrawCross(pGraphics, m_PredPoint, 5.0f);
                }
                // ---- цель: крест + кольцо шанса попадания ----
                if(m_TargetId >= 0)
                {
                        CCharacter *pT = pGame->m_PredictedWorld.GetCharacterById(m_TargetId);
                        if(pT)
                        {
                                if(m_HitChance >= 0.7f)
                                        pGraphics->SetColor(0.4f, 1.0f, 0.5f, 0.85f);
                                else if(m_HitChance >= 0.4f)
                                        pGraphics->SetColor(1.0f, 0.9f, 0.3f, 0.85f);
                                else
                                        pGraphics->SetColor(1.0f, 0.35f, 0.35f, 0.85f);
                                AbDrawCross(pGraphics, pT->m_Pos, 8.0f);
                                AbDrawCircle(pGraphics, pT->m_Pos, 16.0f * m_HitChance + 4.0f, 20);
                        }
                }
        }
        pGraphics->LinesEnd();

        // ---- информация о цели: дистанция, скорость, шанс ----
        if(g_Config.m_VxAimBotVisInfo && m_TargetId >= 0 && m_VisUntil >= NowTick)
        {
                CCharacter *pT = pGame->m_PredictedWorld.GetCharacterById(m_TargetId);
                if(pT)
                {
                        char aBuf[96];
                        str_format(aBuf, sizeof(aBuf), "%.0fpx  %.1fp/t  %d%%",
                                (double)m_TargetDist, (double)m_TargetSpeed, (int)std::round(m_HitChance * 100.0f));
                        pGame->TextRender()->TextColor(0.9f, 0.9f, 1.0f, 0.8f);
                        pGame->TextRender()->Text(pT->m_Pos.x, pT->m_Pos.y - 26.0f, 10.0f, aBuf, -1.0f);
                        pGame->TextRender()->TextColor(pGame->TextRender()->DefaultTextColor());
                }
        }
}

// ============================================================================
// полный сброс (реконнект / смена карты)
// ============================================================================
void CAimBot::Reset()
{
        m_Aiming = false;
        m_TargetId = -1;
        m_LockId = -1;
        m_AimWeapon = -1;
        m_HitChance = 0.0f;
        m_TrajNum = 0;
        m_VisUntil = -1000000;
        m_LastWorkTick = -1000000;
        m_BindMode = BIND_NONE;
        m_BindBlockUntil = 0;
        m_LogGateTick = -1000000;
}
