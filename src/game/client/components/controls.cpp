/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "controls.h"
#include "aled.h"

#include <base/dbg.h>
#include <base/mem.h>
#include <base/time.h>
#include <base/vmath.h>

#include <engine/client.h>
#include <engine/shared/protocol.h>
#include <engine/shared/config.h>
#include <engine/graphics.h>

#include <generated/protocol.h>

#include <game/client/components/camera.h>
#include <game/client/components/chat.h>
#include <game/client/components/menus.h>
#include <game/client/components/scoreboard.h>
#include <game/client/gameclient.h>
#include <game/collision.h>
#include <game/gamecore.h>
#include <game/mapitems.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <memory>
#include <vector>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include <game/client/prediction/entities/laser.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/prediction/gameworld.h>

// Dummy Unfreeze ESP: where / along which path the dummy's laser will unfreeze me (filled by VertexDummyUnfreeze, drawn in players.cpp)
vec2 g_VertexDuGhostPos = vec2(0.0f, 0.0f);
int g_VertexDuGhostUntil = -1;
vec2 g_VertexDuPath[64];
int g_VertexDuPathN = 0;

// ============================================================================
// Dummy Unfreeze v7 (single thread, tiny cost; ricochet search upgraded from the auto-unfreeze bot).
// ============================================================================
namespace
{
constexpr int DU_MAXT = 9; 
constexpr int DU_MAXLEN = 64; 
constexpr int DU_MIN_GAIN = 8; 
constexpr int DU_FRESH_LEFT = 112; 
constexpr int DU_GAP = 6; 
constexpr float DU_PHYS = 28.0f;
constexpr float DU_HIT = 25.0f; 
constexpr float DU_AIM_LEN = 2000.0f;
constexpr float DU_PI = 3.14159265358979323846f;
constexpr float DU_KEEP_RADIUS = 600.0f; 
constexpr double DU_SHOT_BUDGET_MS = 2.5; 
constexpr double DU_ESP_BUDGET_MS = 2.0; 

struct SDuSim
{
	vec2 m_aP[DU_MAXLEN + 1];
	bool m_aF[DU_MAXLEN + 1]; 
	int m_aL[DU_MAXLEN + 1]; 
	bool m_aOn[DU_MAXLEN + 1]; 
	int m_Len = 0;
};

struct SDuState
{
	int m_FireTick = -1000000;
	vec2 m_Aim = vec2(0.0f, 0.0f);
	int m_aRetry[DU_MAXT] = {}; 
	int m_aForecastNext[DU_MAXT] = {};
	int m_PreTick = -1000000; 
	int m_PreCount = 0;
	int m_PrePlan = 0;
	float m_PreAngle = 0.0f;
	int m_EspNext = 0;
	int m_LastTick = 0;
	int m_LogTick = 0;
	int m_NamesTick = -1000000;
	char m_aNamesCopy[256] = "";
	int m_aNameIds[DU_MAXT] = {};
	int m_NumNames = 0;
	bool m_FireHeld = false;
};
SDuState g_DuState;
float g_DuWarmAngle = 0.0f; 
int g_DuWarmTick = -1000000;
int g_DuScanStart = 0; 

float DuNormAngle(float A)
{
	while(A > DU_PI)
		A -= 2.0f * DU_PI;
	while(A < -DU_PI)
		A += 2.0f * DU_PI;
	return A;
}

bool DuOnFreeze(CCollision *pCol, const vec2 &Pos)
{
	const float R = DU_PHYS / 3.0f;
	const vec2 aPts[5] = {Pos, vec2(Pos.x + R, Pos.y - R), vec2(Pos.x + R, Pos.y + R), vec2(Pos.x - R, Pos.y - R), vec2(Pos.x - R, Pos.y + R)};
	for(const vec2 &P : aPts)
	{
		const int Idx = pCol->GetPureMapIndex(P);
		if(Idx < 0)
			continue;
		if(pCol->GetTileIndex(Idx) == TILE_FREEZE || pCol->GetFrontTileIndex(Idx) == TILE_FREEZE)
			return true;
	}
	return false;
}

bool DuSimulate(CGameClient *pGame, CCollision *pCol, int TargetId, int ShooterId, const CNetObj_PlayerInput *pInput, int FreezeOverride, int UnfreezeAt, int MaxLen, int StopAfterLeave, SDuSim &Out)
{
	MaxLen = std::min(MaxLen, DU_MAXLEN);
	CGameWorld SimWorld;
	SimWorld.CopyWorld(&pGame->m_PredictedWorld);
	CCharacter *pC = SimWorld.GetCharacterById(TargetId);
	if(!pC)
		return false;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(i == TargetId || i == ShooterId)
			continue;
		CCharacter *pOther = SimWorld.GetCharacterById(i);
		if(!pOther)
			continue;
		const bool Linked = pC->Core()->HookedPlayer() == i || pOther->Core()->HookedPlayer() == TargetId;
		if(!Linked && distance(pOther->m_Pos, pC->m_Pos) > DU_KEEP_RADIUS)
			delete pOther;
	}
	pC = SimWorld.GetCharacterById(TargetId);
	if(!pC)
		return false;
	if(FreezeOverride > 0 && pC->m_FreezeTime <= 0)
		pC->m_FreezeTime = FreezeOverride;
	CNetObj_PlayerInput In;
	mem_zero(&In, sizeof(In));
	if(pInput)
		In = *pInput;
	In.m_Fire = 0;
	const int StartTick = SimWorld.GameTick();
	Out.m_aP[0] = pC->m_Pos;
	Out.m_aF[0] = pC->m_FreezeTime > 0;
	Out.m_aL[0] = pC->m_FreezeTime;
	Out.m_aOn[0] = DuOnFreeze(pCol, Out.m_aP[0]);
	Out.m_Len = 0;
	int EnterT = Out.m_aF[0] ? 0 : -1;
	int LeaveT = -1;
	for(int s = 1; s <= MaxLen; s++)
	{
		if(s == UnfreezeAt)
			pC->m_FreezeTime = 0;
		pC->OnDirectInput(&In);
		SimWorld.m_GameTick = StartTick + s;
		pC->OnPredictedInput(&In);
		SimWorld.Tick();
		pC = SimWorld.GetCharacterById(TargetId);
		if(!pC || pC->Core()->m_DeepFrozen)
			return false;
		Out.m_aP[s] = pC->m_Pos;
		Out.m_aF[s] = pC->m_FreezeTime > 0;
		Out.m_aL[s] = pC->m_FreezeTime;
		Out.m_aOn[s] = DuOnFreeze(pCol, Out.m_aP[s]);
		Out.m_Len = s;
		if(StopAfterLeave > 0)
		{
			if(EnterT < 0 && Out.m_aF[s])
				EnterT = s;
			if(EnterT >= 0 && LeaveT < 0 && s - 1 >= EnterT && !Out.m_aOn[s - 1] && !Out.m_aOn[s])
				LeaveT = s - 1;
			if(LeaveT >= 0 && s >= LeaveT + StopAfterLeave)
				break;
		}
	}
	return Out.m_Len >= 1;
}

bool DuVerifyShot(CGameClient *pGame, CCollision *pCol, int TargetId, int ShooterId,
	const CNetObj_PlayerInput *pTargetInput, const CNetObj_PlayerInput *pShooterInput,
	int FreezeOverride, vec2 Aim, int FireAt, int MaxBounces, int BounceDelay, int *pHitT, SDuSim *pOut)
{
	if(FireAt < 1)
		FireAt = 1;
	const int MaxLen = std::min(DU_MAXLEN, FireAt + MaxBounces * BounceDelay + 14);
	CGameWorld SimWorld;
	SimWorld.CopyWorld(&pGame->m_PredictedWorld);
	CCharacter *pC = SimWorld.GetCharacterById(TargetId);
	CCharacter *pS = SimWorld.GetCharacterById(ShooterId);
	if(!pC || !pS)
		return false;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(i == TargetId || i == ShooterId)
			continue;
		CCharacter *pOther = SimWorld.GetCharacterById(i);
		if(!pOther)
			continue;
		const bool Linked = pC->Core()->HookedPlayer() == i || pOther->Core()->HookedPlayer() == TargetId ||
			pS->Core()->HookedPlayer() == i || pOther->Core()->HookedPlayer() == ShooterId;
		if(!Linked && distance(pOther->m_Pos, pC->m_Pos) > DU_KEEP_RADIUS && distance(pOther->m_Pos, pS->m_Pos) > DU_KEEP_RADIUS)
			delete pOther;
	}
	pC = SimWorld.GetCharacterById(TargetId);
	pS = SimWorld.GetCharacterById(ShooterId);
	if(!pC || !pS)
		return false;
	if(FreezeOverride > 0 && pC->m_FreezeTime <= 0)
		pC->m_FreezeTime = FreezeOverride;
	CNetObj_PlayerInput TIn;
	mem_zero(&TIn, sizeof(TIn));
	if(pTargetInput)
		TIn = *pTargetInput;
	TIn.m_Fire = 0;
	CNetObj_PlayerInput SIn;
	mem_zero(&SIn, sizeof(SIn));
	if(pShooterInput)
		SIn = *pShooterInput;
	SIn.m_Fire = 0;
	SIn.m_WantedWeapon = 0; 
	const int StartTick = SimWorld.GameTick();
	const int Flight = MaxBounces * BounceDelay + 2; 
	pOut->m_aP[0] = pC->m_Pos;
	pOut->m_aF[0] = pC->m_FreezeTime > 0;
	pOut->m_aL[0] = pC->m_FreezeTime;
	pOut->m_aOn[0] = DuOnFreeze(pCol, pOut->m_aP[0]);
	pOut->m_Len = 0;
	int HitT = -1;
	for(int s = 1; s <= MaxLen; s++)
	{
		if(s == FireAt)
		{
			SIn.m_Fire = (SIn.m_Fire & 1) ? SIn.m_Fire + 2 : SIn.m_Fire + 1;
			SIn.m_TargetX = (int)Aim.x;
			SIn.m_TargetY = (int)Aim.y;
		}
		else if(s == FireAt + 1 && (SIn.m_Fire & 1))
			SIn.m_Fire++;
		if(pS)
			pS->OnDirectInput(&SIn);
		pC->OnDirectInput(&TIn);
		SimWorld.m_GameTick = StartTick + s;
		if(pS)
			pS->OnPredictedInput(&SIn);
		pC->OnPredictedInput(&TIn);
		SimWorld.Tick();
		pS = SimWorld.GetCharacterById(ShooterId);
		pC = SimWorld.GetCharacterById(TargetId);
		if(!pC || pC->Core()->m_DeepFrozen)
			return false;
		pOut->m_aP[s] = pC->m_Pos;
		pOut->m_aF[s] = pC->m_FreezeTime > 0;
		pOut->m_aL[s] = pC->m_FreezeTime;
		pOut->m_aOn[s] = DuOnFreeze(pCol, pOut->m_aP[s]);
		pOut->m_Len = s;
		if(HitT < 0 && s >= FireAt && s <= FireAt + Flight && pOut->m_aF[s - 1] && !pOut->m_aF[s] && pOut->m_aL[s - 1] >= DU_MIN_GAIN)
			HitT = s;
	}
	if(HitT < 0)
		return false;
	const bool Young = pOut->m_aL[HitT - 1] >= DU_FRESH_LEFT; 
	const int End = std::min(pOut->m_Len, HitT + 8);
	for(int t = HitT; t <= End; t++)
	{
		if(pOut->m_aF[t] || (pOut->m_aOn[t] && !Young))
			return false;
	}
	if(pHitT)
		*pHitT = HitT;
	return true;
}

