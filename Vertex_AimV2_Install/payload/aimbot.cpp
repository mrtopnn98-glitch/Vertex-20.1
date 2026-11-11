/* Vertex client — Vertex Aim System v2. Документация модуля — aimbot.h. */
#include "aimbot.h"

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
// константы (взяты из реальной физики: prediction/entities/character.cpp,
// projectile.cpp, laser.cpp)
// ============================================================================
static constexpr float AB_PI = 3.14159265f;
static constexpr float AB_SPAWN = 21.0f; // снаряд рождается в PhysSize*0.75 от центра
static constexpr float AB_HAMMER_HIT = 42.0f; // dist(цель, точка удара) < 14+28 (FireWeapon)
static constexpr float AB_HAMMER_REACH = 63.0f; // 21 + 42 — максимум от центра до центра
static constexpr float AB_GRENADE_BOOM = 100.0f; // взрыв гранаты достаёт цель у стены
static constexpr int AB_CAND_MAX = 8; // сколько лучших кандидатов собираем
static constexpr int AB_TRAJ_MAX = 160;

// ============================================================================
// контекст тика и кандидат в цели
// ============================================================================
struct SAbCtx
{
        CGameClient *m_pGame = nullptr;
        CCollision *m_pCol = nullptr;
        CCharacter *m_pMe = nullptr;
        int m_LocalId = -1;
        int m_DummyId = -1;
        vec2 m_MyPos = vec2(0.0f, 0.0f);
        vec2 m_MouseDir = vec2(1.0f, 0.0f);
        float m_CursorAng = 0.0f;
        const CTuningParams *m_pTun = nullptr;
        bool m_Vanilla = false;
};
struct SAbCand
{
        int m_Id = -1;
        vec2 m_Pos = vec2(0.0f, 0.0f); // текущая позиция
        float m_Dev = 0.0f; // отклонение от прицела (рад)
        float m_Dist = 0.0f;
};

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

// радиус зоны по FOV: 360° -> 1500px (~47 тайлов), 180° -> 850, 90° -> 525
static float AbZoneR(int FovDeg)
{
        return 200.0f + 1300.0f * (std::clamp(FovDeg, 1, 360) / 360.0f);
}

// упреждение: позиция цели через Ticks тиков (линейно по её скорости)
static vec2 AbLead(CCharacter *pT, float Ticks)
{
        return pT->m_Pos + pT->Core()->m_Vel * std::clamp(Ticks, 0.0f, 20.0f);
}

// ============================================================================
// выбор кандидатов: угол от прицела <= FOV/2, дистанция <= радиуса зоны.
// Никакой отдельной "дальности" — радиус зоны растёт вместе с FOV.
// ============================================================================
static bool AbPickTargets(const SAbCtx &Ctx, float FovDeg, float ZoneR, SAbCand *pOut, int Max, int &Num)
{
        Num = 0;
        CGameClient *pGame = Ctx.m_pGame;
        const bool FullCircle = FovDeg >= 360;
        const float FovRad = std::clamp(FovDeg, 1, 360) * AB_PI / 360.0f;
        for(int i = 0; i < MAX_CLIENTS; i++)
        {
                if(i == Ctx.m_LocalId)
                        continue;
                CCharacter *pT = pGame->m_PredictedWorld.GetCharacterById(i);
                if(!pT)
                        continue;
                if(i == Ctx.m_DummyId && g_Config.m_VxAimNoDummy != 0)
                        continue;
                if(g_Config.m_VxAimNoFriends != 0 && pGame->m_aClients[i].m_Friend)
                        continue;
                if(g_Config.m_VxAimNoTeam != 0 && Ctx.m_pMe->SameTeam(i))
                        continue;
                if(g_Config.m_VxAimNoFrozen != 0 && pT->m_FreezeTime > 0)
                        continue;
                if(!pT->CanCollide(Ctx.m_pMe->GetCid()))
                        continue; // solo / неколлайдерный — крюк всё равно не возьмёт
                const vec2 Dv = pT->m_Pos - Ctx.m_MyPos;
                const float Dist = length(Dv);
                if(Dist > ZoneR || Dist < 1.0f)
                        continue;
                const float Dev = AbAngleDev(std::atan2(Dv.y, Dv.x), Ctx.m_CursorAng);
                if(!FullCircle && Dev > FovRad)
                        continue;
                // лучший = ближе к центру прицела, при равенстве — ближе телом
                const float Score = Dev + Dist * 0.0002f;
                int Slot = Num < Max ? Num : Max - 1;
                if(Num == Max && Score >= pOut[Slot].m_Dev + pOut[Slot].m_Dist * 0.0002f)
                        continue;
                while(Slot > 0 && Score < pOut[Slot - 1].m_Dev + pOut[Slot - 1].m_Dist * 0.0002f)
                {
                        pOut[Slot] = pOut[Slot - 1];
                        Slot--;
                }
                pOut[Slot].m_Id = i;
                pOut[Slot].m_Pos = pT->m_Pos;
                pOut[Slot].m_Dev = Dev;
                pOut[Slot].m_Dist = Dist;
                if(Num < Max)
                        Num++;
        }
        return Num > 0;
}

