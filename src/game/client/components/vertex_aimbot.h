/* Vertex aimbot v2 — hook / hammer / shotgun / grenade / laser.
 *
 * Idea: the hook is solved by SIMULATING it in a copy of the predicted world (so every game rule is respected:
 * pass-through blocks, teams, other players on the way, teleporters). The enemy path comes from the same
 * simulation (gravity, walls, hooks) instead of a straight-line guess. Beams (shotgun / laser) are traced with the
 * real weapon collision, bounces are timed like the real laser, grenades are solved on their real arc.
 * Header-only: include it from controls.cpp, no CMake change needed.
 */
#ifndef GAME_CLIENT_COMPONENTS_VERTEX_AIMBOT_H
#define GAME_CLIENT_COMPONENTS_VERTEX_AIMBOT_H

#include <base/time.h>
#include <base/vmath.h>

#include <engine/client.h>
#include <engine/shared/config.h>

#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/prediction/gameworld.h>
#include <game/collision.h>
#include <game/gamecore.h>

#include <algorithm>
#include <cmath>

namespace VxAim
{
constexpr int MAXT = 64;
constexpr float PI = 3.14159265358979f;
constexpr float PHYS = 28.0f;
constexpr int MAX_ENEMIES = 5;

struct SOut
{
	bool m_Found = false; // a target exists (for the ESP highlight)
	bool m_Override = false; // the aim has to be written into the input
	int m_TargetId = -1;
	vec2 m_Aim = vec2(0.0f, 0.0f);
};

struct SSet
{
	int m_Fov = 360;
	int m_Range = 400;
	int m_Prio = 0; // 0 = closest to the cursor, 1 = closest to me
	bool m_Predict = true;
	bool m_Edge = false;
};

struct SEnemy
{
	int m_Id = -1;
	float m_Key = 0.0f;
	vec2 m_aP[MAXT + 1]; // position now (0) and after s ticks
};

// kind: 0 hook, 1 hammer, 2 shotgun, 3 grenade, 4 laser
inline SSet Settings(int Kind)
{
	SSet S;
	switch(Kind)
	{
	case 0:
		S.m_Fov = g_Config.m_ClHookAimFov;
		S.m_Range = g_Config.m_VxAimHookRange;
		S.m_Prio = g_Config.m_VxAimHookPrio;
		S.m_Predict = g_Config.m_VxAimHookPredict != 0;
		S.m_Edge = g_Config.m_VxAimHookEdge != 0;
		break;
	case 1:
		S.m_Fov = g_Config.m_ClVertexAimbotHammerFov;
		S.m_Range = g_Config.m_VxAimHammerRange;
		S.m_Prio = g_Config.m_VxAimHammerPrio;
		S.m_Predict = g_Config.m_VxAimHammerPredict != 0;
		break;
	case 2:
		S.m_Fov = g_Config.m_ClVertexAimbotShotgunFov;
		S.m_Range = g_Config.m_VxAimShotgunRange;
		S.m_Prio = g_Config.m_VxAimShotgunPrio;
		S.m_Predict = g_Config.m_VxAimShotgunPredict != 0;
		break;
	case 3:
		S.m_Fov = g_Config.m_ClVertexAimbotGrenadeFov;
		S.m_Range = g_Config.m_VxAimGrenadeRange;
		S.m_Prio = g_Config.m_VxAimGrenadePrio;
		S.m_Predict = g_Config.m_VxAimGrenadePredict != 0;
		break;
	default:
		S.m_Fov = g_Config.m_ClVertexAimbotLaserFov;
		S.m_Range = g_Config.m_VxAimLaserRange;
		S.m_Prio = g_Config.m_VxAimLaserPrio;
		S.m_Predict = g_Config.m_VxAimLaserPredict != 0;
		break;
	}
	S.m_Fov = std::clamp(S.m_Fov, 1, 360);
	S.m_Range = std::clamp(S.m_Range, 20, 1500);
	return S;
}

inline float AngleDiff(const vec2 &A, const vec2 &B)
{
	const float La = length(A), Lb = length(B);
	if(La < 0.0001f || Lb < 0.0001f)
		return PI;
	return acosf(std::clamp((A.x * B.x + A.y * B.y) / (La * Lb), -1.0f, 1.0f));
}

inline bool InFov(const vec2 &Mouse, const vec2 &Dir, int FovDeg)
{
	if(FovDeg >= 360)
		return true;
	return AngleDiff(Mouse, Dir) <= (float)FovDeg * PI / 360.0f;
}

// the enemies the aim may choose from (sorted by the priority key)
inline int Gather(CGameClient *pGame, int LocalId, int DummyId, const vec2 &Me, const vec2 &Mouse, const SSet &S, bool EnemyInFov, SEnemy *pOut)
{
	int N = 0;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(i == LocalId || i == DummyId)
			continue;
		const auto &C = pGame->m_aClients[i];
		if(!C.m_Active || C.m_Spec)
			continue;
		CCharacter *pC = pGame->m_PredictedWorld.GetCharacterById(i);
		if(!pC)
			continue;
		const vec2 D = pC->m_Pos - Me;
		const float Dist = length(D);
		if(Dist < 1.0f || Dist > (float)S.m_Range + 40.0f)
			continue;
		if(EnemyInFov && !InFov(Mouse, D, S.m_Fov))
			continue;
		const float Key = S.m_Prio == 1 ? Dist : AngleDiff(Mouse, D) * 1000.0f + Dist * 0.01f;
		if(N < MAX_ENEMIES)
		{
			pOut[N].m_Id = i;
			pOut[N].m_Key = Key;
			N++;
		}
		else
		{
			int Worst = 0;
			for(int k = 1; k < N; k++)
				if(pOut[k].m_Key > pOut[Worst].m_Key)
					Worst = k;
			if(Key < pOut[Worst].m_Key)
			{
				pOut[Worst].m_Id = i;
				pOut[Worst].m_Key = Key;
			}
		}
	}
	std::sort(pOut, pOut + N, [](const SEnemy &A, const SEnemy &B) { return A.m_Key < B.m_Key; });
	return N;
}