struct SDuCtx
{
	CCollision *m_pCol;
	vec2 m_DummyPos;
	float m_Reach;
	float m_BounceCost;
	int m_MaxBounces;
	int m_BounceDelay;
	vec2 m_aBlk[MAX_CLIENTS]; 
	int m_NumBlk = 0;
	const SDuSim *m_pOwner = nullptr; 
};

bool DuBeamCut(const SDuCtx &C, const vec2 &A, const vec2 &B)
{
	for(int k = 0; k < C.m_NumBlk; k++)
	{
		vec2 Cl;
		if(closest_point_on_line(A, B, C.m_aBlk[k], Cl) && distance(C.m_aBlk[k], Cl) < 32.0f)
			return true;
	}
	return false;
}

struct SDuHit
{
	bool m_Found = false;
	float m_Angle = 0.0f;
	float m_Score = -1e30f;
	int m_Idx = 0; 
	int m_T = 0; 
	int m_Bounces = 0;
};

bool DuTrace(const SDuCtx &C, const SDuSim &S, float Angle, int TMax, SDuHit *pOut, float *pNear)
{
	const int AimX = (int)roundf(cosf(Angle) * DU_AIM_LEN);
	const int AimY = (int)roundf(sinf(Angle) * DU_AIM_LEN);
	if(AimX == 0 && AimY == 0)
		return false;
	vec2 SegStart = C.m_DummyPos;
	vec2 SegDir = normalize(vec2((float)AimX, (float)AimY));
	float Energy = C.m_Reach;
	for(int b = 0; b <= C.m_MaxBounces && Energy > 1.0f; b++)
	{
		const vec2 To = SegStart + SegDir * Energy;
		vec2 ColTile, Before;
		int TeleNr = 0;
		const int Hit = C.m_pCol->IntersectLineTeleWeapon(SegStart, To, &ColTile, &Before, &TeleNr);
		const vec2 SegEnd = Hit ? Before : To;

			if(b > 0)
			{
				bool OwnHit = false;
				if(C.m_pOwner && C.m_pOwner->m_Len > 0)
				{
					const int LoT = std::max(0, (b - 1) * C.m_BounceDelay);
					const int HiT = std::min(b * C.m_BounceDelay, C.m_pOwner->m_Len);
					for(int Ot = LoT; Ot <= HiT && !OwnHit; Ot++)
					{
						vec2 ClOwn;
						if(closest_point_on_line(SegStart, SegEnd, C.m_pOwner->m_aP[Ot], ClOwn) && distance(C.m_pOwner->m_aP[Ot], ClOwn) < DU_PHYS)
							OwnHit = true;
					}
				}
				else
				{
					vec2 ClOwn;
					OwnHit = closest_point_on_line(SegStart, SegEnd, C.m_DummyPos, ClOwn) && distance(C.m_DummyPos, ClOwn) < DU_PHYS;
				}
				if(OwnHit)
					return false; 
			}

			const int Travel = b == 0 ? 1 : b * C.m_BounceDelay;
			for(int T = 0; T <= TMax; T++)
			{
				const int Idx0 = Travel + T;
				if(Idx0 + 1 > S.m_Len)
					break;
				vec2 C0;
				if(!closest_point_on_line(SegStart, SegEnd, S.m_aP[Idx0], C0))
					continue;
				const float D0 = distance(S.m_aP[Idx0], C0);
				if(pNear && D0 < *pNear)
					*pNear = D0;
				if(D0 >= DU_HIT + 80.0f)
					continue;
				float Worst = D0;
				bool Cover = true;
				const int Lo = std::max(1, Idx0 - 1);
				const int Hi = std::min(Idx0 + 1, S.m_Len);
				for(int Ix = Lo; Ix <= Hi; Ix++)
				{
					if(Ix == Idx0)
						continue;
					vec2 Closest;
					if(!closest_point_on_line(SegStart, SegEnd, S.m_aP[Ix], Closest))
					{
						Cover = false;
						break;
					}
					Worst = std::max(Worst, distance(S.m_aP[Ix], Closest));
				}
				if(!Cover)
					continue;
				if(Worst >= DU_HIT)
					continue;
			bool Frozen = true;
			for(int Ix = Lo; Ix <= Hi; Ix++)
				Frozen = Frozen && S.m_aF[Ix];
			if(!Frozen || S.m_aL[Idx0] < DU_MIN_GAIN || (S.m_aOn[Idx0] && (S.m_aL[Idx0] < DU_FRESH_LEFT || length(S.m_aP[std::min(Idx0 + 1, S.m_Len)] - S.m_aP[Idx0]) < 1.5f)))
				continue;
			vec2 ClCut;
			if(C.m_NumBlk > 0 && closest_point_on_line(SegStart, SegEnd, S.m_aP[Idx0], ClCut) && DuBeamCut(C, SegStart, ClCut))
				return false; 
			pOut->m_Found = true;
			pOut->m_Angle = Angle;
			pOut->m_Score = 1000.0f - (float)b * 40.0f - (float)Idx0 * 3.0f + (DU_HIT - Worst) * 4.0f;
			pOut->m_Idx = Idx0;
			pOut->m_T = T;
			pOut->m_Bounces = b;
			return true;
		}
		if(C.m_NumBlk > 0 && DuBeamCut(C, SegStart, SegEnd))
			return false; 
		if(!Hit)
			break;
		vec2 Pos = SegEnd;
		vec2 Vel = SegDir * 4.0f;
		int Bounces = 0;
		C.m_pCol->MovePoint(&Pos, &Vel, 1.0f, &Bounces);
		Energy -= distance(SegStart, Pos) + C.m_BounceCost;
		if(length(Vel) < 0.001f)
			break;
		SegDir = normalize(Vel);
		SegStart = Pos;
	}
	return false;
}

template<typename F>
SDuHit DuFindShot(const SDuCtx &C, const SDuSim &S, int TMax, F &&OverBudget)
{
	SDuHit Best;
	auto Try = [&](float Ang, float *pNear) {
		SDuHit H;
		if(DuTrace(C, S, Ang, TMax, &H, pNear) && H.m_Score > Best.m_Score)
			Best = H;
	};
	int Positions = 0;
	for(int Ix = 1; Ix <= std::min(S.m_Len, 1 + TMax) && Ix <= 3 + TMax && Positions < 8; Ix += (TMax > 0 ? 2 : 1), Positions++)
	{
		const vec2 ToT = S.m_aP[Ix] - C.m_DummyPos;
		if(length(ToT) < 1.0f)
			continue;
		const float Base = atan2f(ToT.y, ToT.x);
		for(int k = -4; k <= 4; k++)
			Try(Base + (float)k * 0.25f * DU_PI / 180.0f, nullptr);
	}
	if(Best.m_Found && Best.m_Bounces == 0)
		return Best;
	if(C.m_MaxBounces <= 0)
		return Best;
	struct SNear
	{
		float m_Near;
		float m_Angle;
	};
	if(g_DuWarmTick >= 0)
	{
		for(int k = -16; k <= 16; k++)
			Try(g_DuWarmAngle + (float)k * 0.25f * DU_PI / 180.0f, nullptr);
		if(Best.m_Found)
		{
			g_DuWarmAngle = DuNormAngle(Best.m_Angle);
			return Best;
		}
	}
	SNear aNear[16];
	int NumNear = 0;
	auto Push = [&](float Near, float Ang) {
		if(NumNear < 16)
		{
			aNear[NumNear++] = SNear{Near, Ang};
			return;
		}
		int Worst = 0;
		for(int i = 1; i < NumNear; i++)
			if(aNear[i].m_Near > aNear[Worst].m_Near)
				Worst = i;
		if(Near < aNear[Worst].m_Near)
			aNear[Worst] = SNear{Near, Ang};
	};
	constexpr int SWEEP = 360;
	int Scanned = 0;
	for(int a = 0; a < SWEEP && !OverBudget(); a++, Scanned++)
	{
		const float Ang = (float)((g_DuScanStart + a) % SWEEP) * DU_PI / 180.0f;
		float Near = 1e9f;
		Try(Ang, &Near);
		if(Near < 150.0f)
			Push(Near, Ang);
	}
	g_DuScanStart = (g_DuScanStart + std::max(1, Scanned)) % SWEEP;
	std::sort(aNear, aNear + NumNear, [](const SNear &A, const SNear &B) { return A.m_Near < B.m_Near; });
	for(int n = 0; n < NumNear && n < 6 && !OverBudget(); n++)
	{
		const float Seed = aNear[n].m_Angle;
		const float Before = Best.m_Score;
		for(int k = -6; k <= 6; k++)
			Try(Seed + (float)k * 0.25f * DU_PI / 180.0f, nullptr);
		if(Best.m_Found && Best.m_Score > Before)
		{
			const float Fine = Best.m_Angle;
			for(int k = -3; k <= 3; k++)
				Try(Fine + (float)k * 0.1f * DU_PI / 180.0f, nullptr);
		}
	}
	if(Best.m_Found)
	{
		g_DuWarmAngle = DuNormAngle(Best.m_Angle);
		g_DuWarmTick = 0;
	}
	else if(NumNear > 0)
	{
		g_DuWarmAngle = DuNormAngle(aNear[0].m_Angle);
		g_DuWarmTick = 0;
	}
	return Best;
}
} // namespace