// ============================================================================
// ХУК: цель + чистый путь. Первый кандидат (по прицелу) с незагороженным
// путём побеждает. Глухие цели не берём — крюк в стену не подставляем.
// ============================================================================
static bool AbHookSolve(const SAbCtx *pCtx, const SAbCand *pCands, int Num, bool Pred, vec2 &OutDir, vec2 &OutPoint, int &OutId)
{
        const float HookLen = pCtx->m_pTun->m_HookLength;
        for(int k = 0; k < Num; k++)
        {
                CCharacter *pT = pCtx->m_pGame->m_PredictedWorld.GetCharacterById(pCands[k].m_Id);
                if(!pT)
                        continue;
                float Ticks = 1.0f;
                if(Pred)
                {
                        // крюк летит HookFireSpeed px/тик — упреждаем на время полёта
                        Ticks = pCands[k].m_Dist / std::max(1.0f, pCtx->m_pTun->m_HookFireSpeed) + 1.0f;
                        Ticks = std::clamp(Ticks, 1.0f, 8.0f);
                }
                const vec2 P = AbLead(pT, Ticks);
                if(distance(P, pCtx->m_MyPos) > HookLen + 30.0f)
                        continue;
                vec2 Dir = P - pCtx->m_MyPos;
                if(length(Dir) < 1.0f)
                        continue;
                Dir = normalize(Dir);
                vec2 Hit, Before;
                if(pCtx->m_pCol->IntersectLineTeleHook(pCtx->m_MyPos, P, &Hit, &Before))
                {
                        if(distance(pCtx->m_MyPos, Hit) < distance(pCtx->m_MyPos, P) - 26.0f)
                                continue; // стена между нами и целью
                }
                OutDir = Dir;
                OutPoint = P;
                OutId = pCands[k].m_Id;
                return true;
        }
        return false;
}

// ============================================================================
// МОЛОТ: бьёт только то, что реально достаёт (дистанция центра до центра
// <= 63px — это физика FireWeapon, не настройка). 360° FOV = бьёт спиной.
// ============================================================================
static bool AbHammerSolve(const SAbCtx *pCtx, const SAbCand *pCands, int Num, vec2 &OutDir, vec2 &OutPoint)
{
        for(int k = 0; k < Num; k++)
        {
                CCharacter *pT = pCtx->m_pGame->m_PredictedWorld.GetCharacterById(pCands[k].m_Id);
                if(!pT)
                        continue;
                const vec2 P = AbLead(pT, 1.0f); // удар придёт на следующий тик
                vec2 Dir = P - pCtx->m_MyPos;
                const float Len = length(Dir);
                if(Len < 1.0f || Len > AB_HAMMER_REACH)
                        continue;
                Dir = normalize(Dir);
                const vec2 ProjStart = pCtx->m_MyPos + Dir * AB_SPAWN;
                if(distance(P, ProjStart) > AB_HAMMER_HIT)
                        continue;
                OutDir = Dir;
                OutPoint = P;
                return true;
        }
        return false;
}

// ============================================================================
// баллистика: пистолет / граната / vanilla-дробаш.
// Точный расчёт дуги через формулу CalcPos (та же, что у игры):
//   dir * (Speed*t) + (0, Curvature/10000 * (Speed*t)^2) = Rel
// ============================================================================
struct SAbProj
{
        float m_Curv = 0.0f;
        float m_Speed = 0.0f;
        int m_Lifetime = 40;
        bool m_Explosive = false;
};
static bool AbProjParams(const SAbCtx *pCtx, int Weapon, SAbProj &W)
{
        const CTuningParams *pT = pCtx->m_pTun;
        if(Weapon == WEAPON_GUN)
        {
                W.m_Curv = pT->m_GunCurvature;
                W.m_Speed = pT->m_GunSpeed;
                W.m_Lifetime = std::clamp((int)(pT->m_GunLifetime * 50.0f), 5, 100);
        }
        else if(Weapon == WEAPON_GRENADE)
        {
                W.m_Curv = pT->m_GrenadeCurvature;
                W.m_Speed = pT->m_GrenadeSpeed;
                W.m_Lifetime = std::clamp((int)(pT->m_GrenadeLifetime * 50.0f), 5, 100);
                W.m_Explosive = true;
        }
        else if(Weapon == WEAPON_SHOTGUN) // vanilla: обычные пули с кривизной
        {
                W.m_Curv = pT->m_ShotgunCurvature;
                W.m_Speed = pT->m_ShotgunSpeed;
                W.m_Lifetime = std::clamp((int)(pT->m_ShotgunLifetime * 50.0f), 5, 20);
        }
        else
                return false;
        return W.m_Speed > 1.0f;
}
// число тиков полёта до цели
static float AbFlightTicks(const SAbProj &W, float Dist) { return Dist / W.m_Speed * 50.0f; }

