/* Vertex Auto Aled — automatic hammer rescue of ANY frozen player.
 *
 * The user positions and moves the tee manually (moves, jumps, hooks). This module only:
 *   1. scans every player and finds frozen ones that sit behind FREEZE tiles in a hammer's reach (an aled set-up);
 *   2. when a SIMULATION shows that a hit within the next few ticks frees the player for good, it pre-selects the
 *      hammer and aims at the target (so there is nothing left to do at the decisive moment);
 *   3. on the first tick on which the simulation proves the hit (frees now AND one tick later, and the player
 *      stays free for the next 20 ticks), it taps the fire counter: no extra waiting.
 *
 * Movement, jump and hook are never modified. The only setting is on / off (g_Config.m_VxAledEnable),
 * everything else is a constant below.
 *
 * Hammer geometry (CCharacter::FireWeapon): ProjStartPos = tee + aim * 0.75 * proximity radius;
 * FindEntities(ProjStartPos, 0.5 * proximity radius) hits every character whose centre is closer than
 * (target proximity radius + search radius) to ProjStartPos, except the shooter and tees it cannot collide with.
 */
#include "aled.h"

#include <base/dbg.h>
#include <base/math.h>
#include <base/time.h>
#include <base/vmath.h>

#include <engine/client.h>
#include <engine/shared/config.h>

#include <generated/protocol.h>

#include <game/collision.h>
#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/prediction/gameworld.h>
#include <game/mapitems.h>

#include <algorithm>
#include <cmath>

