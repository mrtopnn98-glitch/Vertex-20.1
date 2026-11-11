/* Vertex client — Avoid Freeze (blatant).
 *
 * Automatic rescue of the tee I control ("максимально спасал как только можно"):
 *
 *  - every tick a forecast of my flight is made in a copy of the predicted world
 *    (the very same idiom the dummy unfreeze uses) and the first tick is looked
 *    for where I end up FROZEN / on a DEATH tile / on a TELE-IN tile (per settings);
 *  - when that tick is closer than "Kick in ticks", a rescue search starts:
 *      1) escapes without the hook (direction + jump, keeping my direction first),
 *      2) hook drags: a ring of angles around the last good angle / the cursor,
 *         limited by the FOV; every candidate is simulated to the end and must
 *         stay safe for "Tiles: Ticks" ticks;
 *  - while I am ALREADY frozen the module tries to drag me out towards unfreeze
 *    tiles ("Tiles: Unfreeze"); NSIF turns this off;
 *  - Track Point: the bind places a point, the module automatically walks/hook-
 *    drags back to it ("Safe tracking" forbids moving through freeze/death);
 *  - Auto Drag: while the bind is held the module hook-drags towards the cursor;
 *  - Aim assist: when I hook manually, the aim is steered to the nearest safe
 *    hook direction (within FOV).
 *
 * The decision is re-made from the current predicted state every tick (closed
 * loop). The found inputs are written straight into my input slot (silent aim:
 * the mouse is not touched) and everything is restored when the danger is gone.
 *
 * Everything is single-threaded and bounded by a time budget (2.5 ms) exactly
 * like Dummy Unfreeze v7; when idle the cost is one short forecast per tick.
 * Debug log tag: "avoid-freeze" (vx_af_debug 1).
 */