static bool AbBallisticSolve(const SAbCtx *pCtx, const SAbCand *pCands, int Num, const SAbProj &W, vec2 &OutDir, vec2 &OutPoint)
{
        for(int k = 0; k < Num; k++)
        {
                CCharacter *pT = pCtx->m_pGame->m_PredictedWorld.GetCharacterById(pCands[k].m_Id);
                if(!pT)
                        continue;
                vec2 Dir = normalize(pT->m_Pos - pCtx->m_MyPos);
                vec2 Pred = pT->m_Pos;
                bool Solved = false;
                for(int it = 0; it < 3; it++)
                {
                        const float T = distance(pT->m_Pos, pCtx->m_MyPos) / W.m_Speed;
                        Pred = AbLead(pT, T * 50.0f + 1.0f); // цель в момент прилёта
                        const vec2 Spawn = pCtx->m_MyPos + Dir * AB_SPAWN;
                        const vec2 Rel = Pred - Spawn;
                        // дуга: U = |(Rel.x, Rel.y - K*U^2)| — итерации до сходимости
                        float U = length(Rel);
                        for(int j = 0; j < 6; j++)
                                U = length(vec2(Rel.x, Rel.y - W.m_Curv / 10000.0f * U * U));
                        const vec2 Need = vec2(Rel.x, Rel.y - W.m_Curv / 10000.0f * U * U);
                        if(U < 8.0f || length(Need) < 0.001f)
                                break;
                        Dir = normalize(Need);
                        Solved = true;
                }
                if(!Solved)
                        continue;
                // проверяем полёт: пуля у стены умирает; граната взрывается — стена
                // допустима, если цель рядом со взрывом
                const int MaxFlight = std::clamp((int)std::round(AbFlightTicks(W, distance(Pred, pCtx->m_MyPos))) + 2, 2, W.m_Lifetime);
                const vec2 Spawn = pCtx->m_MyPos + Dir * AB_SPAWN;
                vec2 Prev = Spawn;
                bool Clear = false;
                for(int i = 1; i <= MaxFlight; i++)
                {
                        const vec2 Cur = CalcPos(Spawn, Dir, W.m_Curv, W.m_Speed, i / 50.0f);
                        vec2 ColPos, Before;
                        if(pCtx->m_pCol->IntersectLine(Prev, Cur, &ColPos, &Before))
                        {
                                if(W.m_Explosive && distance(ColPos, Pred) <= AB_GRENADE_BOOM)
                                {
                                        OutDir = Dir;
                                        OutPoint = ColPos; // взрыв у стены достаёт цель
                                        return true;
                                }
                                break; // пуля в стену — цель не берём
                        }
                        Prev = Cur;
                        if(i == MaxFlight)
                                Clear = true;
                }
                if(!Clear)
                        continue;
                OutDir = Dir;
                OutPoint = Pred;
                return true;
        }
        return false;
}

// ============================================================================
// ЛУЧ (лазер / DDRace-дробаш): тот же ход луча, что CLaser::DoBounce —
// сегменты, отражение через MovePoint, трата энергии на сегмент+отскок.
// ============================================================================
struct SAbSeg
{
        vec2 m_a, m_b;
};
struct SAbBeam
{
        SAbSeg m_aSeg[12];
        int m_Num = 0;
        bool m_Blocked = false;
};
static void AbBeamSim(const SAbCtx *pCtx, const vec2 &From, vec2 Dir, float Energy, float Cost, int MaxBounce, SAbBeam &Out)
{
        Dir = normalize(Dir);
        vec2 Pos = From;
        int Bounce = 0;
        Out.m_Num = 0;
        Out.m_Blocked = false;
        while(Bounce <= MaxBounce && Out.m_Num < 12 && Energy > 8.0f)
        {
                const vec2 To = Pos + Dir * Energy;
                vec2 ColPos, Before;
                const int Hit = pCtx->m_pCol->IntersectLineTeleWeapon(Pos, To, &ColPos, &Before);
                const vec2 SegEnd = Hit ? Before : To;
                Out.m_aSeg[Out.m_Num].m_a = Pos;
                Out.m_aSeg[Out.m_Num].m_b = SegEnd;
                Out.m_Num++;
                if(!Hit)
                        break;
                Out.m_Blocked = true;
                // отражение ровно как в CLaser::DoBounce
                vec2 TmpPos = SegEnd;
                vec2 TmpDir = Dir * 4.0f;
                pCtx->m_pCol->MovePoint(&TmpPos, &TmpDir, 1.0f, nullptr);
                const float SegLen = distance(Pos, SegEnd);
                Energy -= SegLen + Cost;
                Pos = TmpPos;
                Dir = normalize(TmpDir);
                Bounce++;
        }
}
// ближайшее расстояние от сегмента до точки + сама точка
static float AbSegDist(const vec2 &A, const vec2 &B, const vec2 &P, vec2 &OutClosest)
{
        if(!closest_point_on_line(A, B, P, OutClosest))
                OutClosest = A;
        return distance(P, OutClosest);
}

// прямой луч + (опционально) поиск направления с отскоком, чей луч достаёт цель
static bool AbBeamSolve(const SAbCtx *pCtx, const SAbCand *pCands, int Num, int BounceFovDeg, bool BounceAim,
        vec2 &OutDir, vec2 &OutPoint, int &OutId, int &OutBounce)
{
        const float Reach = pCtx->m_pTun->m_LaserReach;
        const float Cost = pCtx->m_pTun->m_LaserBounceCost;
        const int MaxBounce = std::clamp((int)pCtx->m_pTun->m_LaserBounceNum, 0, 5);
        const int DelayTicks = (int)(50.0f * pCtx->m_pTun->m_LaserBounceDelay / 1000.0f) + 1;

        // 1) прямой выстрел в упреждённую цель
        for(int k = 0; k < Num; k++)
        {
                CCharacter *pT = pCtx->m_pGame->m_PredictedWorld.GetCharacterById(pCands[k].m_Id);
                if(!pT)
                        continue;
                const vec2 Pred = AbLead(pT, 1.0f);
                vec2 Dir = Pred - pCtx->m_MyPos;
                if(length(Dir) < 1.0f)
                        continue;
                Dir = normalize(Dir);
                SAbBeam Beam;
                AbBeamSim(pCtx, pCtx->m_MyPos, Dir, Reach, Cost, 0, Beam);
                if(!Beam.m_Blocked && Beam.m_Num > 0)
                {
                        OutDir = Dir;
                        OutPoint = Pred;
                        OutId = pCands[k].m_Id;
                        OutBounce = 0;
                        return true;
                }
        }
        if(!BounceAim)
                return false;

        // 2) аим на отскок: перебираем направления в FOV отскока и ищем луч,
        //    чей отражённый сегмент проходит через цель (луч по-настоящему
        //    доезжает до сегмента b через b * задержку отскока — упреждаем)
        const bool FullCircle = BounceFovDeg >= 360;
        const float FovRad = std::clamp(BounceFovDeg, 1, 360) * AB_PI / 360.0f;
        const int Steps = 120;
        float BestScore = -1e30f;
        bool Found = false;
        for(int s = 0; s < Steps; s++)
        {
                float Off;
                if(FullCircle)
                        Off = (float)s / Steps * 2.0f * AB_PI;
                else
                        Off = ((s + 0.5f) / Steps - 0.5f) * 2.0f * FovRad;
                const float Ang = pCtx->m_CursorAng + Off;
                const vec2 Dir(std::cos(Ang), std::sin(Ang));
                SAbBeam Beam;
                AbBeamSim(pCtx, pCtx->m_MyPos, Dir, Reach, Cost, MaxBounce, Beam);
                for(int b = 0; b < Beam.m_Num; b++)
                {
                        for(int k = 0; k < Num; k++)
                        {
                                CCharacter *pT = pCtx->m_pGame->m_PredictedWorld.GetCharacterById(pCands[k].m_Id);
                                if(!pT)
                                        continue;
                                const vec2 Pred = AbLead(pT, (float)(b * DelayTicks) + 1.0f);
                                vec2 Closest;
                                const float D = AbSegDist(Beam.m_aSeg[b].m_a, Beam.m_aSeg[b].m_b, Pred, Closest);
                                if(D > AB_BEAM_HIT)
                                        continue;
                                const float Dev = AbAngleDev(Ang, pCtx->m_CursorAng);
                                const float Score = 1000.0f - (float)b * 60.0f - Dev * 40.0f - D;
                                if(Score > BestScore)
                                {
                                        BestScore = Score;
                                        OutDir = Dir;
                                        OutPoint = Pred;
                                        OutId = pCands[k].m_Id;
                                        OutBounce = b;
                                        Found = true;
                                }
                        }
                }
        }
        return Found;
}