namespace
{
// ---- built-in settings (the menu only has the on / off switch) ----
constexpr float ALED_SEARCH_RANGE = 170.0f; // players this close are watched so the hammer is ready BEFORE the hit is possible (the hit itself needs the real reach, 63)
constexpr int ALED_GRACE_TICKS = 10; // keep aiming at the target this long after the set-up disappeared
constexpr int ALED_PAUSE_TICKS = 6; // pause after a finished attempt
constexpr int ALED_PRESS_LIMIT = 6; // hammer presses per target
constexpr bool ALED_SAFETY = true; // never hit when another tee is inside the hit circle
constexpr bool ALED_AVOID_TELES = true; // do not hit a target that stands on a teleporter
constexpr int ALED_LOOKAHEAD_MAX = 12; // look this many ticks ahead for the moment of the hit
constexpr int ALED_LOOKAHEAD_STEP = 2;

constexpr float ALED_TILE_STEP = 4.0f;
constexpr float ALED_TILE_HALF_WIDTH = 14.0f;
constexpr float ALED_HAMMER_START_FACTOR = 0.75f;
constexpr int ALED_PRESS_GAP = 7; // ticks between two presses (hammer reload is ~6 ticks)
constexpr int ALED_RESULT_WAIT = 8; // ticks after the last press before the attempt counts as finished (latency)
constexpr int ALED_MIN_FREEZE_LEFT = 3; // a freeze that ends within this many ticks is not worth a swing
constexpr float ALED_HIT_MARGIN = 1.0f; // keep this many units away from the edge of the hit circle
constexpr int ALED_WANTED_CLEAR_TICKS = 3;
constexpr int ALED_KEEP_TICKS = 6; // after the hit the target must be free for this many ticks (game rules for re-freezing apply)
constexpr int ALED_FREEZE_TOTAL = 150; // length of a normal freeze (3 s) in ticks
constexpr int ALED_REFREEZE_IMMUNITY = 50; // the game cannot freeze a tee again within 1 s after its freeze started
constexpr int ALED_MIN_GAIN = 8; // the hit has to save at least this many freeze ticks (not a natural expiry)
constexpr float ALED_END_MARGIN = 3.0f; // the between-corridor is shrunk by this much at both ends (own tiles do not count)
constexpr int ALED_BUDGET_MS = 12; // time budget of the search per game tick

// ---- tiles ----
bool AledTileAt(CCollision *pCollision, const vec2 &Pos, int Tile)
{
	const int Index = pCollision->GetPureMapIndex(Pos);
	if(Index < 0)
		return false;
	return pCollision->GetTileIndex(Index) == Tile || pCollision->GetFrontTileIndex(Index) == Tile;
}

bool AledOnTile(CCollision *pCollision, const vec2 &Pos, int Tile)
{
	const float Radius = 28.0f / 3.0f;
	const vec2 Points[5] = {
		Pos,
		vec2(Pos.x + Radius, Pos.y - Radius),
		vec2(Pos.x + Radius, Pos.y + Radius),
		vec2(Pos.x - Radius, Pos.y - Radius),
		vec2(Pos.x - Radius, Pos.y + Radius),
	};
	for(const vec2 &Point : Points)
		if(AledTileAt(pCollision, Point, Tile))
			return true;
	return false;
}

bool AledOnTele(CCollision *pCollision, const vec2 &Pos)
{
	return AledOnTile(pCollision, Pos, TILE_TELEIN) || AledOnTile(pCollision, Pos, TILE_TELEINEVIL);
}

// A FREEZE tile in a map cell strictly BETWEEN the two tees: the cells the tees stand in do not count.
// Pulling a tee out of the edge of a freeze block (it stands in / on that block) is therefore NOT an aled.
// Works in any direction (left / right / up / down / diagonal), one block or many.
bool AledFreezeBetween(CCollision *pCollision, const vec2 &From, const vec2 &To)
{
	const vec2 Delta = To - From;
	const float Dist = length(Delta);
	if(Dist <= 1.0f)
		return false;
	const vec2 Side = vec2(-Delta.y, Delta.x) / Dist;
	const int Steps = std::max(1, (int)std::ceil(Dist / ALED_TILE_STEP));
	for(int i = 0; i <= Steps; ++i)
	{
		const vec2 Center = From + Delta * ((float)i / (float)Steps);
		for(int Across = -2; Across <= 2; ++Across)
			if(AledTileAt(pCollision, Center + Side * ((float)Across * 7.0f), TILE_FREEZE))
				return true;
	}
	return false;
}
struct SAledHit
{
	bool m_InRange = false; // the target centre is inside the real hit circle
	bool m_CanHit = true; // the game lets me collide with / hammer the target
	bool m_Safe = true; // nobody else is inside the hit circle
	float m_Dist = 1e9f; // distance hit-circle centre -> target (smaller is better)
	int m_AimX = 0;
	int m_AimY = 0;
};

SAledHit AledEvaluate(CGameWorld &World, CCharacter *pMe, CCharacter *pTarget, int MyId, int TargetId)
{
	SAledHit R;
	// the aim the server will really see: integer target, normalized there
	const vec2 Delta = pTarget->m_Pos - pMe->m_Pos;
	R.m_AimX = (int)std::round(Delta.x);
	R.m_AimY = (int)std::round(Delta.y);
	if(R.m_AimX == 0 && R.m_AimY == 0)
		R.m_AimY = -1;
	const vec2 Direction = normalize(vec2((float)R.m_AimX, (float)R.m_AimY));

	const float Proximity = pMe->m_ProximityRadius;
	const vec2 HammerStart = pMe->m_Pos + Direction * (Proximity * ALED_HAMMER_START_FACTOR);
	const float SearchRadius = Proximity * 0.5f;
	R.m_Dist = distance(HammerStart, pTarget->m_Pos);
	R.m_InRange = R.m_Dist < SearchRadius + pTarget->m_ProximityRadius - ALED_HIT_MARGIN;
	R.m_CanHit = !pMe->Core()->m_HammerHitDisabled && pMe->CanCollide(TargetId);

	if(ALED_SAFETY)
	{
		for(int Id = 0; Id < MAX_CLIENTS; ++Id)
		{
			if(Id == MyId || Id == TargetId)
				continue;
			CCharacter *pOther = World.GetCharacterById(Id);
			if(!pOther || !pMe->CanCollide(Id))
				continue;
			if(distance(HammerStart, pOther->m_Pos) < SearchRadius + pOther->m_ProximityRadius)
			{
				R.m_Safe = false;
				break;
			}
		}
	}
	return R;
}

// Does the hammer REALLY free the target, and does it STAY free? Simulated in a copy of the world with the game's own rules.
// The hammer fires on step FireAt, aimed at the target as the position is on that step (like the module would aim then).
// AssumeHammer: pretend the hammer is already in the hand. The target must go from frozen to free on that hit (not by a
// natural expiry) and must not be frozen again during the next ALED_KEEP_TICKS ticks.
bool AledVerifyHit(CGameWorld &Src, int MyId, int TargetId, const CNetObj_PlayerInput &Base, int FireAt, bool AssumeHammer, int TargetFreezeLeft)
{
	CGameWorld Sim;
	Sim.CopyWorld(&Src);
	CCharacter *pC = Sim.GetCharacterById(MyId);
	CCharacter *pT = Sim.GetCharacterById(TargetId);
	if(!pC || !pT)
		return false;
	for(int Id = 0; Id < MAX_CLIENTS; ++Id)
	{
		if(Id == MyId || Id == TargetId)
			continue;
		CCharacter *pO = Sim.GetCharacterById(Id);
		if(!pO)
			continue;
		const bool Linked = pC->Core()->HookedPlayer() == Id || pT->Core()->HookedPlayer() == Id || pO->Core()->HookedPlayer() == MyId || pO->Core()->HookedPlayer() == TargetId;
		if(!Linked && distance(pO->m_Pos, pT->m_Pos) > 600.0f && distance(pO->m_Pos, pC->m_Pos) > 600.0f)
			delete pO;
	}
	pC = Sim.GetCharacterById(MyId);
	pT = Sim.GetCharacterById(TargetId);
	if(!pC || !pT)
		return false;
	if(AssumeHammer)
		const_cast<CCharacterCore *>(pC->Core())->m_ActiveWeapon = WEAPON_HAMMER; // Core() is const here; pC itself is our private copy
	if(TargetFreezeLeft > 0 && pT->m_FreezeTime < TargetFreezeLeft)
		pT->m_FreezeTime = TargetFreezeLeft; // other players' freeze may be unknown to the prediction

	CNetObj_PlayerInput In = Base;
	In.m_Fire &= INPUT_STATE_MASK;
	In.m_WantedWeapon = 0;

	const int MaxLen = std::min(FireAt + 2 + ALED_KEEP_TICKS, 48);
	bool aFrozen[49];
	int aLeft[49];
	aFrozen[0] = pT->m_FreezeTime > 0;
	aLeft[0] = pT->m_FreezeTime;
	int Len = 0;
	int HitT = -1;
	const int StartTick = Sim.GameTick();
	for(int s = 1; s <= MaxLen; ++s)
	{
		// aim at the target as it is now (what the module would write on this tick)
		const vec2 D = pT->m_Pos - pC->m_Pos;
		In.m_TargetX = (int)std::round(D.x);
		In.m_TargetY = (int)std::round(D.y);
		if(In.m_TargetX == 0 && In.m_TargetY == 0)
			In.m_TargetY = -1;
		if(s == FireAt)
			In.m_Fire = (In.m_Fire & 1) ? In.m_Fire + 2 : In.m_Fire + 1;
		else if(s == FireAt + 1 && (In.m_Fire & 1))
			In.m_Fire++;
		pC->OnDirectInput(&In);
		Sim.m_GameTick = StartTick + s;
		pC->OnPredictedInput(&In);
		Sim.Tick();
		pC = Sim.GetCharacterById(MyId);
		pT = Sim.GetCharacterById(TargetId);
		if(!pC || !pT)
			return false;
		if(pT->Core()->m_DeepFrozen)
			return false;
		aFrozen[s] = pT->m_FreezeTime > 0;
		aLeft[s] = pT->m_FreezeTime;
		Len = s;
		if(HitT < 0 && s >= FireAt && s <= FireAt + 1 && aFrozen[s - 1] && !aFrozen[s] && aLeft[s - 1] >= ALED_MIN_GAIN)
			HitT = s;
	}
	if(HitT < 0 || Len < HitT + ALED_KEEP_TICKS / 2)
		return false;
	// The game does not freeze a tee again within 1 s after its freeze STARTED (m_FreezeTick), but the prediction does not know
	// that for other players. A young freeze therefore keeps the tee free for the rest of that second whatever the simulation says.
	const int Age = std::clamp(ALED_FREEZE_TOTAL - aLeft[HitT - 1], 0, ALED_FREEZE_TOTAL);
	const int Immune = std::clamp(ALED_REFREEZE_IMMUNITY - Age, 0, ALED_REFREEZE_IMMUNITY);
	for(int t = HitT; t <= std::min(Len, HitT + ALED_KEEP_TICKS); ++t)
		if(aFrozen[t] && t >= HitT + Immune)
			return false; // frozen again right away: the hit would be wasted
	return true;
}
} // namespace