void VertexDummyUnfreeze(CGameClient *pGame, IClient *pClient, CNetObj_PlayerInput *pSlot, CNetObj_PlayerInput *pDummyCore, const CNetObj_PlayerInput &MyInput)
{
	SDuState &St = g_DuState;
	const bool Shoots = g_Config.m_ClVertexDummyUnfreeze != 0;
	const bool Esp = g_Config.m_VxDuEsp != 0;
	if(!Shoots && !Esp)
		return;
	const int LocalId = pGame->m_Snap.m_LocalClientId;
	const int DummyId = pGame->m_aLocalIds[!g_Config.m_ClDummy]; 
	if(LocalId < 0 || DummyId < 0 || DummyId >= MAX_CLIENTS || DummyId == LocalId)
		return;
	const int GameTick = pClient->GameTick(g_Config.m_ClDummy);
	if(GameTick < St.m_LastTick - 50) 
		St = SDuState();
	St.m_LastTick = GameTick;

	CCharacter *pDum = pGame->m_PredictedWorld.GetCharacterById(DummyId);
	if(!pDum)
		return;
	const bool DummyFrozen = pDum->m_FreezeTime > 0;
	const bool HasLaser = pDum->Core()->m_aWeapons[WEAPON_LASER].m_Got && pDum->Core()->m_aWeapons[WEAPON_LASER].m_Ammo != 0;
	const bool LaserActive = pDum->Core()->m_ActiveWeapon == WEAPON_LASER;
	const bool Debug = g_Config.m_ClVertexUnfreezeDebug != 0;
	const bool DoLog = Debug && GameTick - St.m_LogTick >= 25;
	if(DoLog)
		St.m_LogTick = GameTick;

	int aTargets[DU_MAXT];
	int NT = 0;
	aTargets[NT++] = LocalId;
	if(Shoots)
	{
		if(GameTick - St.m_NamesTick >= 25 || str_comp(St.m_aNamesCopy, g_Config.m_VxDuNames) != 0)
		{
			St.m_NamesTick = GameTick;
			str_copy(St.m_aNamesCopy, g_Config.m_VxDuNames, sizeof(St.m_aNamesCopy));
			St.m_NumNames = 0;
			const char *pL = St.m_aNamesCopy;
			while(*pL && St.m_NumNames < DU_MAXT - 1)
			{
				char aName[32];
				int n = 0;
				while(*pL && *pL != ',' && *pL != ';')
				{
					if(n < 31)
						aName[n++] = *pL;
					pL++;
				}
				if(*pL)
					pL++;
				aName[n] = 0;
				while(n > 0 && aName[n - 1] == ' ')
					aName[--n] = 0;
				int Skip = 0;
				while(Skip < n && aName[Skip] == ' ')
					Skip++;
				if(n - Skip <= 0)
					continue;
				for(int c = 0; c < MAX_CLIENTS; c++)
				{
					if(c == LocalId || c == DummyId || !pGame->m_aClients[c].m_Active)
						continue;
					if(str_find_nocase(pGame->m_aClients[c].m_aName, aName + Skip) != nullptr)
					{
						St.m_aNameIds[St.m_NumNames++] = c;
						break;
					}
				}
			}
		}
		for(int i = 0; i < St.m_NumNames && NT < DU_MAXT; i++)
			if(St.m_aNameIds[i] != LocalId && St.m_aNameIds[i] != DummyId && pGame->m_aClients[St.m_aNameIds[i]].m_Active)
				aTargets[NT++] = St.m_aNameIds[i];
	}

	const int64_t T0 = time_get();
	auto Elapsed = [&]() -> double { return (double)(time_get() - T0) * 1000.0 / (double)time_freq(); };

	const CTuningParams &Tuning = pGame->m_aTuning[g_Config.m_ClDummy];
	float Reach = Tuning.m_LaserReach;
	if(Reach <= 1.0f)
		Reach = 800.0f;
	const int BounceDelay = (int)(pClient->GameTickSpeed() * (float)Tuning.m_LaserBounceDelay / 1000.0f) + 1;
	const int MaxBounces = Tuning.m_LaserBounceNum;
	const int SimLenShot = std::min(36, MaxBounces * BounceDelay + 4); 

	bool Shoot = false;
	vec2 AimVec(0.0f, 0.0f);
	bool WantLaser = false;

	for(int Ti = 0; Ti < NT; Ti++)
	{
		const int TargetId = aTargets[Ti];
		const bool IsMe = Ti == 0;
		CCharacter *pT = pGame->m_PredictedWorld.GetCharacterById(TargetId);
		if(!pT || pT->Core()->m_DeepFrozen)
			continue;
		int FreezeLeft = pT->m_FreezeTime;
		if(!IsMe)
			FreezeLeft = std::max(FreezeLeft, pGame->m_aClients[TargetId].m_FreezeEnd - GameTick);
		const bool Frozen = FreezeLeft > 0;
		if(DoLog)
			dbg_msg("dummy-unfreeze", "target %d(%s) frozen=%d left=%d dist=%.0f shooter: laser=%d active=%d frozen=%d", TargetId, pGame->m_aClients[TargetId].m_aName, (int)Frozen, FreezeLeft, distance(pDum->m_Pos, pT->m_Pos), (int)HasLaser, (int)LaserActive, (int)DummyFrozen);
		if(DummyFrozen || !HasLaser)
			continue;

		SDuCtx Ctx;
		Ctx.m_pCol = pT->Collision();
		Ctx.m_DummyPos = pDum->m_Pos;
		Ctx.m_Reach = Reach;
		Ctx.m_BounceCost = Tuning.m_LaserBounceCost;
		Ctx.m_MaxBounces = MaxBounces;
		Ctx.m_BounceDelay = BounceDelay;
		auto FillBlockers = [&]() {
			Ctx.m_NumBlk = 0;
			for(int i = 0; i < MAX_CLIENTS; i++)
			{
				if(i == DummyId || i == TargetId)
					continue;
				CCharacter *pO = pGame->m_PredictedWorld.GetCharacterById(i);
				if(!pO || distance(pO->m_Pos, Ctx.m_DummyPos) > 900.0f)
					continue;
				Ctx.m_aBlk[Ctx.m_NumBlk++] = pO->m_Pos + pO->Core()->m_Vel * 2.0f;
			}
		};
		const CNetObj_PlayerInput *pTargetInput = IsMe ? &MyInput : nullptr;

		if(Frozen)
		{
			if(Shoots && !LaserActive)
				WantLaser = true; 
			if(distance(Ctx.m_DummyPos, pT->m_Pos) > Reach + 40.0f)
				continue;
			if(length(pT->Core()->m_Vel) < 0.02f && DuOnFreeze(Ctx.m_pCol, pT->m_Pos) && Ctx.m_pCol->CheckPoint(pT->m_Pos.x, pT->m_Pos.y + 20.0f))
				continue;

			const bool CanShootNow = Shoots && LaserActive && !Shoot && GameTick - St.m_FireTick >= DU_GAP && GameTick >= St.m_aRetry[Ti] && FreezeLeft > DU_MIN_GAIN;
			if(CanShootNow && Elapsed() < DU_SHOT_BUDGET_MS)
			{
				FillBlockers();
				SDuSim OwnPath;
				if(DuSimulate(pGame, Ctx.m_pCol, DummyId, DummyId, pSlot, 0, 0, std::min(DU_MAXLEN, MaxBounces * BounceDelay + 6), 0, OwnPath))
					Ctx.m_pOwner = &OwnPath;
				SDuSim Sim;
				if(DuSimulate(pGame, Ctx.m_pCol, TargetId, DummyId, pTargetInput, IsMe ? 0 : FreezeLeft, 0, SimLenShot, 0, Sim))
				{
					const SDuHit Hit = DuFindShot(Ctx, Sim, 0, [&]() { return Elapsed() > DU_SHOT_BUDGET_MS; });
					if(DoLog)
						dbg_msg("dummy-unfreeze", "  shot search: found=%d idx=%d bounces=%d blockers=%d own=%d", (int)Hit.m_Found, Hit.m_Idx, Hit.m_Bounces, Ctx.m_NumBlk, Ctx.m_pOwner ? Ctx.m_pOwner->m_Len : -1);
					if(Hit.m_Found)
					{
						SDuSim After;
						int HitT = -1;
						const vec2 AimV = vec2(cosf(Hit.m_Angle) * DU_AIM_LEN, sinf(Hit.m_Angle) * DU_AIM_LEN);
						const bool Ok = DuVerifyShot(pGame, Ctx.m_pCol, TargetId, DummyId, pTargetInput, pSlot, IsMe ? 0 : FreezeLeft, AimV, 1, MaxBounces, BounceDelay, &HitT, &After);
						if(Ok)
						{
							Shoot = true;
							AimVec = AimV;
							if(IsMe && Esp)
							{
								g_VertexDuGhostPos = After.m_aP[HitT];
								g_VertexDuGhostUntil = GameTick + 6;
								g_VertexDuPathN = std::min(HitT + 1, 64);
								for(int i = 0; i < g_VertexDuPathN; i++)
									g_VertexDuPath[i] = After.m_aP[i];
							}
						}
						else
							St.m_aRetry[Ti] = GameTick + 3;
						if(DoLog)
							dbg_msg("dummy-unfreeze", "  verification: %s hitT=%d plan=%d b=%d", Ok ? "OK -> FIRE" : "rejected", HitT, Hit.m_Idx, Hit.m_Bounces);
					}
				}
			}
			if(!(IsMe && Esp))
				continue; 
		}

		if(!(Esp || Shoots) || (!IsMe && !Shoots) || GameTick < St.m_aForecastNext[Ti] || Elapsed() > DU_ESP_BUDGET_MS)
			continue;
		SDuSim Fc;
		if(!DuSimulate(pGame, Ctx.m_pCol, TargetId, DummyId, pTargetInput, 0, 0, 50, SimLenShot, Fc))
		{
			St.m_aForecastNext[Ti] = GameTick + 4;
			continue;
		}
		int Enter = -1;
		for(int s = 0; s <= Fc.m_Len; s++)
		{
			if(Fc.m_aF[s])
			{
				Enter = s;
				break;
			}
		}
		if(Enter < 0)
		{
			St.m_aForecastNext[Ti] = GameTick + 6; 
			continue;
		}
		St.m_aForecastNext[Ti] = GameTick + (Enter > 36 ? 4 : (Enter > 30 ? 2 : 1)); 
		if(Shoots && LaserActive && !Shoot && Enter <= 30 && GameTick - St.m_FireTick >= DU_GAP && GameTick >= St.m_aRetry[Ti])
		{
				FillBlockers();
				SDuSim OwnPath;
				if(DuSimulate(pGame, Ctx.m_pCol, DummyId, DummyId, pSlot, 0, 0, std::min(DU_MAXLEN, MaxBounces * BounceDelay + 6), 0, OwnPath))
					Ctx.m_pOwner = &OwnPath;
				const SDuHit PH = DuFindShot(Ctx, Fc, 30, [&]() { return Elapsed() > DU_SHOT_BUDGET_MS; });
			if(PH.m_Found)
			{
				const int PlanTick = GameTick + PH.m_T;
				if(St.m_PreTick == GameTick - 1 && std::abs(PlanTick - St.m_PrePlan) <= 1 && fabsf(PH.m_Angle - St.m_PreAngle) < 3.0f * DU_PI / 180.0f)
					St.m_PreCount++;
				else
					St.m_PreCount = 1;
				St.m_PreTick = GameTick;
				St.m_PreAngle = PH.m_Angle;
				St.m_PrePlan = PlanTick;
				if(PH.m_T == 0 && ((PH.m_Bounces == 0 && St.m_PreCount >= 2) || St.m_PreCount >= 4))
				{
				SDuSim After;
				int HitT = -1;
				const vec2 AimV = vec2(cosf(PH.m_Angle) * DU_AIM_LEN, sinf(PH.m_Angle) * DU_AIM_LEN);
				const bool Ok = DuVerifyShot(pGame, Ctx.m_pCol, TargetId, DummyId, pTargetInput, pSlot, 0, AimV, PH.m_T + 1, MaxBounces, BounceDelay, &HitT, &After);
				if(Ok)
				{
					Shoot = true;
					AimVec = AimV;
					if(Esp && IsMe)
					{
						g_VertexDuGhostPos = After.m_aP[HitT];
						g_VertexDuGhostUntil = GameTick + 6;
					}
				}
				else
					St.m_aRetry[Ti] = GameTick + 2;
				if(DoLog)
					dbg_msg("dummy-unfreeze", "  pre-freeze shot: enter=%d idx=%d b=%d hitT=%d -> %s", Enter, PH.m_Idx, PH.m_Bounces, HitT, Ok ? "FIRE" : "rejected");
				}
			}
		}
		if(Shoots && !LaserActive && Enter <= 45)
			WantLaser = true;
		if(Esp && IsMe)
		{
			FillBlockers();
			const SDuHit Hit = DuFindShot(Ctx, Fc, 30, [&]() { return Elapsed() > DU_ESP_BUDGET_MS; });
			if(Hit.m_Found)
			{
				g_VertexDuGhostPos = Fc.m_aP[Hit.m_Idx];
				g_VertexDuGhostUntil = GameTick + 6;
				g_VertexDuPathN = std::min(Hit.m_Idx + 1, 64);
				const int Stride = std::max(1, (Hit.m_Idx + 64) / 64);
				g_VertexDuPathN = 0;
				for(int i = 0; i <= Hit.m_Idx && g_VertexDuPathN < 63; i += Stride)
					g_VertexDuPath[g_VertexDuPathN++] = Fc.m_aP[i];
				g_VertexDuPath[g_VertexDuPathN++] = Fc.m_aP[Hit.m_Idx];
			}
			if(DoLog)
				dbg_msg("dummy-unfreeze", "  forecast: enter=%d found=%d idx=%d", Enter, (int)Hit.m_Found, Hit.m_Idx);
		}
	}

	if(!Shoots)
		return;
	if(WantLaser && !LaserActive)
	{
		pSlot->m_WantedWeapon = WEAPON_LASER + 1;
		pDummyCore->m_WantedWeapon = WEAPON_LASER + 1;
	}
	bool Fire = false;
	if(Shoot)
	{
		Fire = true;
		St.m_FireTick = GameTick;
		St.m_Aim = AimVec;
	}
	if(GameTick == St.m_FireTick)
		Fire = true; 
	if(Fire)
	{
		pSlot->m_TargetX = (int)St.m_Aim.x;
		pSlot->m_TargetY = (int)St.m_Aim.y;
		pDummyCore->m_TargetX = (int)St.m_Aim.x;
		pDummyCore->m_TargetY = (int)St.m_Aim.y;
	}
	if(Fire || St.m_FireHeld)
	{
		pSlot->m_Fire = Fire ? 1 : 0;
		pDummyCore->m_Fire = Fire ? 1 : 0;
	}
	St.m_FireHeld = Fire;
}