// ============================================================================
// симуляция траектории от прицела (ESP): граната / vanilla-дробаш
// ============================================================================
static void AbSimTrajProj(const SAbCtx *pCtx, const vec2 &From, const vec2 &Dir, const SAbProj &W, vec2 *pPts, int MaxPts, int &Num, vec2 &Impact)
{
        Num = 0;
        vec2 Prev = From;
        if(Num < MaxPts)
                pPts[Num++] = From;
        for(int i = 1; i <= W.m_Lifetime; i++)
        {
                const vec2 Cur = CalcPos(From, normalize(Dir), W.m_Curv, W.m_Speed, i / 50.0f);
                vec2 ColPos, Before;
                if(pCtx->m_pCol->IntersectLine(Prev, Cur, &ColPos, &Before))
                {
                        if(Num < MaxPts)
                                pPts[Num++] = Before;
                        Impact = Before;
                        return;
                }
                if(Num < MaxPts)
                        pPts[Num++] = Cur;
                Prev = Cur;
                Impact = Cur;
        }
}

// ============================================================================
// главный тик
// ============================================================================
void CAimBot::Update(CGameClient *pGame, IClient *pClient, CControls *pControls, CNetObj_PlayerInput *pSlot)
{
        const bool Debug = g_Config.m_VxAimDebug != 0;
        const bool Enabled = g_Config.m_VxAimEnable != 0 &&
                             (g_Config.m_VxAimHoldBind <= 0 || pGame->Input()->KeyIsPressed(g_Config.m_VxAimHoldBind));
        const bool WasActive = m_Aiming;

        auto Deactivate = [&]() {
                m_Aiming = false;
                m_AimWeapon = -1;
                m_VisUntil = -1000000;
                if(m_FireOwned)
                {
                        // отпускаем свой авто-огонь, чтобы не остаться с зажатой стрельбой
                        if((pSlot->m_Fire & 1) != 0)
                                pSlot->m_Fire = (pSlot->m_Fire + 1) & INPUT_STATE_MASK;
                        m_FireOwned = false;
                }
                if(m_TargetId != -1)
                {
                        m_TargetId = -1;
                        pControls->m_VertexAimTargetId = -1;
                }
                if(Debug && WasActive)
                        dbg_msg("vertex-aim", "off");
        };

        if(!Enabled || pClient->State() != IClient::STATE_ONLINE ||
                pGame->m_Chat.IsActive() || pGame->m_Menus.IsActive() ||
                pGame->m_AvoidFreeze.IsBusy() || pGame->m_AutoAled.IsBusy() ||
                pControls->m_LaserUnfreezeAimActive)
        {
                Deactivate();
                return;
        }
        const int LocalId = pGame->m_Snap.m_LocalClientId;
        if(LocalId < 0 || pGame->m_Snap.m_SpecInfo.m_Active)
        {
                Deactivate();
                return;
        }
        CCharacter *pMe = pGame->m_PredictedWorld.GetCharacterById(LocalId);
        if(!pMe || pMe->m_FreezeTime > 0 || pMe->Core()->m_DeepFrozen)
        {
                Deactivate();
                return;
        }

        const int NowTick = pClient->GameTick(g_Config.m_ClDummy);
        if(NowTick < m_LastWorkTick - 50) // реконнект / смена карты
                Reset();
        if(NowTick == m_LastWorkTick) // раз в тик
                return;
        m_LastWorkTick = NowTick;

        // ---- контекст ----
        SAbCtx Ctx;
        Ctx.m_pGame = pGame;
        Ctx.m_pCol = pMe->Collision();
        Ctx.m_pMe = pMe;
        Ctx.m_LocalId = LocalId;
        Ctx.m_DummyId = pGame->m_aLocalIds[g_Config.m_ClDummy ^ 1];
        if(Ctx.m_DummyId == LocalId)
                Ctx.m_DummyId = -1;
        Ctx.m_MyPos = pMe->m_Pos;
        Ctx.m_pTun = &pGame->m_aTuning[g_Config.m_ClDummy];
        Ctx.m_Vanilla = pGame->m_PredictedWorld.m_WorldConfig.m_IsVanilla;
        Ctx.m_MouseDir = pControls->m_aMousePos[g_Config.m_ClDummy];
        if(length(Ctx.m_MouseDir) > 1.0f)
                Ctx.m_CursorAng = std::atan2(Ctx.m_MouseDir.y, Ctx.m_MouseDir.x);

        // ---- параметры зон для рендера (рисуются даже без нажатий) ----
        m_ZonePos = Ctx.m_MyPos;
        m_ZoneAng = Ctx.m_CursorAng;
        const int ActiveWeapon = pMe->Core()->m_ActiveWeapon;
        const bool HookOn = g_Config.m_VxAimHook != 0;
        const bool WepOn = ActiveWeapon >= WEAPON_HAMMER && ActiveWeapon <= WEAPON_LASER &&
                           ((ActiveWeapon == WEAPON_HAMMER && g_Config.m_VxAimHammer) ||
                                   (ActiveWeapon == WEAPON_GUN && g_Config.m_VxAimGun) ||
                                   (ActiveWeapon == WEAPON_SHOTGUN && g_Config.m_VxAimShotgun) ||
                                   (ActiveWeapon == WEAPON_GRENADE && g_Config.m_VxAimGrenade) ||
                                   (ActiveWeapon == WEAPON_LASER && g_Config.m_VxAimLaser));
        if(HookOn)
        {
                m_HookFovDeg = g_Config.m_VxAimHookFov;
                m_HookZoneR = std::min(AbZoneR(m_HookFovDeg), Ctx.m_pTun->m_HookLength + 30.0f);
        }
        else
                m_HookFovDeg = 0;
        if(WepOn)
        {
                int Fov = 180;
                switch(ActiveWeapon)
                {
                case WEAPON_HAMMER: Fov = g_Config.m_VxAimHammerFov; break;
                case WEAPON_GUN: Fov = g_Config.m_VxAimGunFov; break;
                case WEAPON_SHOTGUN: Fov = g_Config.m_VxAimShotgunFov; break;
                case WEAPON_GRENADE: Fov = g_Config.m_VxAimGrenadeFov; break;
                case WEAPON_LASER: Fov = g_Config.m_VxAimLaserFov; break;
                }
                m_WepFovDeg = Fov;
                // молот достаёт только на длину руки — зона честная
                m_WepZoneR = ActiveWeapon == WEAPON_HAMMER ? AB_HAMMER_REACH : AbZoneR(Fov);
        }
        else
                m_WepFovDeg = 0;

        // ---- цель в зоне хука: подсветка работает даже без нажатий ----
        SAbCand aHookCands[AB_CAND_MAX];
        int HookNum = 0;
        if(HookOn)
                AbPickTargets(Ctx, g_Config.m_VxAimHookFov, m_HookZoneR, aHookCands, AB_CAND_MAX, HookNum);
        SAbCand aWepCands[AB_CAND_MAX];
        int WepNum = 0;
        if(WepOn)
                AbPickTargets(Ctx, m_WepFovDeg, m_WepZoneR, aWepCands, AB_CAND_MAX, WepNum);

        // подсвечиваем цель хука (она и будет "залочена"), иначе — цель оружия
        m_TargetId = HookNum > 0 ? aHookCands[0].m_Id : (WepNum > 0 ? aWepCands[0].m_Id : -1);
        pControls->m_VertexAimTargetId = m_TargetId; // совместимость со старым ESP

        const bool HookKey = pSlot->m_Hook != 0;
        const bool FireKey = (pSlot->m_Fire & 1) != 0;
        const bool WantHook = HookOn && HookNum > 0 && (HookKey || g_Config.m_VxAimHookAuto != 0);

        // ---- решение хука ----
        vec2 AimDir = Ctx.m_MouseDir;
        bool HaveAim = false;
        int AimWeapon = -1;
        vec2 PredPoint(0.0f, 0.0f);
        vec2 AimPoint(0.0f, 0.0f);
        int AimId = -1;
        if(WantHook)
        {
                vec2 Dir, Point;
                int Id = -1;
                if(AbHookSolve(&Ctx, aHookCands, HookNum, true, Dir, Point, Id))
                {
                        AimDir = Dir;
                        AimPoint = Point;
                        PredPoint = Point;
                        AimId = Id;
                        AimWeapon = AB_HOOK;
                        HaveAim = true;
                        if(!HookKey)
                                pSlot->m_Hook = 1; // авто-крюк
                }
        }

        // ---- решение оружия (сильнее хука: намерение стрелять важнее) ----
        bool WantWeapon = false;
        if(WepOn && WepNum > 0)
        {
                if(ActiveWeapon == WEAPON_HAMMER)
                        WantWeapon = FireKey || g_Config.m_VxAimHammerAuto != 0;
                else
                        WantWeapon = FireKey || g_Config.m_VxAimAutoFire != 0;
        }
        if(WantWeapon)
        {
                vec2 Dir, Point;
                bool Ok = false;
                if(ActiveWeapon == WEAPON_HAMMER)
                        Ok = AbHammerSolve(&Ctx, aWepCands, WepNum, Dir, Point);
                else if(ActiveWeapon == WEAPON_LASER || (ActiveWeapon == WEAPON_SHOTGUN && !Ctx.m_Vanilla))
                {
                        int Bounce = 0;
                        int Id = -1;
                        const bool BounceAim = ActiveWeapon == WEAPON_LASER ? g_Config.m_VxAimLaserBounce != 0 : g_Config.m_VxAimShotgunBounce != 0;
                        const int BounceFov = ActiveWeapon == WEAPON_LASER ? g_Config.m_VxAimLaserBounceFov : g_Config.m_VxAimShotgunBounceFov;
                        Ok = AbBeamSolve(&Ctx, aWepCands, WepNum, BounceFov, BounceAim, Dir, Point, Id, Bounce);
                        if(Ok)
                                AimId = Id;
                }
                else
                {
                        SAbProj W;
                        if(AbProjParams(&Ctx, ActiveWeapon, W))
                                Ok = AbBallisticSolve(&Ctx, aWepCands, WepNum, W, Dir, Point);
                }
                if(Ok)
                {
                        AimDir = Dir;
                        AimPoint = Point;
                        PredPoint = Point;
                        AimWeapon = ActiveWeapon;
                        if(AimId < 0 && WepNum > 0)
                                AimId = aWepCands[0].m_Id;
                        HaveAim = true;
                }
        }

        // ---- пишем прицел (тихо: видимый курсор не двигаем) ----
        if(HaveAim)
        {
                const float MouseMax = pControls->GetMaxMouseDistance();
                const vec2 AimVec = AimDir * std::clamp(MouseMax, 100.0f, 900.0f);
                pSlot->m_TargetX = (int)std::round(AimVec.x);
                pSlot->m_TargetY = (int)std::round(AimVec.y);
                m_Aiming = true;
                m_AimWeapon = AimWeapon;
                m_AimPoint = AimPoint;
                m_PredPoint = PredPoint;
                m_TargetId = AimId;
                pControls->m_VertexAimTargetId = AimId;
                m_VisUntil = NowTick + 5;
                m_From = Ctx.m_MyPos;

                // ---- авто-огонь / авто-удар: держим счётчик огня сами ----
                const bool WantFire = (AimWeapon == ActiveWeapon) &&
                                      ((ActiveWeapon == WEAPON_HAMMER && g_Config.m_VxAimHammerAuto != 0) ||
                                              (ActiveWeapon != WEAPON_HAMMER && g_Config.m_VxAimAutoFire != 0));
                if(WantFire)
                {
                        if((pSlot->m_Fire & 1) == 0)
                        {
                                pSlot->m_Fire = (pSlot->m_Fire + 1) & INPUT_STATE_MASK;
                                m_FireOwned = true;
                        }
                }
                else if(m_FireOwned)
                {
                        if((pSlot->m_Fire & 1) != 0)
                                pSlot->m_Fire = (pSlot->m_Fire + 1) & INPUT_STATE_MASK;
                        m_FireOwned = false;
                }

                if(Debug && NowTick - m_LogGateTick >= 50)
                {
                        m_LogGateTick = NowTick;
                        dbg_msg("vertex-aim", "state: tgt=%d w=%d fov=%d", m_TargetId, m_AimWeapon,
                                AimWeapon == AB_HOOK ? g_Config.m_VxAimHookFov : m_WepFovDeg);
                }
        }
        else
        {
                if(m_FireOwned)
                {
                        if((pSlot->m_Fire & 1) != 0)
                                pSlot->m_Fire = (pSlot->m_Fire + 1) & INPUT_STATE_MASK;
                        m_FireOwned = false;
                }
                m_Aiming = false;
                m_AimWeapon = -1;
                m_VisUntil = -1000000;
        }
}