CAutoAled &VertexAutoAled()
{
	static CAutoAled s_AutoAled;
	return s_AutoAled;
}

void CAutoAled::EndPlan(CNetObj_PlayerInput *pSlot, bool RestoreWeapon, int NowTick, bool Cooldown)
{
	// Release only a fire edge written by this module. Never erase a newer user input.
	if(m_PlanActive && pSlot && pSlot->m_Fire == m_LastWrittenFire && (pSlot->m_Fire & 1))
		pSlot->m_Fire = (pSlot->m_Fire + 1) & INPUT_STATE_MASK;
	if(m_PlanActive && RestoreWeapon && pSlot)
	{
		pSlot->m_WantedWeapon = m_UserWantedWeapon;
		if(m_UserWantedWeapon != 0)
		{
			// the switch only has to be requested once: clear the request a few ticks later
			m_RestoredWanted = m_UserWantedWeapon;
			m_ClearWantedTick = NowTick + ALED_WANTED_CLEAR_TICKS;
		}
	}
	if(m_PlanActive && Cooldown)
		m_NextPlanTick = NowTick + ALED_PAUSE_TICKS;

	m_PlanActive = false;
	m_PlanTargetId = -1;
	m_LastWrittenFire = -1;
	m_HammerPresses = 0;
	m_ReleasePending = false;
	m_CacheTick = -1000000;
}