// delete the tees that cannot matter in a world copy (far away and not linked by a hook)
inline void Prune(CGameWorld &W, int KeepA, int KeepB, const vec2 &Center, float Radius)
{
	CCharacter *pA = W.GetCharacterById(KeepA);
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(i == KeepA || i == KeepB)
			continue;
		CCharacter *pO = W.GetCharacterById(i);
		if(!pO)
			continue;
		const bool Linked = (pA && (pA->Core()->HookedPlayer() == i || pO->Core()->HookedPlayer() == KeepA));
		if(!Linked && distance(pO->m_Pos, Center) > Radius)
			delete pO;
	}
}

// where the enemy will be after 0..Len ticks (simulated in a copy of the predicted world)
inline bool SimulateEnemy(CGameClient *pGame, int Id, int Len, bool Predict, vec2 *pOut)
{
	CCharacter *pOrig = pGame->m_PredictedWorld.GetCharacterById(Id);
	if(!pOrig)
		return false;
	Len = std::min(Len, MAXT);
	pOut[0] = pOrig->m_Pos;
	if(!Predict)
	{
		for(int s = 1; s <= Len; s++)
			pOut[s] = pOut[0];
		return true;
	}
	CGameWorld W;
	W.CopyWorld(&pGame->m_PredictedWorld);
	CCharacter *pC = W.GetCharacterById(Id);
	if(!pC)
		return false;
	Prune(W, Id, -1, pC->m_Pos, 600.0f);
	pC = W.GetCharacterById(Id);
	if(!pC)
		return false;
	const int Start = W.GameTick();
	for(int s = 1; s <= Len; s++)
	{
		W.m_GameTick = Start + s;
		W.Tick();
		pC = W.GetCharacterById(Id);
		if(!pC)
		{
			for(; s <= Len; s++)
				pOut[s] = pOut[s - 1];
			break;
		}
		pOut[s] = pC->m_Pos;
	}
	return true;
}

// ---------------- hook ----------------
// Does the hook, fired along Aim and held, really grab TargetId? Simulated with the game's own rules.
inline bool HookSim(CGameClient *pGame, int LocalId, int TargetId, const CNetObj_PlayerInput &In, const vec2 &Aim, int Ticks)
{
	CGameWorld W;
	W.CopyWorld(&pGame->m_PredictedWorld);
	CCharacter *pC = W.GetCharacterById(LocalId);
	if(!pC)
		return false;
	Prune(W, LocalId, TargetId, pC->m_Pos, 900.0f);
	pC = W.GetCharacterById(LocalId);
	if(!pC)
		return false;
	CNetObj_PlayerInput I = In;
	I.m_Hook = 1;
	I.m_TargetX = (int)Aim.x;
	I.m_TargetY = (int)Aim.y;
	const int Start = W.GameTick();
	for(int s = 1; s <= Ticks; s++)
	{
		pC->OnDirectInput(&I);
		W.m_GameTick = Start + s;
		pC->OnPredictedInput(&I);
		W.Tick();
		pC = W.GetCharacterById(LocalId);
		if(!pC)
			return false;
		if(pC->Core()->HookedPlayer() == TargetId)
			return true;
	}
	return false;
}