// ============================================================================
// клавиша "aim при удержании" (раз в кадр из CControls::OnRender)
// ============================================================================
void CAimBot::RenderBinds(CGameClient *pGame)
{
        IInput *pInput = pGame->Input();
        const int64_t Now = time_get();
        if(m_BindMode != BIND_NONE)
        {
                if(Now < m_BindBlockUntil)
                        return;
                for(int Key = 1; Key <= KEY_MOUSE_9; Key++)
                {
                        if(Key == KEY_MOUSE_1 || !pInput->KeyPress(Key))
                                continue;
                        g_Config.m_VxAimHoldBind = Key == KEY_ESCAPE ? -1 : Key;
                        m_BindMode = BIND_NONE;
                        m_BindBlockUntil = Now + time_freq() / 3;
                        break;
                }
        }
}

// ============================================================================
// мировой оверлей (players.cpp): зоны FOV, ESP траекторий, подсветка цели
// ============================================================================
static void AbColor(unsigned int Packed, float Alpha, float &r, float &g, float &b, float &a)
{
        const ColorRGBA C = color_cast<ColorRGBA>(ColorHSLA(Packed));
        r = C.r;
        g = C.g;
        b = C.b;
        a = C.a * Alpha;
}
static void AbDrawCircle(IGraphics *pGraphics, const vec2 &C, float R, int Segments)
{
        float Px = C.x + R, Py = C.y;
        for(int i = 1; i <= Segments; i++)
        {
                const float a = (float)i / Segments * 2.0f * AB_PI;
                const float Nx = C.x + std::cos(a) * R, Ny = C.y + std::sin(a) * R;
                const IGraphics::CLineItem L(Px, Py, Nx, Ny);
                pGraphics->LinesDraw(&L, 1);
                Px = Nx;
                Py = Ny;
        }
}
// клин FOV: две границы + дуга. Радиус и угол — ровно то, что захватывает аим.
static void AbDrawWedge(IGraphics *pGraphics, const vec2 &C, float Ang, float FovDeg, float R)
{
        if(FovDeg >= 360)
        {
                AbDrawCircle(pGraphics, C, R, 48);
                return;
        }
        const float Half = std::clamp(FovDeg, 1, 360) * AB_PI / 360.0f;
        const float a0 = Ang - Half, a1 = Ang + Half;
        const IGraphics::CLineItem L1(C.x, C.y, C.x + std::cos(a0) * R, C.y + std::sin(a0) * R);
        pGraphics->LinesDraw(&L1, 1);
        const IGraphics::CLineItem L2(C.x, C.y, C.x + std::cos(a1) * R, C.y + std::sin(a1) * R);
        pGraphics->LinesDraw(&L2, 1);
        const int Segs = std::clamp((int)(FovDeg / 6), 4, 48);
        float Pa = a0;
        for(int i = 1; i <= Segs; i++)
        {
                const float a = a0 + (a1 - a0) * i / Segs;
                const IGraphics::CLineItem L(C.x + std::cos(Pa) * R, C.y + std::sin(Pa) * R, C.x + std::cos(a) * R, C.y + std::sin(a) * R);
                pGraphics->LinesDraw(&L, 1);
                Pa = a;
        }
}
static void AbDrawCross(IGraphics *pGraphics, const vec2 &P, float R)
{
        const IGraphics::CLineItem aL[2] = {
                IGraphics::CLineItem(P.x - R, P.y, P.x + R, P.y),
                IGraphics::CLineItem(P.x, P.y - R, P.x, P.y + R)};
        pGraphics->LinesDraw(aL, 2);
}