void CControls::VertexUpdateUnfreeze()
{
        constexpr int HORIZON = 40; 
        constexpr int MAX_ENTER = 16; 
        constexpr int LOOKAHEAD = 6; 
        constexpr int COMMIT_TICKS = 5; 
        constexpr int FIRE_ENTER_MAX = 6; 
        constexpr int MIN_HIT_AFTER_LEAVE = 2; 
        constexpr int URGENT_ENTER = 2; 
        constexpr int INPUT_QUIET = 2; 
        constexpr int STABLE_TICKS = 2; 
        constexpr int MAX_LATE_HIT = 3; 
        constexpr float KEEP_RADIUS = 600.0f; 
        constexpr int MAX_VERIFY = 4; 
        constexpr int MIN_GAIN = 8; 
        constexpr float PHYS_SIZE = 28.0f; 
        constexpr float HIT_MARGIN = 4.0f; 
        constexpr float WAIT_TOLERANCE = 60.0f; 
        constexpr float AIM_LEN = 2000.0f; 
        constexpr float PI = 3.14159265358979323846f;
        constexpr float DISTRUST_PX = 1.0f; 
        constexpr int DISTRUST_TICKS = 6; 
        constexpr int BUDGET_MS = 3; 
        constexpr int MIN_FIRE_GAP = 3; 
        constexpr int REQUIRED_VARIANTS = (1 << 1) | (1 << 2) | (1 << 3);

        CGameClient *pGame = GameClient();
        if(!pGame)
                return;

        m_LaserUnfreezeAimActive = false;

        static int s_FireReleaseTick = -1;
        static int s_LastEvalTick = -1000000;
        static int s_PrevEnterAbs = 0;
        static int s_Stable = 0;
        static int s_LastFireTick = -1000000;
        static int s_LastDir = 0, s_LastJump = 0, s_LastHook = 0;
        static int s_LastInputChangeTick = -1000000;
        static vec2 s_PrevForecast1 = vec2(0.0f, 0.0f);
        static int s_PrevForecastTick = -1;
        static int s_DistrustUntil = 0;
        static int s_LastDummy = -1;
        static int s_ExpectHitAbs = 0;
        static int s_ExpectEnterAbs = 0;
        static int s_SkipUntil = -1000000; 
        static int s_LastWorkTick = -1000000;

        auto ResetState = []() {
                s_FireReleaseTick = -1;
                s_LastWorkTick = -1000000;
                s_LastEvalTick = -1000000;
                s_PrevEnterAbs = 0;
                s_Stable = 0;
                s_LastFireTick = -1000000;
                s_LastInputChangeTick = -1000000;
                s_PrevForecastTick = -1;
                s_DistrustUntil = 0;
        };

        if(s_LastDummy != g_Config.m_ClDummy)
        {
                ResetState();
                s_LastDummy = g_Config.m_ClDummy;
        }

        CNetObj_PlayerInput *pInput = &m_aInputData[g_Config.m_ClDummy];
        const int NowTick = pGame->m_PredictedWorld.GameTick();

        if(s_FireReleaseTick >= 0)
        {
                if(NowTick >= s_FireReleaseTick || NowTick < s_FireReleaseTick - 5)
                {
                        if(pInput->m_Fire & 1)
                                pInput->m_Fire++;
                        s_FireReleaseTick = -1;
                }
                else
                        m_LaserUnfreezeAimActive = true;
                return;
        }

        if(!g_Config.m_ClVertexUnfreezeBot)
        {
                s_Stable = 0;
                return;
        }
        if(Client()->State() != IClient::STATE_ONLINE)
                return;

        const int LocalId = pGame->m_Snap.m_LocalClientId;
        if(LocalId < 0 || pGame->m_Snap.m_SpecInfo.m_Active)
                return;

        CCharacter *pPredChar = pGame->m_PredictedWorld.GetCharacterById(LocalId);
        if(!pPredChar)
                return;
        if(s_LastWorkTick == NowTick)
                return;
        s_LastWorkTick = NowTick;
        CCollision *pCol = pPredChar->Collision();

        {
                static bool s_WasFrozen = false;
                static int s_FrozenSince = 0;
                const bool NowFrozen = pPredChar->m_FreezeTime > 0;
                if(NowFrozen && !s_WasFrozen)
                        s_FrozenSince = NowTick;
                if(!NowFrozen && s_WasFrozen && g_Config.m_ClVertexUnfreezeDebug)
                {
                        dbg_msg("unfreeze", "UNFROZEN: frozen for %d ticks, %d ticks after last shot, expectedHit=%d (real-expected=%d) (%s)",
                                NowTick - s_FrozenSince, NowTick - s_LastFireTick, s_ExpectHitAbs, NowTick - s_ExpectHitAbs,
                                (NowTick - s_FrozenSince) < 100 ? "shot worked or short freeze" : "timed out");
                }
                if(NowFrozen && !s_WasFrozen && g_Config.m_ClVertexUnfreezeDebug)
                        dbg_msg("unfreeze", "FROZEN: tick=%d predictedEnter=%d (real-expected=%d) lastFireTick=%d vel=(%.1f,%.1f) pos=(%.0f,%.0f)",
                                NowTick, s_ExpectEnterAbs, NowTick - s_ExpectEnterAbs, s_LastFireTick,
                                pPredChar->Core()->m_Vel.x, pPredChar->Core()->m_Vel.y, pPredChar->m_Pos.x, pPredChar->m_Pos.y);
                s_WasFrozen = NowFrozen;
        }
        if(pPredChar->m_FreezeTime > 0 || pPredChar->Core()->m_DeepFrozen)
        {
                s_Stable = 0;
                return;
        }
        static int s_LastGateLogTick = -1000000;
        auto GateLog = [&](const char *pWhy, int Value) {
                if(!g_Config.m_ClVertexUnfreezeDebug || NowTick - s_LastGateLogTick < 10)
                        return;
                s_LastGateLogTick = NowTick;
                dbg_msg("unfreeze", "[v17] gate: %s (%d)", pWhy, Value);
        };

        if(!pPredChar->Core()->m_aWeapons[WEAPON_LASER].m_Got || pPredChar->Core()->m_aWeapons[WEAPON_LASER].m_Ammo == 0)
        {
                GateLog("no laser or no ammo", 0);
                return;
        }
        constexpr float MIN_SPEED = 0.3f;
        if(length(pPredChar->Core()->m_Vel) < MIN_SPEED)
        {
                s_Stable = 0;
                return;
        }

        const int CurTick = NowTick;
        int64_t T0 = time_get();
        auto OverBudget = [&]() -> bool {
                return (time_get() - T0) * 1000 / time_freq() > BUDGET_MS;
        };

        if(CurTick + 50 < s_LastEvalTick)
                ResetState();

        if(CurTick >= s_LastFireTick && CurTick - s_LastFireTick < MIN_FIRE_GAP)
                return;

        if(s_PrevForecastTick == CurTick - 1 && distance(s_PrevForecast1, pPredChar->m_Pos) > DISTRUST_PX)
        {
                s_DistrustUntil = CurTick + DISTRUST_TICKS;
                s_Stable = 0;
        }

        const CTuningParams &Tuning = pGame->m_aTuning[g_Config.m_ClDummy];
        float LaserReach = Tuning.m_LaserReach;
        if(LaserReach <= 1.0f)
                LaserReach = 800.0f;
        const float BounceCost = Tuning.m_LaserBounceCost;
        const int MaxBounces = Tuning.m_LaserBounceNum;
        const int BounceDelay = (int)(Client()->GameTickSpeed() * (float)Tuning.m_LaserBounceDelay / 1000.0f) + 1;

        CNetObj_PlayerInput BaseInput = *pInput;
        BaseInput.m_Direction = 0;
        if(m_aInputDirectionLeft[g_Config.m_ClDummy] && !m_aInputDirectionRight[g_Config.m_ClDummy])
                BaseInput.m_Direction = -1;
        if(!m_aInputDirectionLeft[g_Config.m_ClDummy] && m_aInputDirectionRight[g_Config.m_ClDummy])
                BaseInput.m_Direction = 1;
        if(BaseInput.m_Direction != s_LastDir || BaseInput.m_Jump != s_LastJump || BaseInput.m_Hook != s_LastHook)
        {
                s_LastInputChangeTick = CurTick;
                s_Stable = 0;
        }
        s_LastDir = BaseInput.m_Direction;
        s_LastJump = BaseInput.m_Jump;
        s_LastHook = BaseInput.m_Hook;
        CGameWorld SimWorld;
        int SimSteps = HORIZON; 

        auto Simulate = [&](int FireStep, int AimX, int AimY, vec2 *pPos, bool *pFrozen, int Variant = 0) -> bool {
                SimWorld.CopyWorld(&pGame->m_PredictedWorld);
                CCharacter *pC = SimWorld.GetCharacterById(LocalId);
                if(!pC)
                        return false;

                for(int i = 0; i < MAX_CLIENTS; i++)
                {
                        if(i == LocalId)
                                continue;
                        CCharacter *pOther = SimWorld.GetCharacterById(i);
                        if(!pOther)
                                continue;
                        const bool Linked = pC->Core()->HookedPlayer() == i || pOther->Core()->HookedPlayer() == LocalId;
                        if(!Linked && distance(pOther->m_Pos, pC->m_Pos) > KEEP_RADIUS)
                                delete pOther;
                }
                pC = SimWorld.GetCharacterById(LocalId);
                if(!pC)
                        return false;

                CNetObj_PlayerInput In = BaseInput;
                switch(Variant)
                {
                case 1:
                        In.m_Direction = 0;
                        break;
                case 2:
                        In.m_Hook = 0;
                        break;
                case 3:
                        In.m_Direction = -BaseInput.m_Direction;
                        break;
                case 4:
                        In.m_Jump = BaseInput.m_Jump ? 0 : 1;
                        break;
                case 5:
                        In.m_Hook = 1;
                        break;
                default:
                        break;
                }
                const int StartTick = SimWorld.GameTick();
                pPos[0] = pC->m_Pos;
                pFrozen[0] = pC->m_FreezeTime > 0;

                for(int s = 1; s <= SimSteps; s++)
                {
                        if(FireStep > 0)
                        {
                                if(s == 1)
                                        In.m_WantedWeapon = WEAPON_LASER + 1;
                                if(s == FireStep)
                                {
                                        In.m_Fire = (In.m_Fire & 1) ? In.m_Fire + 2 : In.m_Fire + 1;
                                        In.m_TargetX = AimX;
                                        In.m_TargetY = AimY;
                                }
                                else if(s == FireStep + 1)
                                {
                                        if(In.m_Fire & 1)
                                                In.m_Fire++;
                                        In.m_TargetX = BaseInput.m_TargetX;
                                        In.m_TargetY = BaseInput.m_TargetY;
                                }
                        }
                        pC->OnDirectInput(&In);
                        SimWorld.m_GameTick = StartTick + s;
                        pC->OnPredictedInput(&In);
                        SimWorld.Tick();
                        pC = SimWorld.GetCharacterById(LocalId);
                        if(!pC) 
                                return false;
                        if(pC->Core()->m_DeepFrozen) 
                                return false;
                        pPos[s] = pC->m_Pos;
                        pFrozen[s] = pC->m_FreezeTime > 0;
                }
                return true;
        };

        auto IsOnFreeze = [&](const vec2 &Pos) -> bool {
                const float R = PHYS_SIZE / 3.0f;
                const vec2 aPts[5] = {
                        Pos,
                        vec2(Pos.x + R, Pos.y - R),
                        vec2(Pos.x + R, Pos.y + R),
                        vec2(Pos.x - R, Pos.y - R),
                        vec2(Pos.x - R, Pos.y + R)};
                for(const vec2 &P : aPts)
                {
                        const int Idx = pCol->GetPureMapIndex(P);
                        if(Idx < 0)
                                continue;
                        if(pCol->GetTileIndex(Idx) == TILE_FREEZE || pCol->GetFrontTileIndex(Idx) == TILE_FREEZE)
                                return true;
                }
                return false;
        };

        if(CurTick < s_SkipUntil && CurTick + 4 > s_SkipUntil)
                return;
        vec2 aBasePos[HORIZON + 1];
        bool aBaseFrozen[HORIZON + 1];
        if(!Simulate(0, 0, 0, aBasePos, aBaseFrozen))
                return;

        s_PrevForecast1 = aBasePos[1];
        s_PrevForecastTick = CurTick;

        int Enter = -1;
        for(int s = 1; s <= HORIZON; s++)
        {
                if(aBaseFrozen[s])
                {
                        Enter = s;
                        break;
                }
        }
        if(Enter < 0 || Enter > MAX_ENTER)
        {
                s_Stable = 0;
                s_SkipUntil = CurTick + 2;
                return;
        }

        if(CurTick != s_LastEvalTick)
        {
                const int EnterAbs = CurTick + Enter;
                if(CurTick - s_LastEvalTick <= 2 && std::abs(EnterAbs - s_PrevEnterAbs) <= 1)
                        s_Stable++;
                else
                        s_Stable = 0;
                s_PrevEnterAbs = EnterAbs;
                s_LastEvalTick = CurTick;
        }

        int Leave = -1; 
        for(int s = Enter; s < HORIZON; s++)
        {
                if(!IsOnFreeze(aBasePos[s]) && !IsOnFreeze(aBasePos[s + 1]))
                {
                        Leave = s;
                        break;
                }
        }
        if(Leave < 0)
        {
                if(g_Config.m_ClVertexUnfreezeDebug)
                        dbg_msg("unfreeze", "gate: enter=%d no exit; pos@enter=(%.0f,%.0f) pos@enter+6=(%.0f,%.0f) pos@end=(%.0f,%.0f) endOnFreeze=%d",
                                Enter, aBasePos[Enter].x, aBasePos[Enter].y,
                                aBasePos[std::min(Enter + 6, HORIZON)].x, aBasePos[std::min(Enter + 6, HORIZON)].y,
                                aBasePos[HORIZON].x, aBasePos[HORIZON].y, (int)IsOnFreeze(aBasePos[HORIZON]));
                return;
        }

        const bool LaserActive = pPredChar->Core()->m_ActiveWeapon == WEAPON_LASER;
        if(!LaserActive && g_Config.m_ClVertexUnfreezeAuto)
                pInput->m_WantedWeapon = WEAPON_LASER + 1;
        if(!LaserActive)
        {
                if(g_Config.m_ClVertexUnfreezeDebug)
                        dbg_msg("unfreeze", "gate: enter=%d laser not active", Enter);
                return;
        }

        if(Enter > (s_Stable >= COMMIT_TICKS ? MAX_ENTER : FIRE_ENTER_MAX))
        {
                GateLog("too early, waiting", Enter);
                return;
        }

        const bool Urgent = Enter <= URGENT_ENTER;

        if(!Urgent)
        {
                if(s_Stable < STABLE_TICKS || CurTick - s_LastInputChangeTick < INPUT_QUIET)
                {
                        if(g_Config.m_ClVertexUnfreezeDebug)
                                dbg_msg("unfreeze", "gate: enter=%d leave=%d waiting stable=%d quiet=%d", Enter, Leave, s_Stable, CurTick - s_LastInputChangeTick);
                        return;
                }
                if(CurTick < s_DistrustUntil)
                {
                        if(g_Config.m_ClVertexUnfreezeDebug)
                                dbg_msg("unfreeze", "gate: enter=%d leave=%d distrust for %d more ticks", Enter, Leave, s_DistrustUntil - CurTick);
                        return;
                }
        }

        struct SCand
        {
                float Score;
                float Angle;
                int AimX, AimY;
                int FireStep;
                int HitIdx;
        };

        const int MinHitAfterLeave = Urgent ? 1 : MIN_HIT_AFTER_LEAVE;
        const int TargetIdx = Leave + MinHitAfterLeave;

        const vec2 FovMouse = m_aMousePos[g_Config.m_ClDummy];
        const float FovMouseLen = length(FovMouse);
        const int FovDeg = g_Config.m_ClVertexUnfreezeFov;
        const bool UseFov = FovDeg > 0 && FovDeg < 360 && FovMouseLen > 0.001f;
        const float FovCos = cosf((float)FovDeg * PI / 360.0f);

        vec2 aBlk[MAX_CLIENTS];
        int NumBlk = 0;
        for(int i = 0; i < MAX_CLIENTS; i++)
        {
                if(i == LocalId)
                        continue;
                CCharacter *pO = pGame->m_PredictedWorld.GetCharacterById(i);
                if(!pO || distance(pO->m_Pos, pPredChar->m_Pos) > 700.0f)
                        continue;
                aBlk[NumBlk++] = pO->m_Pos + pO->Core()->m_Vel * 2.0f;
        }
        auto BeamCut = [&](const vec2 &A, const vec2 &B) -> bool {
                for(int k = 0; k < NumBlk; k++)
                {
                        vec2 C;
                        if(closest_point_on_line(A, B, aBlk[k], C) && distance(aBlk[k], C) < 32.0f)
                                return true;
                }
                return false;
        };

        int64_t ScanT0 = time_get();
        bool ScanPhase = true;
        auto ScanOverBudget = [&]() -> bool { return ScanPhase && (time_get() - ScanT0) * 1000 / time_freq() > BUDGET_MS; };
        auto ScanFirstOverBudget = [&]() -> bool { return ScanPhase && (time_get() - ScanT0) * 1000 / time_freq() > 1; };
        auto Trace = [&](int FireStep, float Angle, SCand *pOut) -> bool {
                if(ScanOverBudget())
                        return false;
                const int AimX = (int)roundf(cosf(Angle) * AIM_LEN);
                const int AimY = (int)roundf(sinf(Angle) * AIM_LEN);
                if(AimX == 0 && AimY == 0)
                        return false;

                if(UseFov)
                {
                        const float AimLen = sqrtf((float)AimX * (float)AimX + (float)AimY * (float)AimY);
                        const float Dot = ((float)AimX * FovMouse.x + (float)AimY * FovMouse.y) / (AimLen * FovMouseLen);
                        if(Dot < FovCos)
                                return false;
                }

                vec2 SegStart = aBasePos[FireStep - 1];
                vec2 SegDir = normalize(vec2((float)AimX, (float)AimY));
                float Energy = LaserReach;

                for(int b = 0; b <= MaxBounces && Energy > 1.0f; b++)
                {
                        const vec2 To = SegStart + SegDir * Energy;
                        vec2 ColTile, Before;
                        int TeleNr = 0;
                        const int Hit = pCol->IntersectLineTeleWeapon(SegStart, To, &ColTile, &Before, &TeleNr);
                        const vec2 SegEnd = Hit ? Before : To;

                        if(NumBlk > 0 && (b == 0 || !Hit) && BeamCut(SegStart, SegEnd))
                                return false;
                        if(b > 0)
                        {
                                const int Idx0 = FireStep + b * BounceDelay - 1;
                                if(Idx0 - 1 > HORIZON)
                                        break;

                                float BestDist = 0.0f;
                                int BestIdx = -1;
                                bool Robust = Idx0 >= Leave + MinHitAfterLeave && Idx0 <= HORIZON;
                                for(int Idx = Idx0 - 1; Robust && Idx <= Idx0 + 1; Idx++)
                                {
                                        if(Idx > HORIZON)
                                                continue;
                                        vec2 Closest;
                                        if(Idx < 1 || !aBaseFrozen[Idx] || !closest_point_on_line(SegStart, SegEnd, aBasePos[Idx], Closest))
                                        {
                                                Robust = false;
                                                break;
                                        }
                                        const float Dist = distance(aBasePos[Idx], Closest);
                                        BestDist = std::max(BestDist, Dist);
                                }
                                if(Robust)
                                        BestIdx = Idx0;
                                if(BestIdx >= 0 && BestDist < PHYS_SIZE - HIT_MARGIN)
                                {
                                        vec2 ClCut;
                                        if(NumBlk > 0 && closest_point_on_line(SegStart, SegEnd, aBasePos[BestIdx], ClCut) && BeamCut(SegStart, ClCut))
                                                return false;
                                        pOut->Score = 1000.0f - fabsf((float)(BestIdx - TargetIdx)) * 40.0f + (PHYS_SIZE - BestDist) * 3.0f - b * 10.0f;
                                        pOut->Angle = Angle;
                                        pOut->AimX = AimX;
                                        pOut->AimY = AimY;
                                        pOut->FireStep = FireStep;
                                        pOut->HitIdx = BestIdx;
                                        return true;
                                }
                        }

                        if(!Hit)
                                break;

                        vec2 Pos = SegEnd;
                        vec2 Vel = SegDir * 4.0f;
                        int Bounces = 0;
                        pCol->MovePoint(&Pos, &Vel, 1.0f, &Bounces);
                        Energy -= distance(SegStart, Pos) + BounceCost;
                        if(length(Vel) < 0.001f)
                                break;
                        SegDir = normalize(Vel);
                        SegStart = Pos;
                }
                return false;
        };

        const int NumAngles = std::clamp(g_Config.m_ClVertexUnfreezeAngles, 36, 360);
        static int s_ScanAngleStart = 0;
        std::vector<SCand> vNow;
        float BestLater = -1e30f;

        const int LastStep = std::min(1 + LOOKAHEAD, Enter);
        int FirstStepAnglesScanned = 0;
        ScanT0 = time_get();
        for(int f = 1; f <= LastStep && !ScanOverBudget(); f++)
        {
                for(int a = 0; a < NumAngles && !(f == 1 ? ScanFirstOverBudget() : ScanOverBudget()); a += (f == 1 ? 1 : 2))
                {
                        const int AngleIndex = (s_ScanAngleStart + a) % NumAngles;
                        if(f == 1)
                                ++FirstStepAnglesScanned;
                        SCand C;
                        if(!Trace(f, AngleIndex * 2.0f * PI / (float)NumAngles, &C))
                                continue;
                        if(Enter > 3 && C.HitIdx > Leave + MAX_LATE_HIT)
                                continue;
                        if(f == 1)
                                vNow.push_back(C);
                        else
                                BestLater = std::max(BestLater, C.Score);
                }
        }

        s_ScanAngleStart = (s_ScanAngleStart + std::max(1, FirstStepAnglesScanned)) % NumAngles;
        ScanPhase = false;
        T0 = time_get();
        if(g_Config.m_ClVertexUnfreezeDebug)
                dbg_msg("unfreeze", "enter=%d leave=%d now=%d laterBest=%.0f fov=%d", Enter, Leave, (int)vNow.size(), BestLater, FovDeg);

        if(vNow.empty())
                return;

        std::sort(vNow.begin(), vNow.end(), [](const SCand &A, const SCand &B) { return A.Score > B.Score; });

        if(BestLater > vNow[0].Score + WAIT_TOLERANCE && Enter > 6)
                return;

        SimSteps = std::min(HORIZON, Leave + 16);

        int WhyFail = 0;

        auto Verify = [&](const SCand &C, int Variant = 0, int Delay = 0) -> bool {
                if(OverBudget())
                {
                        WhyFail = 6;
                        return false;
                }
                vec2 aPos[HORIZON + 1];
                bool aFrozen[HORIZON + 1];
                if(!Simulate(C.FireStep + Delay, C.AimX, C.AimY, aPos, aFrozen, Variant))
                {
                        if(Variant == 0 && Delay == 0)
                                WhyFail = 1;
                        return false;
                }
                const int End = std::min(SimSteps, C.HitIdx + Delay + 12);
                if(aFrozen[End])
                {
                        if(Variant == 0 && Delay == 0)
                                WhyFail = 2;
                        return false;
                }
                if(Variant != 0)
                        return true; 
                int FrozenTicks = 0;
                int BaseTicks = 0;
                for(int s = 1; s <= End; s++)
                {
                        FrozenTicks += aFrozen[s] ? 1 : 0;
                        BaseTicks += aBaseFrozen[s] ? 1 : 0;
                }
                if(BaseTicks - FrozenTicks < MIN_GAIN)
                {
                        if(Delay == 0)
                                WhyFail = 3;
                        return false;
                }
                return true;
        };

        bool HaveFinal = false;
        SCand Final = {};
        int FinalVar = -1;

        const float RefineStep = 0.15f * PI / 180.0f;
        float aTried[MAX_VERIFY];
        int NumTried = 0;

        for(const SCand &Seed : vNow)
        {
                if(NumTried >= MAX_VERIFY || OverBudget())
                {
                        if(OverBudget() && !HaveFinal)
                                WhyFail = 6;
                        break;
                }

                bool Skip = false;
                for(int i = 0; i < NumTried; i++)
                {
                        float D = fabsf(Seed.Angle - aTried[i]);
                        if(D > PI)
                                D = 2.0f * PI - D;
                        if(D < 1.5f * PI / 180.0f)
                                Skip = true;
                }
                if(Skip)
                        continue;
                aTried[NumTried++] = Seed.Angle;

                SCand Best = Seed;
                for(int ri = -10; ri <= 10 && !OverBudget(); ri++)
                {
                        SCand C;
                        if(Trace(1, Seed.Angle + ri * RefineStep, &C) && C.Score > Best.Score)
                                Best = C;
                }

                auto WithAngle = [&](const SCand &Src, float Ang) -> SCand {
                        SCand R = Src;
                        R.Angle = Ang;
                        R.AimX = (int)roundf(cosf(Ang) * AIM_LEN);
                        R.AimY = (int)roundf(sinf(Ang) * AIM_LEN);
                        return R;
                };

                WhyFail = 0;
                bool Ok = false;
                bool HaveFallback = false;
                SCand Fallback = Best;
                static const int s_aOrder[] = {0, 1, -1, 2, -2, 3, -3};
                const float RobustStep = 0.1f * PI / 180.0f;
                for(const int Off : s_aOrder)
                {
                        if(OverBudget())
                        {
                                WhyFail = 6;
                                break;
                        }
                        const SCand C = WithAngle(Best, Best.Angle + Off * 0.2f * PI / 180.0f);
                        if(C.AimX == 0 && C.AimY == 0)
                                continue;
                        if(!Verify(C))
                                continue;
                        if(!HaveFallback)
                        {
                                Fallback = C;
                                HaveFallback = true;
                        }
                        if(Verify(C, 0, 1) && Verify(WithAngle(C, C.Angle - RobustStep)) && Verify(WithAngle(C, C.Angle + RobustStep)))
                        {
                                Best = C;
                                Ok = true;
                                break;
                        }
                }
                if(!Ok && HaveFallback && Urgent)
                {
                        Best = Fallback;
                        Ok = true;
                        if(g_Config.m_ClVertexUnfreezeDebug)
                                dbg_msg("unfreeze", "last-chance fallback angle used (enter=%d)", Enter);
                }
                if(!Ok)
                {
                        if(g_Config.m_ClVertexUnfreezeDebug)
                                dbg_msg("unfreeze", "verify failed: angle=%.1f hit=%d why=%d", Best.Angle * 180.0f / PI, Best.HitIdx, WhyFail == 6 ? 6 : (HaveFallback ? 4 : WhyFail));
                        continue;
                }

                int Var = 0;
                bool RequiredOk = true;
                for(int v = 1; v <= 5; v++)
                {
                        if((v == 1 || v == 3) && BaseInput.m_Direction == 0)
                                continue;
                        if(v == 2 && BaseInput.m_Hook == 0)
                                continue;
                        if(v == 5 && BaseInput.m_Hook != 0)
                                continue;
                        if(OverBudget())
                        {
                                RequiredOk = false;
                                WhyFail = 6;
                                break;
                        }
                        if(Verify(Best, v))
                                Var++;
                        else if(REQUIRED_VARIANTS & (1 << v))
                                RequiredOk = false;
                }
                if(!RequiredOk && !Urgent)
                {
                        if(g_Config.m_ClVertexUnfreezeDebug)
                                dbg_msg("unfreeze", "scenario check failed: angle=%.1f hit=%d why=%d", Best.Angle * 180.0f / PI, Best.HitIdx, WhyFail == 6 ? 6 : 5);
                        continue;
                }

                if(!HaveFinal || Var > FinalVar || (Var == FinalVar && Best.Score > Final.Score))
                {
                        Final = Best;
                        FinalVar = Var;
                        HaveFinal = true;
                }
        }

        if(!HaveFinal)
                return;

        {
                const SCand &Best = Final;
                const vec2 AimOffset((float)Best.AimX, (float)Best.AimY);
                m_LaserUnfreezeAimOffset = AimOffset;
                m_LaserUnfreezeAimActive = true;

                pInput->m_Fire = (pInput->m_Fire & 1) ? pInput->m_Fire + 2 : pInput->m_Fire + 1;
                s_FireReleaseTick = CurTick + 1;
                s_LastFireTick = CurTick;
                s_ExpectHitAbs = CurTick + Best.HitIdx;
                s_ExpectEnterAbs = CurTick + Enter;
                s_Stable = 0;

                if(g_Config.m_ClVertexUnfreezeDebug)
                {
                        float Dev = fabsf(Best.Angle - atan2f(FovMouse.y, FovMouse.x));
                        if(Dev > PI)
                                Dev = 2.0f * PI - Dev;
                        dbg_msg("unfreeze", "[v17] FIRE! angle=%.1f dev_from_cursor=%.1f fov=%d hit=%d enter=%d leave=%d score=%.0f scenarios=%d time=%lldus",
                                Best.Angle * 180.0f / PI, Dev * 180.0f / PI, FovDeg, Best.HitIdx, Enter, Leave, Best.Score, FinalVar,
                                (long long)((time_get() - T0) * 1000000 / time_freq()));
                }
        }
}