inline bool SolveHook(CGameClient *pGame, CCharacter *pMe, int LocalId, const SEnemy &E, const SSet &S, const vec2 &Mouse, const CNetObj_PlayerInput &In, int64_t Deadline, vec2 *pAim)
{
	const CTuningParams &T = pGame->m_aTuning[g_Config.m_ClDummy];
	const float Sp = std::max(1.0f, (float)T.m_HookFireSpeed);
	const float HL = T.m_HookLength;
	const vec2 Me = pMe->m_Pos;
	const int Tmax = std::min(14, (int)(HL / Sp) + 3);

	// first tick on which the hook head can reach the enemy
	int T0 = -1;
	for(int t = 1; t <= Tmax; t++)
	{
		const float Reach = 42.0f + (float)t * Sp;
		const float D = distance(Me, E.m_aP[t]);
		if(D <= HL + 30.0f && D <= Reach + 30.0f)
		{
			T0 = t;
			break;
		}
	}
	if(T0 < 0)
		return false;

	// aim points on the enemy: centre first; the edge scanner adds the rim of the hitbox (for shots blocked by a wall corner)
	vec2 aOff[16];
	int NumOff = 0;
	aOff[NumOff++] = vec2(0.0f, 0.0f);
	if(S.m_Edge)
	{
		for(int k = 0; k < 4; k++)
			aOff[NumOff++] = vec2(cosf(k * PI / 2.0f), sinf(k * PI / 2.0f)) * 11.0f;
		for(int k = 0; k < 8; k++)
			aOff[NumOff++] = vec2(cosf(k * PI / 4.0f + 0.3927f), sinf(k * PI / 4.0f + 0.3927f)) * 23.0f;
	}

	for(int dt = 0; dt <= 1; dt++)
	{
		const int t = std::min(T0 + dt, MAXT);
		for(int o = 0; o < NumOff; o++)
		{
			const vec2 P = E.m_aP[t] + aOff[o];
			const vec2 D = P - Me;
			const float L = length(D);
			if(L < 1.0f || L > HL + 20.0f)
				continue;
			if(!InFov(Mouse, D, S.m_Fov))
				continue;
			if(time_get() > Deadline)
				return false;
			const vec2 Dir = D / L;
			const vec2 Aim = vec2((float)(int)(Dir.x * 400.0f), (float)(int)(Dir.y * 400.0f));
			if(HookSim(pGame, LocalId, E.m_Id, In, Aim, std::min(T0 + 3, 16)))
			{
				*pAim = Aim;
				return true;
			}
		}
	}
	return false;
}