void CAutoAled::Reset()
{
	m_PlanActive = false;
	m_PlanTargetId = -1;
	m_LastWorkTick = -1000000;
	m_NextPlanTick = -1000000;
	m_UserWantedWeapon = 0;
	m_LastWrittenFire = -1;
	m_HammerPresses = 0;
	m_LastPressTick = -1000000;
	m_ReleasePending = false;
	m_LastReadyTick = -1000000;
	m_CacheTick = -1000000;
	m_ClearWantedTick = -1;
	m_RestoredWanted = 0;
	m_pStatus = "Auto Aled: disabled";
	m_pCooldownStatus = "Searching for frozen players";
}

void CAutoAled::Cancel(CNetObj_PlayerInput *pSlot)
{
	EndPlan(pSlot, true, m_LastWorkTick, false);
	m_pStatus = "Searching for frozen players";
}

void CAutoAled::Update(CGameClient *pGame, IClient *pClient, CNetObj_PlayerInput *pSlot)
{
	if(!pGame || !pClient || !pSlot)
		return;

	const int Dummy = g_Config.m_ClDummy;
	const int NowTick = pClient->GameTick(Dummy);
    	
	// a request that only restored the user's weapon is cleared again (so it does not keep forcing the weapon)
	if(m_ClearWantedTick >= 0 && NowTick >= m_ClearWantedTick)
	{
		if(pSlot->m_WantedWeapon == m_RestoredWanted)
			pSlot->m_WantedWeapon = 0;
		m_ClearWantedTick = -1;
	}

	if(!g_Config.m_VxAledEnable)
	{
		EndPlan(pSlot, true, NowTick, false);
		m_pStatus = "Auto Aled: disabled";
		return;
	}
	if(pClient->State() != IClient::STATE_ONLINE)
	{
		EndPlan(pSlot, true, NowTick, false);
		m_pStatus = "Searching for frozen players";
		return;
	}
	if(m_LastDummy != Dummy)
	{
		EndPlan(pSlot, true, NowTick, false);
		m_LastDummy = Dummy;
	}
	if(NowTick < m_LastWorkTick - 50) // reconnect / map change
	{
		EndPlan(pSlot, true, NowTick, false);
		Reset();
	}
	m_LastWorkTick = NowTick;

	const int MyId = pGame->m_aLocalIds[Dummy];
	if(MyId < 0 || MyId >= MAX_CLIENTS || pGame->m_Snap.m_SpecInfo.m_Active)
	{
		EndPlan(pSlot, true, NowTick, false);
		m_pStatus = "Searching for frozen players";
		return;
	}

	// predicted world first (it is where the server will be when the input arrives); the snapshot world is the fallback
	CGameWorld *pWorld = &pGame->m_PredictedWorld;
	CCharacter *pMe = pWorld->GetCharacterById(MyId);
	if(!pMe)
	{
		pWorld = &pGame->m_GameWorld;
		pMe = pWorld->GetCharacterById(MyId);
	}
	if(!pMe)
	{
		EndPlan(pSlot, true, NowTick, false);
		m_pStatus = "Searching for frozen players";
		return;
	}
	CGameWorld &World = *pWorld;

	if(pMe->m_FreezeTime > 0 || pMe->Core()->m_DeepFrozen)
	{
		EndPlan(pSlot, true, NowTick, true);
		m_pStatus = "Waiting: no frozen players"; // I am frozen myself: nothing to do
		return;
	}
	if(!pMe->Core()->m_aWeapons[WEAPON_HAMMER].m_Got || pMe->Core()->m_HammerHitDisabled)
	{
		EndPlan(pSlot, true, NowTick, false);
		m_pStatus = "Waiting: unsafe hammer hit"; // the hammer cannot be used at all
		return;
	}

	// Do not steal a deliberate user weapon change or fire press.
	if(m_PlanActive && pSlot->m_WantedWeapon != 0 && pSlot->m_WantedWeapon != WEAPON_HAMMER + 1)
	{
		EndPlan(pSlot, false, NowTick, false);
		m_pStatus = "Searching for frozen players";
		return;
	}
	if(m_PlanActive && pSlot->m_Fire != m_LastWrittenFire)
	{
		EndPlan(pSlot, true, NowTick, false);
		m_pStatus = "Searching for frozen players";
		return;
	}
	if(!m_PlanActive && (pSlot->m_Fire & 1))
	{
		m_pStatus = "Searching for frozen players"; // the user holds fire: not touched
		return;
	}
	// NOTE: a non-zero wanted weapon of the user is normally just the weapon he selected with a number key earlier
	// (the game keeps it in the input), so it must not block Auto Aled. It is remembered and restored afterwards.

	// the heavy search runs once per game tick: repeated calls inside the same tick only re-apply the aim
	if(NowTick == m_CacheTick)
	{
		if(m_PlanActive)
		{
		     			if(m_CacheAimX != 0 || m_CacheAimY != 0)
			{
				pSlot->m_TargetX = m_CacheAimX;
				pSlot->m_TargetY = m_CacheAimY;
			}
			pSlot->m_WantedWeapon = WEAPON_HAMMER + 1;
		}
		return;
	}

	CCollision *pCollision = pMe->Collision();

	auto FreezeLeftOf = [&](CCharacter *pC, int Id) -> int {
		const int Snap = std::max(0, pGame->m_aClients[Id].m_FreezeEnd - NowTick);
		return std::max(pC->m_FreezeTime, Snap);
	};
	auto IsDeepFrozen = [&](CCharacter *pC, int Id) -> bool {
		return pC->Core()->m_DeepFrozen || pGame->m_aClients[Id].m_DeepFrozen;
	};

	// ---- the current plan target: did my hit free it? ----
	if(m_PlanActive && m_PlanTargetId >= 0)
	{
		CCharacter *pPlanTarget = World.GetCharacterById(m_PlanTargetId);
		const bool Gone = !pPlanTarget;
		const int Left = pPlanTarget ? FreezeLeftOf(pPlanTarget, m_PlanTargetId) : 0;
		if(Gone || Left <= 0)
		{
			const bool Freed = !Gone && m_HammerPresses > 0;
			EndPlan(pSlot, true, NowTick, true);
			m_pCooldownStatus = Freed ? "Auto Aled: target unfrozen" : "Searching for frozen players";
			m_pStatus = m_pCooldownStatus;
			return;
		}
	}

	if(!m_PlanActive && NowTick < m_NextPlanTick)
	{
		m_pStatus = m_pCooldownStatus;
		return;
	}

	m_CacheTick = NowTick;
	const int64_t SearchT0 = time_get();
	auto OverBudget = [&]() -> bool { return (time_get() - SearchT0) * 1000 / time_freq() > ALED_BUDGET_MS; };

	// ---- scan EVERY player: who is frozen behind FREEZE tiles within reach? ----
	struct SCand
	{
		int m_Id = -1;
		SAledHit m_Hit;
		float m_Score = 1e30f;
		int m_Left = 0;
	};
	SCand aCand[8];
	int NumCand = 0;
	bool AnyPlayer = false, AnyFrozen = false, AnyBetween = false;
	bool CurEligible = false;

	for(int Id = 0; Id < MAX_CLIENTS; ++Id)
	{
		if(Id == MyId || !pGame->m_aClients[Id].m_Active)
			continue;
		CCharacter *pT = World.GetCharacterById(Id);
		if(!pT)
			continue;
		AnyPlayer = true;
		if(IsDeepFrozen(pT, Id) || FreezeLeftOf(pT, Id) <= ALED_MIN_FREEZE_LEFT)
			continue;
		AnyFrozen = true;
		if(distance(pMe->m_Pos, pT->m_Pos) > ALED_SEARCH_RANGE)
			continue;
		if(!AledFreezeBetween(pCollision, pMe->m_Pos, pT->m_Pos))
			continue;
		AnyBetween = true;
				if(AledTileAt(pCollision, pT->m_Pos, TILE_FREEZE) && distance(pMe->m_Pos, pT->m_Pos) < 36.0f)
			continue; // dummy sits at the edge right next to me: pulling it out, not an aled
						// my hook holds (or is flying at) this tee: I am pulling it out myself, that is not an aled
		if(pMe->Core()->HookedPlayer() == Id)
			continue;
		if(pMe->Core()->m_HookState == HOOK_FLYING && length(pT->m_Pos - pMe->m_Pos) > 1.0f && length(pMe->Core()->m_HookDir) > 0.01f &&
			dot(normalize(pMe->Core()->m_HookDir), normalize(pT->m_Pos - pMe->m_Pos)) > 0.94f)
			continue;
		if(ALED_AVOID_TELES && AledOnTele(pCollision, pT->m_Pos))
			continue;

		const bool IsCurrent = m_PlanActive && Id == m_PlanTargetId;
		if(IsCurrent)
			CurEligible = true;
		if(NumCand < 8)
		{
			aCand[NumCand].m_Id = Id;
			aCand[NumCand].m_Hit = AledEvaluate(World, pMe, pT, MyId, Id);
			aCand[NumCand].m_Score = aCand[NumCand].m_Hit.m_Dist - (IsCurrent ? 1000.0f : 0.0f);
			aCand[NumCand].m_Left = FreezeLeftOf(pT, Id);
			++NumCand;
		}
	}
	std::sort(aCand, aCand + NumCand, [](const SCand &A, const SCand &B) { return A.m_Score < B.m_Score; });

	const bool AssumeHammer = pMe->Core()->m_ActiveWeapon != WEAPON_HAMMER;
	const int SwitchDelay = AssumeHammer ? 2 : 0; // the hammer still has to be switched in: the shot comes a few ticks later

	// ---- 1) a hit RIGHT NOW: the simulation proves that it frees the target and keeps it free ----
	int ReadyIdx = -1;
	for(int c = 0; c < NumCand && c < 3 && !OverBudget(); ++c)
	{
		const SCand &C = aCand[c];
		if(!C.m_Hit.m_InRange || !C.m_Hit.m_CanHit || !C.m_Hit.m_Safe)
			continue;
		if(!AledVerifyHit(World, MyId, C.m_Id, *pSlot, 1 + SwitchDelay, AssumeHammer, C.m_Left))
			continue;
		// when something moves, the hit must also work one tick later (latency jitter): no marginal shots.
		// Two still tees need no second check: the first one is exact.
		CCharacter *pCT = World.GetCharacterById(C.m_Id);
		const bool Moving = !pCT || length(pCT->Core()->m_Vel) > 1.5f || length(pMe->Core()->m_Vel) > 1.5f;
				if(!Moving || AledVerifyHit(World, MyId, C.m_Id, *pSlot, 2 + SwitchDelay, AssumeHammer, C.m_Left))
		{
			ReadyIdx = c;
			break;
		}
	}

	// ---- 2) a frozen player behind a freeze block is near: get ready in advance (hammer in the hand) ----
	int IntentIdx = -1;
	if(ReadyIdx < 0 && NumCand > 0)
		IntentIdx = 0;

	const int PickIdx = ReadyIdx >= 0 ? ReadyIdx : IntentIdx;

	// ---- nothing to do ----
	if(PickIdx < 0)
	{
		if(m_PlanActive && CurEligible && NowTick - m_LastReadyTick <= ALED_GRACE_TICKS)
		{
			// keep the aim for a moment: the user is still moving into position (never press here)
			CCharacter *pPlanTarget = World.GetCharacterById(m_PlanTargetId);
			 			if(pPlanTarget && distance(pPlanTarget->m_Pos, pMe->m_Pos) < 90.0f)
			{
				const vec2 Aim = pPlanTarget->m_Pos - pMe->m_Pos;
				pSlot->m_TargetX = (int)std::round(Aim.x);
				pSlot->m_TargetY = (int)std::round(Aim.y);
				if(pSlot->m_TargetX == 0 && pSlot->m_TargetY == 0)
					pSlot->m_TargetY = -1;
				m_CacheAimX = pSlot->m_TargetX;
				m_CacheAimY = pSlot->m_TargetY;
			}
			if(m_ReleasePending && NowTick > m_LastPressTick && (pSlot->m_Fire & 1))
			{
				pSlot->m_Fire = (pSlot->m_Fire + 1) & INPUT_STATE_MASK;
				m_ReleasePending = false;
			}
			m_LastWrittenFire = pSlot->m_Fire;
			m_pStatus = "Waiting: unsafe hammer hit";
			return;
		}
		if(m_PlanActive)
			EndPlan(pSlot, true, NowTick, false);

		if(!AnyPlayer)
			m_pStatus = "Searching for frozen players";
		else if(!AnyFrozen)
			m_pStatus = "Waiting: no frozen players";
		else if(!AnyBetween)
			m_pStatus = "Waiting: no Freeze tile between tees";
		else if(NumCand == 0)
			m_pStatus = "Waiting: target out of hammer range";
		else
			m_pStatus = "Waiting: unsafe hammer hit"; // geometry fits, but nothing proves a lasting unfreeze yet
		return;
	}

	// ---- an aled is possible or being set up: start / continue the plan ----
	const SCand &Pick = aCand[PickIdx];
	if(!m_PlanActive)
	{
		m_UserWantedWeapon = pSlot->m_WantedWeapon != 0 ? pSlot->m_WantedWeapon : pMe->Core()->m_ActiveWeapon + 1;
		m_HammerPresses = 0;
		m_ReleasePending = false;
		m_LastPressTick = -1000000;
	}
	else if(Pick.m_Id != m_PlanTargetId)
	{
		m_HammerPresses = 0; // new target: its own press budget
	}
	m_PlanActive = true;
	m_PlanTargetId = Pick.m_Id;
	m_LastReadyTick = NowTick;

		pSlot->m_WantedWeapon = WEAPON_HAMMER + 1;
	m_CacheAimX = 0;
	m_CacheAimY = 0;
	if(ReadyIdx >= 0 || Pick.m_Hit.m_Dist < 70.0f) // far away only the hammer is prepared: the user's own aim (hook!) stays untouched
	{
		pSlot->m_TargetX = Pick.m_Hit.m_AimX;
		pSlot->m_TargetY = Pick.m_Hit.m_AimY;
		m_CacheAimX = pSlot->m_TargetX;
		m_CacheAimY = pSlot->m_TargetY;
	}

	// the previous press is released on the next tick (the counter alternates odd = pressed, even = released)
	if(m_ReleasePending && NowTick > m_LastPressTick)
	{
		if(pSlot->m_Fire & 1)
			pSlot->m_Fire = (pSlot->m_Fire + 1) & INPUT_STATE_MASK;
		m_ReleasePending = false;
	}

	if(pMe->Core()->m_ActiveWeapon != WEAPON_HAMMER)
	{
		// the hammer is queued; never fire the weapon that is still in the hand
		if(pSlot->m_Fire & 1)
			pSlot->m_Fire = (pSlot->m_Fire + 1) & INPUT_STATE_MASK;
		m_LastWrittenFire = pSlot->m_Fire;
		m_pStatus = "Auto Aled: target selected";
		return;
	}

	// attempts finished? (the press budget is used and the last result had time to arrive)
	if(m_HammerPresses >= ALED_PRESS_LIMIT && NowTick - m_LastPressTick >= ALED_RESULT_WAIT)
	{
		m_pCooldownStatus = "Auto Aled: attempt completed";
		EndPlan(pSlot, true, NowTick, true);
		m_pStatus = m_pCooldownStatus;
		return;
	}

	// press only when the hit is proven RIGHT NOW (ReadyIdx), not merely expected later (IntentIdx)
	const bool CanPress = ReadyIdx >= 0 && m_HammerPresses < ALED_PRESS_LIMIT && NowTick - m_LastPressTick >= ALED_PRESS_GAP && !(pSlot->m_Fire & 1) && !m_ReleasePending;
	if(!CanPress)
	{
		m_LastWrittenFire = pSlot->m_Fire;
		m_pStatus = "Auto Aled: target selected";
		return;
	}

	// a REAL press: the counter goes from even to odd (CountInput sees one press)
	const int NextFire = (pSlot->m_Fire + 1) & INPUT_STATE_MASK;
	if(!(NextFire & 1))
	{
		m_LastWrittenFire = pSlot->m_Fire;
		m_pStatus = "Auto Aled: target selected";
		return;
	}
	pSlot->m_Fire = NextFire;
	m_LastWrittenFire = NextFire;
	m_LastPressTick = NowTick;
	m_ReleasePending = true;
	++m_HammerPresses;
	m_pStatus = "Auto Aled: hammer attack";
}