CControls::CControls()
{
        mem_zero(&m_aLastData, sizeof(m_aLastData));
        std::fill(std::begin(m_aMousePos), std::end(m_aMousePos), vec2(0.0f, 0.0f));
        std::fill(std::begin(m_aMousePosOnAction), std::end(m_aMousePosOnAction), vec2(0.0f, 0.0f));
        std::fill(std::begin(m_aTargetPos), std::end(m_aTargetPos), vec2(0.0f, 0.0f));
        std::fill(std::begin(m_aMouseInputType), std::end(m_aMouseInputType), EMouseInputType::ABSOLUTE);
}

void CControls::OnReset()
{
        ResetInput(0);
        ResetInput(1);
        std::fill(std::begin(m_aAmmoCount), std::end(m_aAmmoCount), 0);
        m_LastSendTime = 0;
        GameClient()->m_AvoidFreeze.Reset();
        VertexAutoAled().Reset();
}

void CControls::ResetInput(int Dummy)
{
		// === vertex glue (step2): reset physical key tracking ===
		m_aPhysicalHook[Dummy] = 0;
		m_PhysicalJump = 0;
		m_ManualJumpConsumed = false;
        m_aLastData[Dummy].m_Direction = 0;
        if((m_aLastData[Dummy].m_Fire & 1) != 0)
                m_aLastData[Dummy].m_Fire++;
        m_aLastData[Dummy].m_Fire &= INPUT_STATE_MASK;
        m_aLastData[Dummy].m_Jump = 0;
        m_aInputData[Dummy] = m_aLastData[Dummy];
        m_aInputDirectionLeft[Dummy] = 0;
        m_aInputDirectionRight[Dummy] = 0;
}