// ---------------- beams (shotgun / laser) ----------------
inline bool TraceBeam(CCollision *pCol, const vec2 &Origin, const vec2 &Own, float Angle, const SEnemy &E, int ELen, float Reach, float BounceCost, int MaxBounces, int BDelay, const vec2 *pBlk, int NumBlk, int *pBounces)
{
	vec2 SegStart = Origin;
	vec2 SegDir(cosf(Angle), sinf(Angle));
	float Energy = Reach;
	for(int b = 0; b <= MaxBounces && Energy > 1.0f; b++)
	{
		const vec2 To = SegStart + SegDir * Energy;
		vec2 ColTile, Before;
		int TeleNr = 0;
		const int Hit = pCol->IntersectLineTeleWeapon(SegStart, To, &ColTile, &Before, &TeleNr);
		const vec2 SegEnd = Hit ? Before : To;
		vec2 Cl;
		// after a bounce the beam can hit its owner
		if(b > 0 && closest_point_on_line(SegStart, SegEnd, Own, Cl) && distance(Own, Cl) < PHYS)
			return false;
		// the real laser evaluates segment b at tick 1 + b * BounceDelay after the shot
		const int Idx = std::min(b * BDelay + 1, ELen);
		if(closest_point_on_line(SegStart, SegEnd, E.m_aP[Idx], Cl) && distance(E.m_aP[Idx], Cl) < (b == 0 ? 24.0f : 22.0f))
		{
			bool Ok = true;
			if(b > 0)
			{
				for(int d = -1; d <= 1; d += 2)
				{
					const int J = std::clamp(Idx + d, 1, ELen);
					vec2 C2;
					if(!closest_point_on_line(SegStart, SegEnd, E.m_aP[J], C2) || distance(E.m_aP[J], C2) >= 27.0f)
						Ok = false;
				}
			}
			if(Ok)
			{
				for(int k = 0; k < NumBlk; k++)
				{
					vec2 C3;
					if(closest_point_on_line(SegStart, Cl, pBlk[k], C3) && distance(pBlk[k], C3) < PHYS)
						return false; // somebody else takes the hit
				}
				*pBounces = b;
				return true;
			}
		}
		for(int k = 0; k < NumBlk; k++)
		{
			vec2 C3;
			if(closest_point_on_line(SegStart, SegEnd, pBlk[k], C3) && distance(pBlk[k], C3) < PHYS)
				return false; // the beam ends on another tee
		}
		if(!Hit)
			break;
		vec2 Pos = SegEnd;
		vec2 Vel = SegDir * 4.0f;
		int Bn = 0;
		pCol->MovePoint(&Pos, &Vel, 1.0f, &Bn);
		Energy -= distance(SegStart, Pos) + BounceCost;
		if(length(Vel) < 0.001f)
			break;
		SegDir = normalize(Vel);
		SegStart = Pos;
	}
	return false;
}

inline bool SolveBeam(CGameClient *pGame, CCollision *pCol, const vec2 &Origin, const vec2 &Own, const SEnemy &E, int ELen, const SSet &S, const vec2 &Mouse, const vec2 *pBlk, int NumBlk, int64_t Deadline, vec2 *pAim)
{
	const CTuningParams &T = pGame->m_aTuning[g_Config.m_ClDummy];
	float Reach = T.m_LaserReach;
	if(Reach <= 1.0f)
		Reach = 800.0f;
	Reach = std::min(Reach, (float)S.m_Range + 40.0f);
	const int MaxB = T.m_LaserBounceNum;
	const float BCost = T.m_LaserBounceCost;
	const int BDelay = (int)(50.0f * (float)T.m_LaserBounceDelay / 1000.0f) + 1;

	// 1) straight line at the predicted position
	const vec2 ToE = E.m_aP[1] - Origin;
	if(length(ToE) > 1.0f)
	{
		const float Base = atan2f(ToE.y, ToE.x);
		for(int k = 0; k <= 24; k++)
		{
			const int Sg = (k & 1) ? (k + 1) / 2 : -(k / 2);
			const float Ang = Base + (float)Sg * 0.25f * PI / 180.0f;
			const vec2 Dir(cosf(Ang), sinf(Ang));
			if(!InFov(Mouse, Dir, S.m_Fov))
				continue;
			int B = 0;
			if(TraceBeam(pCol, Origin, Own, Ang, E, ELen, Reach, BCost, MaxB, BDelay, pBlk, NumBlk, &B))
			{
				*pAim = Dir * 1500.0f;
				return true;
			}
		}
	}
	// 2) ricochets: sweep around the cursor, the fewest bounces and the smallest deviation win
	if(MaxB <= 0)
		return false;
	const float MouseAng = atan2f(Mouse.y, Mouse.x);
	const int HalfDeg = std::min(180, S.m_Fov / 2);
	const float Step = (HalfDeg > 45 ? 1.0f : 0.5f) * PI / 180.0f;
	const int Count = (int)((float)HalfDeg * PI / 180.0f / Step);
	float BestScore = -1e30f;
	vec2 BestAim(0.0f, 0.0f);
	for(int k = 0; k <= Count * 2; k++)
	{
		if(time_get() > Deadline)
			break;
		const int Sg = (k & 1) ? (k + 1) / 2 : -(k / 2);
		if(std::abs(Sg) > Count)
			continue;
		const float Ang = MouseAng + (float)Sg * Step;
		int B = 0;
		if(TraceBeam(pCol, Origin, Own, Ang, E, ELen, Reach, BCost, MaxB, BDelay, pBlk, NumBlk, &B))
		{
			const float Sc = 1000.0f - (float)B * 30.0f - (float)std::abs(Sg) * Step * 180.0f / PI;
			if(Sc > BestScore)
			{
				BestScore = Sc;
				BestAim = vec2(cosf(Ang), sinf(Ang)) * 1500.0f;
			}
		}
	}
	if(BestScore > -1e29f)
	{
		*pAim = BestAim;
		return true;
	}
	return false;
}