#include "avoidfreeze.h"
#include <base/dbg.h>
#include <base/system.h>
#include <base/time.h>
#include <engine/client.h>
#include <engine/graphics.h>
#include <engine/input.h>
#include <engine/shared/config.h>
#include <generated/protocol.h>
#include <game/collision.h>
#include <game/gamecore.h>
#include <game/mapitems.h>
#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/prediction/gameworld.h>
#include <algorithm>
#include <cmath>
// ============================================================================
// constants
// ============================================================================
constexpr int AF_MAXLEN = 64; // longest forecast (ticks)
constexpr int AF_PATH = 48; // path points kept for the visuals
constexpr int AF_HORIZON_CAP = 60;
constexpr float AF_PHYS = 28.0f; // CCharacterCore::PhysicalSize()
constexpr int AF_KEEP_RADIUS = 600; // tees farther away are not simulated
constexpr double AF_BUDGET_MS = 2.5; // per tick for the whole search
constexpr int AF_HOOK_MAXPHASE = 26; // the hook phase of a candidate (ticks)
constexpr int AF_HOOK_RELEASE = 6; // release the hook this many ticks after it attached
constexpr float AF_AIM_LEN = 1000.0f; // length of the silent aim vector
constexpr float AF_PI = 3.14159265358979323846f;
constexpr float AF_NAV_FAR = 140.0f; // start navigating back to the track point
constexpr float AF_NAV_NEAR = 80.0f; // stop navigating (hysteresis)
constexpr float AF_NAV_PROGRESS = 8.0f; // a nav candidate has to gain this many px
constexpr int AF_MIN_GAIN_EXIT = 8; // a frozen exit has to save this many freeze ticks
constexpr int AF_EXIT_INTERVAL = 10; // re-search the frozen exit every N ticks
enum
{
	AF_NONE = 0,
	AF_DIR = 1, // escape with direction / jump, no hook
	AF_HOOK = 2, // escape with a hook drag
	AF_NAV = 3, // walk / hook back to the track point
	AF_DRAG = 4, // manual auto drag towards the cursor
};
// ============================================================================
// forecast trace + candidate
// ============================================================================
struct SAfSim
{
	vec2 m_aP[AF_MAXLEN + 1];
	bool m_aF[AF_MAXLEN + 1]; // frozen after tick s
	bool m_aD[AF_MAXLEN + 1]; // dead / deep frozen after tick s
	bool m_aT[AF_MAXLEN + 1]; // on a tele-in tile after tick s
	bool m_aOn[AF_MAXLEN + 1]; // on a freeze tile after tick s
	bool m_aU[AF_MAXLEN + 1]; // on an unfreeze tile after tick s
	int m_Len = 0;
	int m_AttachAt = -1; // tick the hook attached the first time
};
struct SAfCand
{
	bool m_Found = false;
	int m_Type = AF_NONE;
	int m_Dir = 0;
	bool m_Jump = false;
	bool m_Hook = false;
	float m_Angle = 0.0f;
	vec2 m_Aim = vec2(0.0f, 0.0f);
	float m_Score = -1e30f;
	int m_HookTicks = 0;
	vec2 m_aPath[AF_PATH];
	int m_NumPath = 0;
};
// ============================================================================
// visuals (filled by Update, drawn by RenderWorld)
// ============================================================================
static vec2 s_aPathVis[AF_PATH];
static int s_PathVisN = 0;
static int s_PathVisUntil = -1;
static vec2 s_TrackVisPos = vec2(0.0f, 0.0f);
static int s_TrackVisUntil = -1;
static vec2 s_AimVisFrom = vec2(0.0f, 0.0f);
static vec2 s_AimVisTo = vec2(0.0f, 0.0f);
static int s_AimVisUntil = -1;
static void AfSetPathVis(const vec2 *pPts, int Num, int UntilTick)
{
	s_PathVisN = 0;
	for(int i = 0; i < Num && i < AF_PATH; i++)
		s_aPathVis[s_PathVisN++] = pPts[i];
	s_PathVisUntil = UntilTick;
}
// copy the flight trace of a candidate into the path kept for the visuals
static void AfPushPathVis(SAfCand &C, const SAfSim &S)
{
	C.m_NumPath = 0;
	const int Last = S.m_Len;
	if(Last < 0)
		return;
	const int Stride = std::max(1, (Last + AF_PATH - 1) / AF_PATH);
	for(int i = 0; i <= Last && C.m_NumPath < AF_PATH; i += Stride)
		C.m_aPath[C.m_NumPath++] = S.m_aP[i];
	if(C.m_NumPath > 0 && (C.m_aPath[C.m_NumPath - 1].x != S.m_aP[Last].x || C.m_aPath[C.m_NumPath - 1].y != S.m_aP[Last].y))
	{
		if(C.m_NumPath < AF_PATH)
			C.m_aPath[C.m_NumPath++] = S.m_aP[Last];
		else
			C.m_aPath[C.m_NumPath - 1] = S.m_aP[Last];
	}
}
// ============================================================================
// tiles
// ============================================================================
static bool AfTileAt(CCollision *pCol, const vec2 &Pos, int Tile)
{
	const int Idx = pCol->GetPureMapIndex(Pos);
	if(Idx < 0)
		return false;
	return pCol->GetTileIndex(Idx) == Tile || pCol->GetFrontTileIndex(Idx) == Tile;
}
static bool AfOnTile(CCollision *pCol, const vec2 &Pos, int Tile)
{
	const float R = AF_PHYS / 3.0f;
	const vec2 aPts[5] = {
		Pos,
		vec2(Pos.x + R, Pos.y - R),
		vec2(Pos.x + R, Pos.y + R),
		vec2(Pos.x - R, Pos.y - R),
		vec2(Pos.x - R, Pos.y + R)};
	for(const vec2 &P : aPts)
		if(AfTileAt(pCol, P, Tile))
			return true;
	return false;
}
static bool AfOnFreeze(CCollision *pCol, const vec2 &Pos) { return AfOnTile(pCol, Pos, TILE_FREEZE); }
static bool AfOnUnfreeze(CCollision *pCol, const vec2 &Pos) { return AfOnTile(pCol, Pos, TILE_UNFREEZE); }
static bool AfOnTele(CCollision *pCol, const vec2 &Pos)
{
	return AfOnTile(pCol, Pos, TILE_TELEIN) || AfOnTile(pCol, Pos, TILE_TELEINEVIL);
}
static float AfAngleDev(float A, float B)
{
	float D = A - B;
	while(D > AF_PI)
		D -= 2.0f * AF_PI;
	while(D < -AF_PI)
		D += 2.0f * AF_PI;
	return std::fabs(D);
}
// ============================================================================
// forecast: my flight in a copy of the predicted world with a fixed input.
// The hook (when held) is released AF_HOOK_RELEASE ticks after it attached:
// drag, then let go and drift. Death / deep freeze marks m_aD and shortens
// the trace.
// ============================================================================
static bool AfSimulate(CGameClient *pGame, int Cid, const CNetObj_PlayerInput *pIn, int MaxLen, SAfSim &Out)
{
	MaxLen = std::min(MaxLen, AF_MAXLEN);
	CGameWorld SimWorld;
	SimWorld.CopyWorld(&pGame->m_PredictedWorld);
	CCharacter *pC = SimWorld.GetCharacterById(Cid);
	if(!pC)
		return false;
	// other tees: keep the ones my hook is linked to, drop the far ones
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(i == Cid)
			continue;
		CCharacter *pOther = SimWorld.GetCharacterById(i);
		if(!pOther)
			continue;
		const bool Linked = pC->Core()->HookedPlayer() == i || pOther->Core()->HookedPlayer() == Cid;
		if(!Linked && distance(pOther->m_Pos, pC->m_Pos) > AF_KEEP_RADIUS)
			delete pOther;
	}
	pC = SimWorld.GetCharacterById(Cid);
	if(!pC)
		return false;
	CCollision *pCol = pC->Collision();
	CNetObj_PlayerInput In = *pIn;
	const int StartTick = SimWorld.GameTick();
	Out.m_aP[0] = pC->m_Pos;
	Out.m_aF[0] = pC->m_FreezeTime > 0;
	Out.m_aD[0] = false;
	Out.m_aT[0] = AfOnTele(pCol, pC->m_Pos);
	Out.m_aOn[0] = AfOnFreeze(pCol, pC->m_Pos);
	Out.m_aU[0] = AfOnUnfreeze(pCol, pC->m_Pos);
	Out.m_Len = 0;
	Out.m_AttachAt = -1;
	for(int s = 1; s <= MaxLen; s++)
	{
		if(In.m_Hook && Out.m_AttachAt > 0 && s >= Out.m_AttachAt + AF_HOOK_RELEASE)
			In.m_Hook = 0; // the drag is done, let go and drift
		pC->OnDirectInput(&In);
		SimWorld.m_GameTick = StartTick + s;
		pC->OnPredictedInput(&In);
		SimWorld.Tick();
		pC = SimWorld.GetCharacterById(Cid);
		if(!pC || pC->Core()->m_DeepFrozen)
		{
			Out.m_aP[s] = Out.m_aP[s - 1];
			Out.m_aF[s] = false;
			Out.m_aD[s] = true;
			Out.m_aT[s] = false;
			Out.m_aOn[s] = false;
			Out.m_aU[s] = false;
			Out.m_Len = s;
			return true;
		}
		if(In.m_Hook && Out.m_AttachAt < 0 && pC->Core()->m_HookState == HOOK_ATTACHED)
			Out.m_AttachAt = s;
		Out.m_aP[s] = pC->m_Pos;
		Out.m_aF[s] = pC->m_FreezeTime > 0;
		Out.m_aD[s] = false;
		Out.m_aT[s] = AfOnTele(pCol, pC->m_Pos);
		Out.m_aOn[s] = AfOnFreeze(pCol, pC->m_Pos);
		Out.m_aU[s] = AfOnUnfreeze(pCol, pC->m_Pos);
		Out.m_Len = s;
	}
	return true;
}
// ============================================================================
// CAvoidFreeze
// ============================================================================
bool CAvoidFreeze::AnyActive() const
{
	return m_RescueActive || m_ExitActive || m_NavActive || m_DragActive || m_AssistActive;
}
void CAvoidFreeze::RestoreSlot(CNetObj_PlayerInput *pSlot)
{
	pSlot->m_Direction = m_UserDir;
	pSlot->m_Jump = m_UserJump;
	pSlot->m_Hook = m_UserHook;
	pSlot->m_TargetX = m_UserTargetX;
	pSlot->m_TargetY = m_UserTargetY;
	m_WroteJump = m_UserJump;
	m_WroteHook = m_UserHook;
}
void CAvoidFreeze::Reset()
{
	m_PlanType = AF_NONE;
	m_PlanTick = -1000000;
	m_LastWorkTick = -1000000;
	m_LogGateTick = -1000000;
	m_NextExitTick = -1000000;
	m_WarmTick = -1000000;
	m_LastInputChangeTick = -1000000;
	m_TrackPointSet = false;
	m_TrackPoint = vec2(0.0f, 0.0f);
	m_RescueActive = m_ExitActive = m_NavActive = m_DragActive = m_AssistActive = false;
	s_PathVisN = 0;
	s_PathVisUntil = -1;
	s_TrackVisUntil = -1;
	s_AimVisUntil = -1;
}
void CAvoidFreeze::Update(CGameClient *pGame, IClient *pClient, CNetObj_PlayerInput *pSlot)
{
	const bool ModeOk = g_Config.m_VxAfMode == 2 && g_Config.m_VxAfEnable != 0 && g_Config.m_VxAfBlatant != 0;
	const bool WasActive = AnyActive();
	if(!ModeOk)
	{
		if(WasActive)
		{
			RestoreSlot(pSlot); // give the control back exactly as it was
			m_RescueActive = m_ExitActive = m_NavActive = m_DragActive = m_AssistActive = false;
			if(g_Config.m_VxAfDebug)
				dbg_msg("avoid-freeze", "off: controls restored");
		}
		return;
	}
	if(pClient->State() != IClient::STATE_ONLINE)
		return;
	const int LocalId = pGame->m_Snap.m_LocalClientId;
	if(LocalId < 0 || pGame->m_Snap.m_SpecInfo.m_Active)
		return;
	CCharacter *pMe = pGame->m_PredictedWorld.GetCharacterById(LocalId);
	if(!pMe)
		return;
	const int NowTick = pClient->GameTick(g_Config.m_ClDummy);
	if(NowTick < m_LastWorkTick - 50) // reconnect / map change: forget everything
		Reset();
	if(NowTick == m_LastWorkTick) // once per tick
		return;
	m_LastWorkTick = NowTick;
	const CNetObj_PlayerInput User = *pSlot; // my input as it is right now (direction / aim are already up to date)
	m_UserDir = User.m_Direction;
	m_UserTargetX = User.m_TargetX;
	m_UserTargetY = User.m_TargetY;
	// jump/hook are sticky: while the plan holds them, the user's key events still write
	// through (the input system overwrites our value on press/release) — adopt those,
	// otherwise keep the last known user values
	if(!WasActive || pSlot->m_Jump != m_WroteJump)
		m_UserJump = User.m_Jump;
	if(!WasActive || pSlot->m_Hook != m_WroteHook)
		m_UserHook = User.m_Hook;
	const bool Debug = g_Config.m_VxAfDebug != 0;
	// ---- afk detection (any input change resets the timer; tracked only while passive,
	// the module owns the controls while a plan runs anyway) ----
	const bool HookEdge = m_UserHook != 0 && m_LastHook == 0;
	if(!WasActive && (m_UserDir != m_LastDir || m_UserJump != m_LastJump || m_UserHook != m_LastHook))
		m_LastInputChangeTick = NowTick;
	if(!WasActive)
	{
		m_LastDir = m_UserDir;
		m_LastJump = m_UserJump;
		m_LastHook = m_UserHook;
	}
	const int TickSpeed = pClient->GameTickSpeed();
	const bool Afk = TickSpeed > 0 && NowTick - m_LastInputChangeTick >= g_Config.m_VxAfAfkSeconds * TickSpeed;
	const int64_t T0 = time_get();
	auto OverBudget = [&]() -> bool {
		return (double)(time_get() - T0) * 1000.0 / (double)time_freq() > AF_BUDGET_MS;
	};
	const int SimLen = std::clamp(g_Config.m_VxAfCheckTicks, 5, AF_MAXLEN);
	const int Horizon = std::min(std::clamp(g_Config.m_VxAfTileTicks, 5, AF_HORIZON_CAP), SimLen);
	const bool ChkDeath = g_Config.m_VxAfDeath != 0;
	const bool ChkTeles = g_Config.m_VxAfTeles != 0;
	// ---- base forecast with my own input ----
	const int ScanLen = g_Config.m_VxAfPred ? SimLen : 1;
	SAfSim Base;
	if(!AfSimulate(pGame, LocalId, &User, ScanLen, Base))
		return;
	// trigger of the rescue: what counts as "danger ahead" (Death is optional here)
	auto TriggerAt = [&](const SAfSim &S, int s) -> bool {
		return S.m_aF[s] || (ChkDeath && S.m_aD[s]) || (ChkTeles && S.m_aT[s]);
	};
	// hard disqualification of a candidate: dying is NEVER a plan
	auto BadAt = [&](const SAfSim &S, int s) -> bool {
		return S.m_aD[s] || S.m_aF[s] || (ChkTeles && S.m_aT[s]);
	};
	int DangerT = -1;
	for(int s = 1; s <= Base.m_Len; s++)
	{
		if(TriggerAt(Base, s))
		{
			DangerT = s;
			break;
		}
	}
	const bool FrozenNow = Base.m_aF[0];
	const int Kick = std::clamp(g_Config.m_VxAfKickTicks, 2, SimLen);
	const bool InDanger = DangerT >= 0 && DangerT <= Kick;
	const vec2 MyPos = Base.m_aP[0];
	// cursor direction (for FOV / aim preference)
	const float AimAngle = (User.m_TargetX || User.m_TargetY) ? std::atan2((float)User.m_TargetY, (float)User.m_TargetX) : 0.0f;
	const float AimLen = std::sqrt((float)User.m_TargetX * User.m_TargetX + (float)User.m_TargetY * User.m_TargetY);
	// ---- candidate machinery: no-hook escapes + a hook ring; Accept validates & scores ----
	SAfCand Best;
	bool HavePlan = false;
	auto TrackBest = [&](const SAfCand &C) {
		if(!HavePlan || C.m_Score > Best.m_Score)
		{
			Best = C;
			HavePlan = true;
		}
	};
	// stickiness: keep the previous plan while it is still good
	auto Sticky = [&](const SAfCand &C) -> float {
		if(m_PlanTick == NowTick - 1 && C.m_Type == m_PlanType && C.m_Dir == m_PlanDir &&
			(!C.m_Hook || AfAngleDev(C.m_Angle, m_PlanAngle) < 0.26f))
			return 60.0f;
		return 0.0f;
	};
	auto RunCandidates = [&](auto &&Accept, int DirSimLen) {
		// 1) escapes without the hook: keep my direction first, then the opposite, then none; jump variants
		if(g_Config.m_VxAfDirection)
		{
			for(int Di = 0; Di < 3 && !OverBudget(); Di++)
			{
				int Dir;
				if(Di == 0)
					Dir = User.m_Direction != 0 ? User.m_Direction : 1;
				else if(Di == 1)
					Dir = User.m_Direction != 0 ? -User.m_Direction : -1;
				else
					Dir = 0;
				for(int J = 0; J < 2 && !OverBudget(); J++)
				{
					CNetObj_PlayerInput In = User;
					In.m_Direction = Dir;
					In.m_Jump = J;
					In.m_Hook = 0;
					SAfSim S;
					if(!AfSimulate(pGame, LocalId, &In, DirSimLen, S))
						continue;
					SAfCand C;
					C.m_Type = AF_DIR;
					C.m_Dir = Dir;
					C.m_Jump = J != 0;
					if(Accept(C, S))
					{
						AfPushPathVis(C, S);
						TrackBest(C);
					}
				}
			}
		}
		// 2) hook candidates: a ring of angles, warm-started from the last good one
		if(g_Config.m_VxAfHook && g_Config.m_VxAfAimbot && !OverBudget())
		{
			const bool AnyAngle = g_Config.m_VxAfAutoAim != 0;
			const bool UseFov = g_Config.m_VxAfAimFov > 0 && g_Config.m_VxAfAimFov < 360 && AimLen > 1.0f;
			const float FovRad = (float)g_Config.m_VxAfAimFov * AF_PI / 360.0f;
			const float Start = (m_WarmTick >= 0 && NowTick - m_WarmTick <= 30) ? m_WarmAngle : AimAngle;
			const int N = std::clamp(g_Config.m_VxAfSegments, 6, 64);
			const int HookLen = std::min(AF_MAXLEN, AF_HOOK_MAXPHASE + Horizon);
			for(int k = 0; k <= (AnyAngle ? N : 0) && !OverBudget(); k++)
			{
				const float Ang = Start + (float)k * 2.0f * AF_PI / (float)N;
				if(UseFov && AfAngleDev(Ang, AimAngle) > FovRad)
					continue;
				CNetObj_PlayerInput In = User;
				In.m_Hook = 1;
				In.m_TargetX = (int)std::round(std::cos(Ang) * AF_AIM_LEN);
				In.m_TargetY = (int)std::round(std::sin(Ang) * AF_AIM_LEN);
				if(!In.m_TargetX && !In.m_TargetY)
					continue;
				SAfSim S;
				if(!AfSimulate(pGame, LocalId, &In, HookLen, S))
					continue;
				SAfCand C;
				C.m_Type = AF_HOOK;
				C.m_Hook = true;
				C.m_Angle = Ang;
				C.m_Aim = vec2((float)In.m_TargetX, (float)In.m_TargetY);
				C.m_Dir = User.m_Direction;
				C.m_HookTicks = S.m_AttachAt > 0 ? S.m_AttachAt : std::min(AF_HOOK_MAXPHASE, S.m_Len);
				if(Accept(C, S))
				{
					AfPushPathVis(C, S);
					TrackBest(C);
				}
			}
		}
	};
	// ---- decide what to do this tick ----
	enum
	{
		PH_IDLE = 0,
		PH_RESCUE,
		PH_EXIT,
		PH_NAV,
		PH_DRAG,
	} Phase = PH_IDLE;
	if(FrozenNow)
	{
		// already frozen: drag myself out towards unfreeze tiles
		if(g_Config.m_VxAfUnfreeze && !g_Config.m_VxAfNsif && NowTick >= m_NextExitTick)
		{
			Phase = PH_EXIT;
			const int ExitLen = std::min(45, SimLen);
			SAfSim BaseExit;
			const bool HaveBase = AfSimulate(pGame, LocalId, &User, ExitLen, BaseExit);
			auto FrozenUpTo = [&](const SAfSim &S, int Len) -> int {
				int Frozen = 0;
				for(int s = 1; s <= Len; s++)
					Frozen += S.m_aF[s] ? 1 : 0;
				return Frozen;
			};
			auto Accept = [&](SAfCand &C, const SAfSim &S) -> bool {
				const int Len = std::min(ExitLen, S.m_Len);
				if(Len < 20 || !HaveBase)
					return false;
				for(int s = 1; s <= Len; s++)
					if(S.m_aD[s] || (ChkTeles && S.m_aT[s]))
						return false;
				bool Unf = false;
				for(int s = 1; s <= Len; s++)
					if(S.m_aU[s])
					{
						Unf = true;
						break;
					}
				const int Frozen = FrozenUpTo(S, Len);
				const int BaseF = FrozenUpTo(BaseExit, Len);
				if(!(Unf || BaseF - Frozen >= AF_MIN_GAIN_EXIT))
					return false;
				C.m_Score = 2000.0f - (float)Frozen * 10.0f + (Unf ? 400.0f : 0.0f) + Sticky(C);
				if(C.m_Type == AF_HOOK)
					C.m_Score -= 100.0f + C.m_HookTicks * 2.0f;
				if(C.m_Dir != 0 && C.m_Dir == User.m_Direction)
					C.m_Score += 50.0f;
				return true;
			};
			RunCandidates(Accept, ExitLen);
			if(!HavePlan)
				m_NextExitTick = NowTick + AF_EXIT_INTERVAL; // nothing helps (yet): do not burn the budget every tick
		}
	}
	else if(InDanger && g_Config.m_VxAfPred)
	{
		Phase = PH_RESCUE;
		auto Accept = [&](SAfCand &C, const SAfSim &S) -> bool {
			if(S.m_Len < Horizon)
				return false;
			for(int s = 1; s <= Horizon; s++)
				if(BadAt(S, s))
					return false;
			C.m_Score = 2000.0f + Sticky(C);
			if(C.m_Type == AF_HOOK)
				C.m_Score -= 150.0f + C.m_HookTicks * 2.0f + AfAngleDev(C.m_Angle, AimAngle) / AF_PI * 80.0f;
			if(C.m_Dir != 0 && C.m_Dir == User.m_Direction)
				C.m_Score += 50.0f;
			bool Unf = false;
			for(int s = 1; s <= S.m_Len; s++)
				if(S.m_aU[s])
				{
					Unf = true;
					break;
				}
			if(Unf)
				C.m_Score += 80.0f;
			for(int s = Horizon + 1; s <= S.m_Len; s++)
			{
				if(BadAt(S, s))
				{
					C.m_Score -= 60.0f; // safe for the horizon but not much beyond
					break;
				}
			}
			return true;
		};
		RunCandidates(Accept, SimLen);
	}
	else if(m_DragHeld && g_Config.m_VxAfHook && g_Config.m_VxAfAimbot && AimLen > 1.0f)
	{
		// auto drag: continuously hook towards the cursor while it stays safe
		Phase = PH_DRAG;
		CNetObj_PlayerInput In = User;
		In.m_Hook = 1;
		In.m_Jump = 0;
		SAfSim S;
		bool Ok = false;
		const int DragLen = std::min(24, SimLen);
		if(AfSimulate(pGame, LocalId, &In, DragLen, S) && S.m_Len >= std::min(12, DragLen))
		{
			Ok = true;
			for(int s = 1; s <= S.m_Len; s++)
			{
				if(S.m_aF[s] || S.m_aD[s] || (ChkTeles && S.m_aT[s]))
				{
					Ok = false;
					break;
				}
			}
		}
		if(Ok)
		{
			SAfCand C;
			C.m_Found = true;
			C.m_Type = AF_DRAG;
			C.m_Hook = true;
			C.m_Dir = User.m_Direction;
			C.m_Angle = AimAngle;
			C.m_Aim = vec2((float)User.m_TargetX, (float)User.m_TargetY);
			C.m_Score = 1000.0f;
			AfPushPathVis(C, S);
			TrackBest(C);
		}
	}
	else if(g_Config.m_VxAfTrackPoint && m_TrackPointSet && (g_Config.m_VxAfAfk || !Afk))
	{
		const float Dist = distance(MyPos, m_TrackPoint);
		if(Dist > (m_NavActive ? AF_NAV_NEAR : AF_NAV_FAR))
		{
			Phase = PH_NAV;
			const int NavLen = std::min(30, SimLen);
			auto Accept = [&](SAfCand &C, const SAfSim &S) -> bool {
				if(S.m_Len < 12)
					return false;
				const int At = std::min(20, S.m_Len);
				if(distance(S.m_aP[At], m_TrackPoint) > distance(S.m_aP[0], m_TrackPoint) - AF_NAV_PROGRESS)
					return false; // has to actually come closer
				for(int s = 1; s <= S.m_Len; s++)
				{
					if(S.m_aD[s] || (ChkTeles && S.m_aT[s]))
						return false;
					if(g_Config.m_VxAfSafeTrack && S.m_aF[s])
						return false; // safe tracking: never move through freeze
				}
				C.m_Score = 1500.0f - distance(S.m_aP[At], m_TrackPoint) * 0.5f + Sticky(C);
				if(C.m_Type == AF_HOOK)
					C.m_Score -= 60.0f;
				return true;
			};
			RunCandidates(Accept, NavLen);
		}
	}
	// ---- aim assist of a manual hook (when nothing else is going on) ----
	bool AssistNow = false;
	if(!HavePlan && g_Config.m_VxAfAimAssist && !FrozenNow && !InDanger && m_UserHook != 0 && (HookEdge || m_AssistActive) && AimLen > 1.0f)
	{
		const bool UseFov = g_Config.m_VxAfAimFov > 0 && g_Config.m_VxAfAimFov < 360;
		const float FovRad = (float)g_Config.m_VxAfAimFov * AF_PI / 360.0f;
		const int N = std::clamp(g_Config.m_VxAfSegments, 6, 64);
		float BestDev = 1e9f;
		vec2 BestAim(0.0f, 0.0f);
		bool Found = false;
		for(int k = 0; k < N && !OverBudget(); k++)
		{
			// centre-out sweep over the cone: 0, +1, -1, +2, -2 ... halves of the FOV
			const float Frac = (float)((k + 1) / 2) / (float)N * (UseFov ? 1.0f : 0.5f);
			const float Ang = AimAngle + (((k & 1) ? 1.0f : -1.0f) * Frac * FovRad);
			CNetObj_PlayerInput In = User; // the user keeps the hook held
			In.m_Hook = 1;
			In.m_TargetX = (int)std::round(std::cos(Ang) * AF_AIM_LEN);
			In.m_TargetY = (int)std::round(std::sin(Ang) * AF_AIM_LEN);
			SAfSim S;
			if(!AfSimulate(pGame, LocalId, &In, std::min(20, SimLen), S))
				continue;
			bool Safe = S.m_Len >= std::min(20, SimLen);
			for(int s = 1; Safe && s <= S.m_Len; s++)
				if(S.m_aF[s] || S.m_aD[s] || (ChkTeles && S.m_aT[s]))
					Safe = false;
			if(Safe && AfAngleDev(Ang, AimAngle) < BestDev)
			{
				BestDev = AfAngleDev(Ang, AimAngle);
				BestAim = vec2((float)In.m_TargetX, (float)In.m_TargetY);
				Found = true;
			}
		}
		if(Found && BestDev > 0.035f) // more than ~2° off: correct the aim
		{
			pSlot->m_TargetX = (int)BestAim.x;
			pSlot->m_TargetY = (int)BestAim.y;
			AssistNow = true;
			s_AimVisFrom = MyPos;
			s_AimVisTo = MyPos + normalize(BestAim) * 900.0f;
			s_AimVisUntil = NowTick + 2;
		}
	}
	m_AssistActive = AssistNow;
	// ---- apply the plan ----
	if(HavePlan)
	{
		m_PlanType = Best.m_Type;
		m_PlanDir = Best.m_Dir;
		m_PlanJump = Best.m_Jump;
		m_PlanHook = Best.m_Hook;
		m_PlanAim = Best.m_Aim;
		m_PlanAngle = Best.m_Angle;
		m_PlanScore = Best.m_Score;
		m_PlanTick = NowTick;
		pSlot->m_Direction = Best.m_Dir;
		pSlot->m_Jump = Best.m_Jump ? 1 : 0;
		m_WroteJump = pSlot->m_Jump;
		if(Best.m_Hook)
		{
			pSlot->m_Hook = 1;
			pSlot->m_TargetX = (int)Best.m_Aim.x;
			pSlot->m_TargetY = (int)Best.m_Aim.y;
			m_WroteHook = 1;
		}
		else
		{
			pSlot->m_Hook = 0;
			m_WroteHook = 0;
		}
		if(Best.m_Type == AF_HOOK)
		{
			m_WarmAngle = Best.m_Angle;
			m_WarmTick = NowTick;
			s_AimVisFrom = MyPos;
			s_AimVisTo = MyPos + normalize(Best.m_Aim) * 900.0f;
			s_AimVisUntil = NowTick + 2;
		}
		AfSetPathVis(Best.m_aPath, Best.m_NumPath, NowTick + 6);
		if(Best.m_NumPath > 0)
		{
			s_TrackVisPos = Best.m_aPath[Best.m_NumPath - 1];
			s_TrackVisUntil = NowTick + 6;
		}
		if(Phase == PH_EXIT)
			m_ExitActive = true;
		else if(Phase == PH_NAV)
			m_NavActive = true;
		else if(Phase == PH_DRAG)
			m_DragActive = true;
		else
			m_RescueActive = true;
		if(Debug && !WasActive)
			dbg_msg("avoid-freeze", "SAVE: phase=%d enter=%d type=%d dir=%d jump=%d hook=%d angle=%.1f score=%.0f afk=%d",
				(int)Phase, DangerT, Best.m_Type, Best.m_Dir, (int)Best.m_Jump, (int)Best.m_Hook,
				Best.m_Angle * 180.0f / AF_PI, Best.m_Score, (int)Afk);
	}
	else
	{
		m_PlanType = AF_NONE;
		m_RescueActive = false;
		m_ExitActive = false;
		m_NavActive = false;
		m_DragActive = false;
		m_WroteJump = m_UserJump;
		m_WroteHook = m_UserHook;
	}
	// ---- release: give the control back exactly as it was ----
	const bool NowActive = AnyActive();
	if(WasActive && !NowActive)
	{
		RestoreSlot(pSlot);
		if(Debug)
			dbg_msg("avoid-freeze", "RELEASE: danger=%d frozen=%d", DangerT, (int)FrozenNow);
	}
	// ---- periodic debug line ----
	if(Debug && NowTick - m_LogGateTick >= 50)
	{
		m_LogGateTick = NowTick;
		dbg_msg("avoid-freeze", "state: phase=%d danger=%d frozen=%d afk=%d pos=(%.0f,%.0f) tp=%d budget=%lldus",
			(int)Phase, DangerT, (int)FrozenNow, (int)Afk, MyPos.x, MyPos.y, (int)m_TrackPointSet,
			(long long)((time_get() - T0) * 1000000 / time_freq()));
	}
}
// ============================================================================
// binds (called once per render frame from CControls::OnRender)
// ============================================================================
void CAvoidFreeze::StartBind(int Mode)
{
	m_BindMode = Mode;
	m_BindBlockUntil = time_get() + time_freq() / 5; // ignore the click that started the capture
}
void CAvoidFreeze::RenderBinds(CGameClient *pGame)
{
	IInput *pInput = pGame->Input();
	const int64_t Now = time_get();
	// capture a key for a pending bind (started from the settings tab)
	if(m_BindMode != BIND_NONE)
	{
		if(Now < m_BindBlockUntil)
			return;
		for(int Key = 1; Key <= KEY_MOUSE_9; Key++)
		{
			if(Key == KEY_MOUSE_1 || !pInput->KeyPress(Key))
				continue;
			int *pTarget = m_BindMode == BIND_BASIC ? &g_Config.m_VxAfBasicBind : m_BindMode == BIND_DRAG ? &g_Config.m_VxAfDragBind : &g_Config.m_VxAfTpBind;
			*pTarget = Key == KEY_ESCAPE ? -1 : Key;
			if(*pTarget > 0)
			{
				// the same key belongs to only one Vertex avoid bind
				int *apAll[3] = {&g_Config.m_VxAfBasicBind, &g_Config.m_VxAfDragBind, &g_Config.m_VxAfTpBind};
				for(int *pOther : apAll)
				{
					if(pOther != pTarget && *pOther == *pTarget)
						*pOther = -1;
				}
			}
			m_BindMode = BIND_NONE;
			m_BindBlockUntil = Now + time_freq() / 3;
			break;
		}
		return;
	}
	// action binds do not work while the chat / a menu is open
	if(pGame->m_Chat.IsActive() || pGame->m_Menus.IsActive())
	{
		m_DragHeld = false;
		return;
	}
	if(g_Config.m_VxAfBasicBind > 0 && pInput->KeyPress(g_Config.m_VxAfBasicBind))
	{
		g_Config.m_VxAfEnable = g_Config.m_VxAfEnable ? 0 : 1;
		char aBuf[64];
		str_format(aBuf, sizeof(aBuf), "Avoid: %s", g_Config.m_VxAfEnable ? "on" : "off");
		pGame->Echo(aBuf);
	}
	m_DragHeld = g_Config.m_VxAfDragBind > 0 && pInput->KeyIsPressed(g_Config.m_VxAfDragBind);
	if(g_Config.m_VxAfTpBind > 0 && pInput->KeyPress(g_Config.m_VxAfTpBind))
	{
		const vec2 Pos = pGame->m_PredictedChar.m_Pos;
		if(!m_TrackPointSet)
		{
			m_TrackPoint = Pos;
			m_TrackPointSet = true;
			pGame->Echo("Track point: set");
		}
		else if(distance(m_TrackPoint, Pos) > 150.0f)
		{
			m_TrackPoint = Pos;
			pGame->Echo("Track point: moved");
		}
		else
		{
			m_TrackPointSet = false;
			pGame->Echo("Track point: cleared");
		}
	}
}
// ============================================================================
// world overlay (call from the world render, e.g. players.cpp near the dummy
// unfreeze ESP): plan path, hook aim line, track point / plan target
// ============================================================================
void CAvoidFreeze::RenderWorld(CGameClient *pGame)
{
	if(g_Config.m_VxAfMode != 2 || g_Config.m_VxAfEnable == 0 || g_Config.m_VxAfBlatant == 0)
		return;
	const int NowTick = pGame->m_PredictedWorld.GameTick();
	IGraphics *pGraphics = pGame->Graphics();
	pGraphics->TextureClear();
	pGraphics->LinesBegin();
	if(g_Config.m_VxAfVisPath && s_PathVisUntil >= NowTick && s_PathVisN > 1)
	{
		pGraphics->SetColor(1.0f, 1.0f, 1.0f, 0.35f);
		for(int i = 0; i + 1 < s_PathVisN; i++)
		{
			const IGraphics::CLineItem L(s_aPathVis[i].x, s_aPathVis[i].y, s_aPathVis[i + 1].x, s_aPathVis[i + 1].y);
			pGraphics->LinesDraw(&L, 1);
		}
	}
	if(g_Config.m_VxAfVisAim && s_AimVisUntil >= NowTick)
	{
		pGraphics->SetColor(1.0f, 0.35f, 0.35f, 0.65f);
		const IGraphics::CLineItem L(s_AimVisFrom.x, s_AimVisFrom.y, s_AimVisTo.x, s_AimVisTo.y);
		pGraphics->LinesDraw(&L, 1);
	}
	if(g_Config.m_VxAfVisTrack)
	{
		// the saved track point (green) and the current plan target (yellow)
		if(m_TrackPointSet)
		{
			pGraphics->SetColor(0.4f, 1.0f, 0.5f, 0.85f);
			const float r = 7.0f;
			const vec2 P = m_TrackPoint;
			const IGraphics::CLineItem aL[2] = {
				IGraphics::CLineItem(P.x - r, P.y, P.x + r, P.y),
				IGraphics::CLineItem(P.x, P.y - r, P.x, P.y + r)};
			pGraphics->LinesDraw(aL, 2);
		}
		if(s_TrackVisUntil >= NowTick)
		{
			pGraphics->SetColor(1.0f, 0.9f, 0.3f, 0.85f);
			const float r = 5.0f;
			const vec2 P = s_TrackVisPos;
			const IGraphics::CLineItem aL[2] = {
				IGraphics::CLineItem(P.x - r, P.y, P.x + r, P.y),
				IGraphics::CLineItem(P.x, P.y - r, P.x, P.y + r)};
			pGraphics->LinesDraw(aL, 2);
		}
	}
	pGraphics->LinesEnd();
}