void CControls::OnPlayerDeath()
{
        std::fill(std::begin(m_aAmmoCount), std::end(m_aAmmoCount), 0);
}

struct CInputState
{
        CControls *m_pControls;
        int *m_apVariables[NUM_DUMMIES];
};

void CControls::ConKeyInputState(IConsole::IResult *pResult, void *pUserData)
{
        CInputState *pState = (CInputState *)pUserData;
	// === vertex glue (step2b): physical jump tracking ===
	const int Stroke = pResult->GetInteger(0);
	if(pState->m_apVariables[g_Config.m_ClDummy] == &pState->m_pControls->m_aInputData[g_Config.m_ClDummy].m_Jump)
	{
		if(!Stroke || !pState->m_pControls->m_PhysicalJump)
			pState->m_pControls->m_ManualJumpConsumed = false;
		pState->m_pControls->m_PhysicalJump = Stroke;
	}
        if(pState->m_pControls->GameClient()->m_GameInfo.m_BugDDRaceInput && pState->m_pControls->GameClient()->m_Snap.m_SpecInfo.m_Active)
                return;
	*pState->m_apVariables[g_Config.m_ClDummy] = Stroke; // === vertex glue (step2b) ===
	// === vertex glue (step2b): physical hook tracking ===
	if(pState->m_apVariables[g_Config.m_ClDummy] == &pState->m_pControls->m_aInputData[g_Config.m_ClDummy].m_Hook)
		pState->m_pControls->m_aPhysicalHook[g_Config.m_ClDummy] = Stroke;
}