// ---------------- hammer ----------------
inline bool SolveHammer(CCollision *pCol, const vec2 &Origin, const SEnemy &E, const SSet &S, const vec2 &Mouse, vec2 *pAim)
{
	for(int t = 1; t <= 2; t++)
	{
		const vec2 P = E.m_aP[t];
		const vec2 D = P - Origin;
		const float L = length(D);
		if(L < 1.0f)
			continue;
		const vec2 Dir = D / L;
		const vec2 Start = Origin + Dir * 21.0f; // the hit circle starts PhysSize * 0.75 ahead
		if(distance(Start, P) >= 40.0f)
			continue;
		if(!InFov(Mouse, Dir, S.m_Fov))
			continue;
		vec2 C, B;
		if(pCol->IntersectLine(Start, P, &C, &B) != 0)
			continue; // a wall between
		*pAim = Dir * 400.0f;
		return true;
	}
	return false;
}

// ---------------- grenade ----------------
inline bool SolveGrenade(CGameClient *pGame, CCollision *pCol, const vec2 &Origin, const SEnemy &E, int ELen, const SSet &S, const vec2 &Mouse, const vec2 *pBlk, int NumBlk, vec2 *pAim)
{
	const CTuningParams &T = pGame->m_aTuning[g_Config.m_ClDummy];
	const float Sg = std::max(1.0f, (float)T.m_GrenadeSpeed);
	const float C = (float)T.m_GrenadeCurvature / 10000.0f;
	for(int k = 1; k <= 45 && k + 1 <= ELen; k++)
	{
		const vec2 Tgt = E.m_aP[k + 1]; // the grenade flies k ticks after the shot tick
		if(distance(Origin, Tgt) > (float)S.m_Range)
			break;
		const float L = Sg * (float)k / 50.0f;
		vec2 Dir = Tgt - Origin;
		if(length(Dir) < 1.0f)
			continue;
		Dir = normalize(Dir);
		vec2 Start = Origin + Dir * 21.0f;
		for(int It = 0; It < 3; It++)
		{
			const vec2 Need = Tgt - Start - vec2(0.0f, C * L * L);
			if(length(Need) < 1.0f)
				break;
			Dir = normalize(Need);
			Start = Origin + Dir * 21.0f;
		}
		const vec2 Need = Tgt - Start - vec2(0.0f, C * L * L);
		if(fabsf(length(Need) - L) > 14.0f)
			continue; // the arc does not arrive at this tick
		Dir = normalize(Need);
		if(!InFov(Mouse, Dir, S.m_Fov))
			continue;
		bool Blocked = false;
		vec2 Prev = Start;
		for(int i = 1; i <= 16 && !Blocked; i++)
		{
			const float Lt = Sg * ((float)k * (float)i / 16.0f) / 50.0f;
			const vec2 Pt = Start + Dir * Lt + vec2(0.0f, C * Lt * Lt);
			vec2 Col, Bef;
			if(pCol->IntersectLine(Prev, Pt, &Col, &Bef) != 0 && distance(Col, Tgt) > 48.0f)
				Blocked = true;
			for(int b = 0; b < NumBlk && !Blocked; b++)
				if(distance(Pt, pBlk[b]) < PHYS + 6.0f)
					Blocked = true;
			Prev = Pt;
		}
		if(Blocked)
			continue;
		*pAim = Dir * 1500.0f;
		return true;
	}
	return false;
}