void CAimBot::RenderWorld(CGameClient *pGame)
{
        if(g_Config.m_VxAimEnable == 0)
                return;
        if(pGame->Client()->State() != IClient::STATE_ONLINE || pGame->m_Snap.m_SpecInfo.m_Active)
                return;
        CCharacter *pMe = pGame->m_PredictedWorld.GetCharacterById(pGame->m_Snap.m_LocalClientId);
        if(!pMe)
                return;

        // защита от двойной отрисовки в одном кадре
        const int64_t Now = time_get();
        if(Now < m_NextDrawTime)
                return;
        m_NextDrawTime = Now + time_freq() / 250;

        IGraphics *pGraphics = pGame->Graphics();
        pGame->RenderTools()->MapScreenToGroup(pGame->m_Camera.m_Center.x, pGame->m_Camera.m_Center.y,
                pGame->m_Layers.GameGroup(), pGame->m_Layers.GameGroupEx(), pGame->m_Camera.m_Zoom);

        float Fr, Fg, Fb, Fa;
        AbColor(g_Config.m_VxAimFovColor, 1.0f, Fr, Fg, Fb, Fa);

        // ---- зоны FOV (ровно те, что захватывает аим) ----
        pGraphics->TextureClear();
        pGraphics->LinesBegin();
        if(g_Config.m_VxAimDrawFov != 0)
        {
                if(m_HookFovDeg > 0)
                {
                        pGraphics->SetColor(Fr, Fg, Fb, 0.30f);
                        AbDrawWedge(pGraphics, m_ZonePos, m_ZoneAng, m_HookFovDeg, m_HookZoneR);
                }
                if(m_WepFovDeg > 0)
                {
                        pGraphics->SetColor(Fr, Fg, Fb, 0.18f);
                        AbDrawWedge(pGraphics, m_ZonePos, m_ZoneAng, m_WepFovDeg, m_WepZoneR);
                }
        }
        pGraphics->LinesEnd();

        // ---- ESP траекторий от прицела (граната / дробаш / лазер) ----
        const bool GrenEsp = g_Config.m_VxAimTrajGrenade != 0;
        const bool ShotEsp = g_Config.m_VxAimTrajShotgun != 0;
        const bool LaserEsp = g_Config.m_VxAimTrajLaser != 0;
        if(GrenEsp || ShotEsp || LaserEsp)
        {
                SAbCtx Ctx;
                Ctx.m_pGame = pGame;
                Ctx.m_pCol = pMe->Collision();
                Ctx.m_pMe = pMe;
                Ctx.m_LocalId = pGame->m_Snap.m_LocalClientId;
                Ctx.m_MyPos = pMe->m_Pos;
                Ctx.m_pTun = &pGame->m_aTuning[g_Config.m_ClDummy];
                Ctx.m_Vanilla = pGame->m_PredictedWorld.m_WorldConfig.m_IsVanilla;
                Ctx.m_MouseDir = vec2(std::cos(m_ZoneAng), std::sin(m_ZoneAng));
                Ctx.m_CursorAng = m_ZoneAng;

                float Tr, Tg, Tb, Ta;
                AbColor(g_Config.m_VxAimTrajColor, 1.0f, Tr, Tg, Tb, Ta);
                pGraphics->TextureClear();
                pGraphics->LinesBegin();

                vec2 aPts[AB_TRAJ_MAX];
                int Num = 0;
                vec2 Impact = Ctx.m_MyPos;
                if(GrenEsp)
                {
                        SAbProj W;
                        AbProjParams(&Ctx, WEAPON_GRENADE, W);
                        AbSimTrajProj(&Ctx, Ctx.m_MyPos + Ctx.m_MouseDir * AB_SPAWN, Ctx.m_MouseDir, W, aPts, AB_TRAJ_MAX, Num, Impact);
                        pGraphics->SetColor(Tr, Tg, Tb, 0.55f);
                        for(int i = 0; i + 1 < Num; i++)
                        {
                                const IGraphics::CLineItem L(aPts[i].x, aPts[i].y, aPts[i + 1].x, aPts[i + 1].y);
                                pGraphics->LinesDraw(&L, 1);
                        }
                        // круг взрыва в точке попадания
                        pGraphics->SetColor(Tr, Tg, Tb, 0.25f);
                        AbDrawCircle(pGraphics, Impact, AB_GRENADE_BOOM, 24);
                }
                if(ShotEsp || LaserEsp)
                {
                        if(ShotEsp && Ctx.m_Vanilla)
                        {
                                // vanilla-дробаш: пули с кривизной
                                SAbProj W;
                                AbProjParams(&Ctx, WEAPON_SHOTGUN, W);
                                AbSimTrajProj(&Ctx, Ctx.m_MyPos + Ctx.m_MouseDir * AB_SPAWN, Ctx.m_MouseDir, W, aPts, AB_TRAJ_MAX, Num, Impact);
                                pGraphics->SetColor(Tr, Tg, Tb, 0.55f);
                                for(int i = 0; i + 1 < Num; i++)
                                {
                                        const IGraphics::CLineItem L(aPts[i].x, aPts[i].y, aPts[i + 1].x, aPts[i + 1].y);
                                        pGraphics->LinesDraw(&L, 1);
                                }
                        }
                        else
                        {
                                // DDRace: дробаш и лазер — луч с отражениями (как CLaser)
                                const float Reach = Ctx.m_pTun->m_LaserReach;
                                const float Cost = Ctx.m_pTun->m_LaserBounceCost;
                                const int MaxB = std::clamp((int)Ctx.m_pTun->m_LaserBounceNum, 0, ShotEsp ? 6 : 5);
                                SAbBeam Beam;
                                AbBeamSim(&Ctx, Ctx.m_MyPos, Ctx.m_MouseDir, Reach, Cost, MaxB, Beam);
                                pGraphics->SetColor(Tr, Tg, Tb, 0.55f);
                                for(int b = 0; b < Beam.m_Num; b++)
                                {
                                        const IGraphics::CLineItem L(Beam.m_aSeg[b].m_a.x, Beam.m_aSeg[b].m_a.y, Beam.m_aSeg[b].m_b.x, Beam.m_aSeg[b].m_b.y);
                                        pGraphics->LinesDraw(&L, 1);
                                        if(b > 0) // точка отскока
                                        {
                                                pGraphics->SetColor(Tr, Tg, Tb, 0.85f);
                                                AbDrawCross(pGraphics, Beam.m_aSeg[b].m_a, 4.0f);
                                                pGraphics->SetColor(Tr, Tg, Tb, 0.55f);
                                        }
                                }
                                if(Beam.m_Num > 0)
                                        Impact = Beam.m_aSeg[Beam.m_Num - 1].m_b;
                        }
                }
                // маркер точки попадания
                pGraphics->SetColor(1.0f, 0.9f, 0.3f, 0.85f);
                AbDrawCross(pGraphics, Impact, 7.0f);
                pGraphics->LinesEnd();
        }

        // ---- линия аима + точка предикта (последнее наведение) ----
        const int NowTick = pGame->m_PredictedWorld.GameTick();
        pGraphics->TextureClear();
        pGraphics->LinesBegin();
        if(m_VisUntil >= NowTick && m_AimWeapon >= 0)
        {
                pGraphics->SetColor(Fr, Fg, Fb, 0.6f);
                const IGraphics::CLineItem L(m_From.x, m_From.y, m_AimPoint.x, m_AimPoint.y);
                pGraphics->LinesDraw(&L, 1);
                pGraphics->SetColor(1.0f, 0.9f, 0.3f, 0.8f);
                AbDrawCross(pGraphics, m_PredPoint, 6.0f);
        }
        pGraphics->LinesEnd();

        // ---- подсветка залоченной цели выбранным цветом ----
        if(g_Config.m_VxAimHighlight != 0 && m_TargetId >= 0)
        {
                CCharacter *pT = pGame->m_PredictedWorld.GetCharacterById(m_TargetId);
                if(pT)
                {
                        float Hr, Hg, Hb, Ha;
                        AbColor(g_Config.m_VxAimHlColor, 1.0f, Hr, Hg, Hb, Ha);
                        // мягкое свечение за игроком
                        pGraphics->TextureClear();
                        pGraphics->QuadsBegin();
                        const float aR[3] = {66.0f, 50.0f, 34.0f};
                        const float aA[3] = {0.05f, 0.10f, 0.20f};
                        for(int i = 0; i < 3; i++)
                        {
                                pGraphics->SetColor(Hr, Hg, Hb, Ha * aA[i]);
                                IGraphics::CQuadItem Q(pT->m_Pos.x - aR[i], pT->m_Pos.y - aR[i], 2.0f * aR[i], 2.0f * aR[i]);
                                pGraphics->QuadsDrawTL(&Q, 1);
                        }
                        pGraphics->QuadsEnd();
                        // рамка по хитбоксу
                        pGraphics->TextureClear();
                        pGraphics->LinesBegin();
                        pGraphics->SetColor(Hr, Hg, Hb, Ha * 0.9f);
                        const float S = 17.0f;
                        const IGraphics::CLineItem aL[4] = {
                                IGraphics::CLineItem(pT->m_Pos.x - S, pT->m_Pos.y - S, pT->m_Pos.x + S, pT->m_Pos.y - S),
                                IGraphics::CLineItem(pT->m_Pos.x + S, pT->m_Pos.y - S, pT->m_Pos.x + S, pT->m_Pos.y + S),
                                IGraphics::CLineItem(pT->m_Pos.x + S, pT->m_Pos.y + S, pT->m_Pos.x - S, pT->m_Pos.y + S),
                                IGraphics::CLineItem(pT->m_Pos.x - S, pT->m_Pos.y + S, pT->m_Pos.x - S, pT->m_Pos.y - S)};
                        pGraphics->LinesDraw(aL, 4);
                        pGraphics->LinesEnd();
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
        m_AimWeapon = -1;
        m_VisUntil = -1000000;
        m_FireOwned = false;
        m_LastWorkTick = -1000000;
        m_BindMode = BIND_NONE;
        m_BindBlockUntil = 0;
        m_NextDrawTime = 0;
        m_LogGateTick = -1000000;
        m_HookFovDeg = 0;
        m_WepFovDeg = 0;
}