void CControls::ConKeyInputCounter(IConsole::IResult *pResult, void *pUserData)
{
        CInputState *pState = (CInputState *)pUserData;
        if((pState->m_pControls->GameClient()->m_GameInfo.m_BugDDRaceInput && pState->m_pControls->GameClient()->m_Snap.m_SpecInfo.m_Active) || pState->m_pControls->GameClient()->m_Spectator.IsActive())
                return;
        int *pVariable = pState->m_apVariables[g_Config.m_ClDummy];
        if(((*pVariable) & 1) != pResult->GetInteger(0))
                (*pVariable)++;
        *pVariable &= INPUT_STATE_MASK;
}

struct CInputSet
{
        CControls *m_pControls;
        int *m_apVariables[NUM_DUMMIES];
        int m_Value;
};

void CControls::ConKeyInputSet(IConsole::IResult *pResult, void *pUserData)
{
        CInputSet *pSet = (CInputSet *)pUserData;
        if(pResult->GetInteger(0))
                *pSet->m_apVariables[g_Config.m_ClDummy] = pSet->m_Value;
}

void CControls::ConKeyInputNextPrevWeapon(IConsole::IResult *pResult, void *pUserData)
{
        CInputSet *pSet = (CInputSet *)pUserData;
        ConKeyInputCounter(pResult, pSet);
        pSet->m_pControls->m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = 0;
}

void CControls::OnConsoleInit()
{
        {
                static CInputState s_State = {this, {&m_aInputDirectionLeft[0], &m_aInputDirectionLeft[1]}};
                Console()->Register("+left", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Move left");
        }
        {
                static CInputState s_State = {this, {&m_aInputDirectionRight[0], &m_aInputDirectionRight[1]}};
                Console()->Register("+right", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Move right");
        }
        {
                static CInputState s_State = {this, {&m_aInputData[0].m_Jump, &m_aInputData[1].m_Jump}};
                Console()->Register("+jump", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Jump");
        }
        {
                static CInputState s_State = {this, {&m_aInputData[0].m_Hook, &m_aInputData[1].m_Hook}};
                Console()->Register("+hook", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Hook");
        }
        {
                static CInputState s_State = {this, {&m_aInputData[0].m_Fire, &m_aInputData[1].m_Fire}};
                Console()->Register("+fire", "", CFGFLAG_CLIENT, ConKeyInputCounter, &s_State, "Fire");
        }
        {
                static CInputState s_State = {this, {&m_aShowHookColl[0], &m_aShowHookColl[1]}};
                Console()->Register("+showhookcoll", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Show Hook Collision");
        }
        {
                static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 1};
                Console()->Register("+weapon1", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to hammer");
        }
        {
                static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 2};
                Console()->Register("+weapon2", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to gun");
        }
        {
                static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 3};
                Console()->Register("+weapon3", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to shotgun");
        }
        {
                static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 4};
                Console()->Register("+weapon4", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to grenade");
        }
        {
                static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 5};
                Console()->Register("+weapon5", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to laser");
        }
        {
                static CInputSet s_Set = {this, {&m_aInputData[0].m_NextWeapon, &m_aInputData[1].m_NextWeapon}, 0};
                Console()->Register("+nextweapon", "", CFGFLAG_CLIENT, ConKeyInputNextPrevWeapon, &s_Set, "Switch to next weapon");
        }
        {
                static CInputSet s_Set = {this, {&m_aInputData[0].m_PrevWeapon, &m_aInputData[1].m_PrevWeapon}, 0};
                Console()->Register("+prevweapon", "", CFGFLAG_CLIENT, ConKeyInputNextPrevWeapon, &s_Set, "Switch to previous weapon");
        }
}

void CControls::OnMessage(int Msg, void *pRawMsg)
{
        if(Msg == NETMSGTYPE_SV_WEAPONPICKUP)
        {
                CNetMsg_Sv_WeaponPickup *pMsg = (CNetMsg_Sv_WeaponPickup *)pRawMsg;
                if(g_Config.m_ClAutoswitchWeapons)
                        m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = pMsg->m_Weapon + 1;
                m_aAmmoCount[std::max(0, pMsg->m_Weapon % NUM_WEAPONS)] = 10;
        }
}

int CControls::SnapInput(int *pData)
{
        if(GameClient()->m_Chat.IsActive())
                m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_CHATTING;
        else if(GameClient()->m_Menus.IsActive())
                m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_IN_MENU;
        else
                m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_PLAYING;

        if(GameClient()->m_Scoreboard.IsActive())
                m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_SCOREBOARD;

        if(Client()->ServerCapAnyPlayerFlag() && GameClient()->m_Controls.m_aShowHookColl[g_Config.m_ClDummy])
                m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_AIM;

        if(Client()->ServerCapAnyPlayerFlag() && GameClient()->m_Camera.CamType() == CCamera::CAMTYPE_SPEC)
                m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_SPEC_CAM;

        switch(m_aMouseInputType[g_Config.m_ClDummy])
        {
        case CControls::EMouseInputType::AUTOMATED:
                m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_INPUT_ABSOLUTE;
                break;
        case CControls::EMouseInputType::ABSOLUTE:
                m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_INPUT_ABSOLUTE | PLAYERFLAG_INPUT_MANUAL;
                break;
        case CControls::EMouseInputType::RELATIVE:
                m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_INPUT_MANUAL;
                break;
        }

        bool Send = m_aLastData[g_Config.m_ClDummy].m_PlayerFlags != m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;

        m_aLastData[g_Config.m_ClDummy].m_PlayerFlags = m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;

        if(!(m_aInputData[g_Config.m_ClDummy].m_PlayerFlags & PLAYERFLAG_PLAYING))
        {
                if(!GameClient()->m_GameInfo.m_BugDDRaceInput)
                        ResetInput(g_Config.m_ClDummy);

                mem_copy(pData, &m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));

                m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)m_aMousePos[g_Config.m_ClDummy].x;
                m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)m_aMousePos[g_Config.m_ClDummy].y;

                Send = Send || time_get() > m_LastSendTime + time_freq();
        }
        else
        {
                m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)m_aMousePos[g_Config.m_ClDummy].x;
                m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)m_aMousePos[g_Config.m_ClDummy].y;

                if(g_Config.m_ClSubTickAiming && m_aMousePosOnAction[g_Config.m_ClDummy] != vec2(0.0f, 0.0f))
                {
                        m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)m_aMousePosOnAction[g_Config.m_ClDummy].x;
                        m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)m_aMousePosOnAction[g_Config.m_ClDummy].y;
                        m_aMousePosOnAction[g_Config.m_ClDummy] = vec2(0.0f, 0.0f);
                }

                VertexUpdateUnfreeze();

                m_vDebugRays.clear();

                if(m_LaserUnfreezeAimActive)
                {
                        m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)m_LaserUnfreezeAimOffset.x;
                        m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)m_LaserUnfreezeAimOffset.y;
                }

                if(!m_aInputData[g_Config.m_ClDummy].m_TargetX && !m_aInputData[g_Config.m_ClDummy].m_TargetY)
                {
                        m_aInputData[g_Config.m_ClDummy].m_TargetX = 1;
                        m_aMousePos[g_Config.m_ClDummy].x = 1;
                }

                m_aInputData[g_Config.m_ClDummy].m_Direction = 0;
                if(m_aInputDirectionLeft[g_Config.m_ClDummy] && !m_aInputDirectionRight[g_Config.m_ClDummy])
                        m_aInputData[g_Config.m_ClDummy].m_Direction = -1;
                if(!m_aInputDirectionLeft[g_Config.m_ClDummy] && m_aInputDirectionRight[g_Config.m_ClDummy])
                        m_aInputData[g_Config.m_ClDummy].m_Direction = 1;

                if(g_Config.m_ClDummyCopyMoves)
                {
                        CNetObj_PlayerInput *pDummyInput = &GameClient()->m_DummyInput;
                        if(!GameClient()->m_Snap.m_SpecInfo.m_Active || GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
                        {
                                pDummyInput->m_Direction = m_aInputData[g_Config.m_ClDummy].m_Direction;
                                pDummyInput->m_Hook = m_aInputData[g_Config.m_ClDummy].m_Hook;
                                pDummyInput->m_Jump = m_aInputData[g_Config.m_ClDummy].m_Jump;
                                pDummyInput->m_PlayerFlags = m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;
                                pDummyInput->m_TargetX = m_aInputData[g_Config.m_ClDummy].m_TargetX;
                                pDummyInput->m_TargetY = m_aInputData[g_Config.m_ClDummy].m_TargetY;
                                pDummyInput->m_WantedWeapon = m_aInputData[g_Config.m_ClDummy].m_WantedWeapon;
                                if(!g_Config.m_ClDummyControl)
                                        pDummyInput->m_Fire += m_aInputData[g_Config.m_ClDummy].m_Fire - m_aLastData[g_Config.m_ClDummy].m_Fire;
                                pDummyInput->m_NextWeapon += m_aInputData[g_Config.m_ClDummy].m_NextWeapon - m_aLastData[g_Config.m_ClDummy].m_NextWeapon;
                                pDummyInput->m_PrevWeapon += m_aInputData[g_Config.m_ClDummy].m_PrevWeapon - m_aLastData[g_Config.m_ClDummy].m_PrevWeapon;
                        }
                        m_aInputData[!g_Config.m_ClDummy] = *pDummyInput;
                }

                if(g_Config.m_ClDummyControl)
                {
                        CNetObj_PlayerInput *pDummyInput = &GameClient()->m_DummyInput;
                        pDummyInput->m_Jump = g_Config.m_ClDummyJump;
                        if(g_Config.m_ClDummyFire)
                                pDummyInput->m_Fire = g_Config.m_ClDummyFire;
                        else if((pDummyInput->m_Fire & 1) != 0)
                                pDummyInput->m_Fire++;
                        pDummyInput->m_Hook = g_Config.m_ClDummyHook;
                        m_aInputData[!g_Config.m_ClDummy] = *pDummyInput;
                }

                if(g_Config.m_ClVertexDummyUnfreeze || g_Config.m_VxDuEsp)
                        VertexDummyUnfreeze(GameClient(), Client(), &m_aInputData[!g_Config.m_ClDummy], &GameClient()->m_DummyInput, m_aInputData[g_Config.m_ClDummy]);
                
                GameClient()->m_AvoidFreeze.Update(GameClient(), Client(), &m_aInputData[g_Config.m_ClDummy]);
                VertexAutoAled().Update(GameClient(), Client(), &m_aInputData[g_Config.m_ClDummy]);
                
                // ВЫЗОВ АИМБОТА
                GameClient()->m_Aimbot.ApplyAimbot(&m_aInputData[g_Config.m_ClDummy], this, GameClient());

                if(g_Config.m_DbgStress)
                {
                        float t = Client()->LocalTime();
                        mem_zero(&m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));
                        m_aInputData[g_Config.m_ClDummy].m_Direction = ((int)t / 2) & 1;
                        m_aInputData[g_Config.m_ClDummy].m_Jump = ((int)t);
                        m_aInputData[g_Config.m_ClDummy].m_Fire = ((int)(t * 10));
                        m_aInputData[g_Config.m_ClDummy].m_Hook = ((int)(t * 2)) & 1;
                        m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = ((int)t) % NUM_WEAPONS;
                        m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)(std::sin(t * 3) * 100.0f);
                        m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)(std::cos(t * 3) * 100.0f);
                }

		// ===== OpenKrx Avoid (glue, step2) =====
		// Runs after all Vertex bots and before the send-detection compare,
		// so a rescue packet is never lost (KRX SnapInput order).
		m_aInputData[g_Config.m_ClDummy] = GameClient()->m_Avoid.AdjustInput(
			GameClient(), m_aInputData[g_Config.m_ClDummy],
			GameClient()->m_aLocalIds[g_Config.m_ClDummy], m_aMousePos[g_Config.m_ClDummy],
			m_aPhysicalHook[g_Config.m_ClDummy] != 0);
		m_aInputData[g_Config.m_ClDummy] = GameClient()->m_ClassicAvoid.AdjustInput(
			GameClient(), m_aInputData[g_Config.m_ClDummy],
			GameClient()->m_aLocalIds[g_Config.m_ClDummy]);
                Send = Send || m_aInputData[g_Config.m_ClDummy].m_Direction != m_aLastData[g_Config.m_ClDummy].m_Direction;
                Send = Send || m_aInputData[g_Config.m_ClDummy].m_Jump != m_aLastData[g_Config.m_ClDummy].m_Jump;
                Send = Send || m_aInputData[g_Config.m_ClDummy].m_Fire != m_aLastData[g_Config.m_ClDummy].m_Fire;
                Send = Send || m_aInputData[g_Config.m_ClDummy].m_Hook != m_aLastData[g_Config.m_ClDummy].m_Hook;
                Send = Send || m_aInputData[g_Config.m_ClDummy].m_WantedWeapon != m_aLastData[g_Config.m_ClDummy].m_WantedWeapon;
                Send = Send || m_aInputData[g_Config.m_ClDummy].m_NextWeapon != m_aLastData[g_Config.m_ClDummy].m_NextWeapon;
                Send = Send || m_aInputData[g_Config.m_ClDummy].m_PrevWeapon != m_aLastData[g_Config.m_ClDummy].m_PrevWeapon;
                Send = Send || time_get() > m_LastSendTime + time_freq() / 25;
                Send = Send || (GameClient()->m_Snap.m_pLocalCharacter && GameClient()->m_Snap.m_pLocalCharacter->m_Weapon == WEAPON_NINJA && (m_aInputData[g_Config.m_ClDummy].m_Direction || m_aInputData[g_Config.m_ClDummy].m_Jump || m_aInputData[g_Config.m_ClDummy].m_Hook));
        }

        m_aLastData[g_Config.m_ClDummy] = m_aInputData[g_Config.m_ClDummy];


        if(!Send)
                return 0;

        m_LastSendTime = time_get();
        mem_copy(pData, &m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));
        return sizeof(m_aInputData[0]);
}