// ---------------- entry point ----------------
inline SOut Run(CGameClient *pGame, IClient *pClient, CCollision *pCol, const vec2 &Mouse, const CNetObj_PlayerInput &In)
{
	(void)pClient;
	SOut Out;
	const int LocalId = pGame->m_Snap.m_LocalClientId;
	if(LocalId < 0)
		return Out;
	CCharacter *pMe = pGame->m_PredictedWorld.GetCharacterById(LocalId);
	if(!pMe)
		return Out;
	if(length(Mouse) < 0.001f)
		return Out;
	const int DummyId = pGame->m_aLocalIds[!g_Config.m_ClDummy];
	const int64_t Deadline = time_get() + time_freq() * 3 / 1000;
	const vec2 MePos = pMe->m_Pos;

	// ---- hook ----
	if(g_Config.m_ClHookAim)
	{
		const SSet S = Settings(0);
		SEnemy aE[MAX_ENEMIES];
		const int N = Gather(pGame, LocalId, DummyId, MePos, Mouse, S, true, aE);
		const bool Held = In.m_Hook == 1 && pMe->Core()->m_HookState != HOOK_GRABBED;
		if(N > 0)
		{
			if(!Held)
			{
				// preview only: the highlighted target, no aim override
				Out.m_Found = true;
				Out.m_TargetId = aE[0].m_Id;
				return Out;
			}
			for(int n = 0; n < N; n++)
			{
				if(time_get() > Deadline)
					break;
				if(!SimulateEnemy(pGame, aE[n].m_Id, 14, S.m_Predict, aE[n].m_aP))
					continue;
				vec2 Aim;
				if(SolveHook(pGame, pMe, LocalId, aE[n], S, Mouse, In, Deadline, &Aim))
				{
					Out.m_Found = true;
					Out.m_Override = true;
					Out.m_TargetId = aE[n].m_Id;
					Out.m_Aim = Aim;
					return Out;
				}
			}
		}
	}

	// ---- weapons: only while firing without the hook ----
	if(In.m_Hook != 0 || (In.m_Fire & 1) == 0)
		return Out;
	int Kind = -1;
	bool On = false;
	switch(pMe->Core()->m_ActiveWeapon)
	{
	case WEAPON_HAMMER:
		Kind = 1;
		On = g_Config.m_ClVertexAimbotHammerAim != 0;
		break;
	case WEAPON_SHOTGUN:
		Kind = 2;
		On = g_Config.m_ClVertexAimbotShotgunAim != 0;
		break;
	case WEAPON_GRENADE:
		Kind = 3;
		On = g_Config.m_ClVertexAimbotGrenadeAim != 0;
		break;
	case WEAPON_LASER:
		Kind = 4;
		On = g_Config.m_ClVertexAimbotLaserAim != 0;
		break;
	default:
		break;
	}
	if(!On)
		return Out;

	const SSet S = Settings(Kind);
	const bool Beam = Kind == 2 || Kind == 4;
	SEnemy aE[MAX_ENEMIES];
	const int N = Gather(pGame, LocalId, DummyId, MePos, Mouse, S, !Beam, aE);
	if(N == 0)
		return Out;

	// where the shot starts: where I will be when the server fires
	const vec2 Origin = MePos + pMe->Core()->m_Vel;

	for(int n = 0; n < N; n++)
	{
		if(time_get() > Deadline)
			break;
		const SEnemy &Ref = aE[n];
		const int Len = Kind == 1 ? 3 : (Kind == 3 ? 48 : 40);
		SEnemy E;
		E.m_Id = Ref.m_Id;
		if(!SimulateEnemy(pGame, E.m_Id, Len, S.m_Predict, E.m_aP))
			continue;

		// other tees that would take the hit
		vec2 aBlk[MAX_CLIENTS];
		int NumBlk = 0;
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(i == LocalId || i == E.m_Id || i == DummyId)
				continue;
			CCharacter *pO = pGame->m_PredictedWorld.GetCharacterById(i);
			if(!pO || distance(pO->m_Pos, MePos) > 900.0f)
				continue;
			aBlk[NumBlk++] = pO->m_Pos + pO->Core()->m_Vel * 2.0f;
		}

		vec2 Aim;
		bool Ok = false;
		if(Kind == 1)
			Ok = SolveHammer(pCol, Origin, E, S, Mouse, &Aim);
		else if(Kind == 3)
			Ok = SolveGrenade(pGame, pCol, Origin, E, Len, S, Mouse, aBlk, NumBlk, &Aim);
		else
			Ok = SolveBeam(pGame, pCol, Origin, Origin, E, Len, S, Mouse, aBlk, NumBlk, Deadline, &Aim);
		if(Ok)
		{
			Out.m_Found = true;
			Out.m_Override = true;
			Out.m_TargetId = E.m_Id;
			Out.m_Aim = Aim;
			return Out;
		}
	}
	return Out;
}
} // namespace VxAim

#endif