void CControls::OnRender()
{
        if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
                return;

        GameClient()->m_Menus.VertexClickGuiTick();
        if(g_Config.m_VxClickGuiKey > 0 && !GameClient()->m_Chat.IsActive() && Input()->KeyPress(g_Config.m_VxClickGuiKey))
                GameClient()->m_Menus.VertexToggleClickGui();

        if(!GameClient()->m_Chat.IsActive() && !GameClient()->m_Menus.IsActive())
        {
                const auto &&ToggleBind = [&](int Key, auto *pValue, const char *pName) {
                        if(Key > 0 && Input()->KeyPress(Key))
                        {
                                *pValue = !*pValue;
                                char aBuf[64];
                                str_format(aBuf, sizeof(aBuf), "%s: %s", pName, *pValue ? "on" : "off");
                                GameClient()->Echo(aBuf);
                        }
                };
                ToggleBind(g_Config.m_VxBindAvoid, &g_Config.m_VxAfEnable, "Avoid");
                ToggleBind(g_Config.m_VxBindAim, &GameClient()->m_Aimbot.m_Enabled, "Aim");
                ToggleBind(g_Config.m_VxBindUnfreeze, &g_Config.m_ClVertexUnfreezeBot, "Auto Unfreeze");
                		if(g_Config.m_VxBindAled > 0 && Input()->KeyPress(g_Config.m_VxBindAled))
			g_Config.m_VxAledEnable = !g_Config.m_VxAledEnable;
        }

        GameClient()->m_AvoidFreeze.RenderBinds(GameClient());

        if(g_Config.m_ClAutoswitchWeaponsOutOfAmmo && !GameClient()->m_GameInfo.m_UnlimitedAmmo && GameClient()->m_Snap.m_pLocalCharacter)
        {
                m_aAmmoCount[std::max(0, GameClient()->m_Snap.m_pLocalCharacter->m_Weapon % NUM_WEAPONS)] = GameClient()->m_Snap.m_pLocalCharacter->m_AmmoCount;
                if(m_aInputData[g_Config.m_ClDummy].m_Fire % 2 != 0 &&
                        GameClient()->m_Snap.m_pLocalCharacter->m_AmmoCount == 0 &&
                        GameClient()->m_Snap.m_pLocalCharacter->m_Weapon != WEAPON_HAMMER &&
                        GameClient()->m_Snap.m_pLocalCharacter->m_Weapon != WEAPON_NINJA)
                {
                        int Weapon;
                        for(Weapon = WEAPON_LASER; Weapon > WEAPON_GUN; Weapon--)
                        {
                                if(Weapon == GameClient()->m_Snap.m_pLocalCharacter->m_Weapon)
                                        continue;
                                if(m_aAmmoCount[Weapon] > 0)
                                        break;
                        }
                        if(Weapon != GameClient()->m_Snap.m_pLocalCharacter->m_Weapon)
                                m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = Weapon + 1;
                }
        }

        if(GameClient()->m_Snap.m_pGameInfoObj && !GameClient()->m_Snap.m_SpecInfo.m_Active)
        {
                vec2 DyncamOffsetDelta = GameClient()->m_Camera.m_DyncamTargetCameraOffset - GameClient()->m_Camera.m_aDyncamCurrentCameraOffset[g_Config.m_ClDummy];
                float Zoom = GameClient()->m_Camera.m_Zoom;
                m_aTargetPos[g_Config.m_ClDummy] = GameClient()->m_LocalCharacterPos + m_aMousePos[g_Config.m_ClDummy] - DyncamOffsetDelta + DyncamOffsetDelta / Zoom;
        }
        else if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_UsePosition)
        {
                m_aTargetPos[g_Config.m_ClDummy] = GameClient()->m_Snap.m_SpecInfo.m_Position + m_aMousePos[g_Config.m_ClDummy];
        }
        else
        {
                m_aTargetPos[g_Config.m_ClDummy] = m_aMousePos[g_Config.m_ClDummy];
        }
}

bool CControls::OnCursorMove(float x, float y, IInput::ECursorType CursorType)
{
        if(GameClient()->IsWorldPaused())
                return false;

        if(CursorType == IInput::CURSOR_JOYSTICK && g_Config.m_InpControllerAbsolute && GameClient()->m_Snap.m_pGameInfoObj && !GameClient()->m_Snap.m_SpecInfo.m_Active)
        {
                vec2 AbsoluteDirection;
                if(Input()->GetActiveJoystick()->Absolute(&AbsoluteDirection.x, &AbsoluteDirection.y))
                {
                        m_aMousePos[g_Config.m_ClDummy] = AbsoluteDirection * GetMaxMouseDistance();
                        GameClient()->m_Controls.m_aMouseInputType[g_Config.m_ClDummy] = CControls::EMouseInputType::ABSOLUTE;
                }
                return true;
        }

        float Factor = 1.0f;
        if(g_Config.m_ClDyncam && g_Config.m_ClDyncamMousesens)
        {
                Factor = g_Config.m_ClDyncamMousesens / 100.0f;
        }
        else
        {
                switch(CursorType)
                {
                case IInput::CURSOR_MOUSE:
                        Factor = g_Config.m_InpMousesens / 100.0f;
                        break;
                case IInput::CURSOR_JOYSTICK:
                        Factor = g_Config.m_InpControllerSens / 100.0f;
                        break;
                default:
                        dbg_assert_failed("CControls::OnCursorMove CursorType %d", (int)CursorType);
                }
        }

        if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
                Factor *= GameClient()->m_Camera.m_Zoom;

        m_aMousePos[g_Config.m_ClDummy] += vec2(x, y) * Factor;
        GameClient()->m_Controls.m_aMouseInputType[g_Config.m_ClDummy] = CControls::EMouseInputType::RELATIVE;
        ClampMousePos();
        return true;
}

void CControls::ClampMousePos()
{
        if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
        {
                m_aMousePos[g_Config.m_ClDummy].x = std::clamp(m_aMousePos[g_Config.m_ClDummy].x, -201.0f * 32, (Collision()->GetWidth() + 201.0f) * 32.0f);
                m_aMousePos[g_Config.m_ClDummy].y = std::clamp(m_aMousePos[g_Config.m_ClDummy].y, -201.0f * 32, (Collision()->GetHeight() + 201.0f) * 32.0f);
        }
        else
        {
                const float MouseMin = GetMinMouseDistance();
                const float MouseMax = GetMaxMouseDistance();

                float MouseDistance = length(m_aMousePos[g_Config.m_ClDummy]);
                if(MouseDistance < 0.001f)
                {
                        m_aMousePos[g_Config.m_ClDummy].x = 0.001f;
                        m_aMousePos[g_Config.m_ClDummy].y = 0;
                        MouseDistance = 0.001f;
                }
                if(MouseDistance < MouseMin)
                        m_aMousePos[g_Config.m_ClDummy] = normalize_pre_length(m_aMousePos[g_Config.m_ClDummy], MouseDistance) * MouseMin;
                MouseDistance = length(m_aMousePos[g_Config.m_ClDummy]);
                if(MouseDistance > MouseMax)
                        m_aMousePos[g_Config.m_ClDummy] = normalize_pre_length(m_aMousePos[g_Config.m_ClDummy], MouseDistance) * MouseMax;
        }
}

float CControls::GetMinMouseDistance() const
{
        return g_Config.m_ClDyncam ? g_Config.m_ClDyncamMinDistance : g_Config.m_ClMouseMinDistance;
}

float CControls::GetMaxMouseDistance() const
{
        float CameraMaxDistance = 200.0f;
        float FollowFactor = (g_Config.m_ClDyncam ? g_Config.m_ClDyncamFollowFactor : g_Config.m_ClMouseFollowfactor) / 100.0f;
        float DeadZone = g_Config.m_ClDyncam ? g_Config.m_ClDyncamDeadzone : g_Config.m_ClMouseDeadzone;
        float MaxDistance = g_Config.m_ClDyncam ? g_Config.m_ClDyncamMaxDistance : g_Config.m_ClMouseMaxDistance;
        return std::min(FollowFactor != 0.0f ? CameraMaxDistance / FollowFactor + DeadZone : MaxDistance, MaxDistance);
}
