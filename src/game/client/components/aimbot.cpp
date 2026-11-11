#include "aimbot.h"

#include <algorithm>
#include <limits>
#include <cmath>
#include <vector>
#include <utility>
#include <cstdint>
#include <unordered_set>

#include <base/color.h>
#include <engine/client.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <game/client/animstate.h>
#include <game/client/render.h>
#include <game/client/components/controls.h>
#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/prediction/gameworld.h>
#include <generated/protocol7.h>
#include <game/collision.h>
#include <game/gamecore.h>
#include <game/mapitems.h>
#include <game/tuning.h>

namespace
{
constexpr float VERTEX_PI = 3.14159265358979323846f;
constexpr float VERTEX_TWO_PI = 2.0f * VERTEX_PI;
constexpr float AIM_EPSILON = 0.001f;
constexpr float HOOK_PLAYER_HIT_RADIUS = CCharacterCore::PhysicalSize() + 2.0f;
constexpr float HOOK_START_OFFSET = CCharacterCore::PhysicalSize() * 1.5f;
constexpr float HAMMER_REACH = 63.0f;
constexpr float PROJECTILE_SPAWN_OFFSET = 21.0f;
constexpr float GRENADE_EXPLOSION_RADIUS = 135.0f;
constexpr float GRENADE_FULL_STRENGTH_RADIUS = 48.0f;
constexpr int FULL_CIRCLE_SEGMENTS = 64;
constexpr int MAX_PROJECTILE_TICKS = 250;
constexpr int MAX_BEAM_BOUNCES = 6;
constexpr int MAX_HOOK_PREDICTION_TICKS = 128;

float GrenadeNonZeroImpulseRadius(float ExplosionStrength)
{
	if(ExplosionStrength < 1.0f)
		return 0.0f;
	return std::clamp(GRENADE_EXPLOSION_RADIUS -
		(GRENADE_EXPLOSION_RADIUS - GRENADE_FULL_STRENGTH_RADIUS) / ExplosionStrength,
		GRENADE_FULL_STRENGTH_RADIUS, GRENADE_EXPLOSION_RADIUS);
}

float ClampFov(float FovDegrees)
{
	return std::clamp(FovDegrees, 1.0f, 360.0f);
}

float HalfFovRad(float FovDegrees)
{
	return std::clamp(FovDegrees, 1.0f, 315.0f) * 0.01f;
}

// Produce the exact integer aim vector that will be written to CNetObj_PlayerInput.
// The normalization/rounding loop matches the final packet conversion in ApplyAimbot.
bool QuantizeHookInputDirection(float Angle, float Scale, int *pTargetX, int *pTargetY, vec2 *pDirection)
{
	if(!pTargetX || !pTargetY || !pDirection)
		return false;
	Scale = std::max(Scale, 1.0f);
	int TargetX = static_cast<int>(std::lround(std::cos(Angle) * Scale));
	int TargetY = static_cast<int>(std::lround(std::sin(Angle) * Scale));
	if(TargetX == 0 && TargetY == 0)
		return false;
	for(int Pass = 0; Pass < 8; ++Pass)
	{
		const vec2 Unit = normalize(vec2(static_cast<float>(TargetX), static_cast<float>(TargetY)));
		const int NextX = static_cast<int>(std::lround(Unit.x * Scale));
		const int NextY = static_cast<int>(std::lround(Unit.y * Scale));
		if(NextX == TargetX && NextY == TargetY)
		{
			*pTargetX = TargetX;
			*pTargetY = TargetY;
			*pDirection = Unit;
			return true;
		}
		if(NextX == 0 && NextY == 0)
			return false;
		TargetX = NextX;
		TargetY = NextY;
	}
	return false;
}

float AngleDifferenceDegrees(vec2 A, vec2 B)
{
	if(length(A) <= AIM_EPSILON || length(B) <= AIM_EPSILON)
		return 180.0f;
	const float AngleA = std::atan2(A.y, A.x);
	const float AngleB = std::atan2(B.y, B.x);
	return std::fabs(std::remainder(AngleA - AngleB, VERTEX_TWO_PI)) * 180.0f / VERTEX_PI;
}

bool IsInsideFov(vec2 AimDirection, vec2 ToTarget, float FovDegrees)
{
	if(length(AimDirection) <= AIM_EPSILON || length(ToTarget) <= AIM_EPSILON)
		return false;
	return AngleDifferenceDegrees(AimDirection, ToTarget) <= ClampFov(FovDegrees) * 0.5f;
}

ColorRGBA ConfigColor(unsigned int Hsla)
{
	return color_cast<ColorRGBA>(ColorHSLA(Hsla));
}

void SetGraphicsColor(IGraphics *pGraphics, ColorRGBA Color, float AlphaMultiplier = 1.0f)
{
	pGraphics->SetColor(Color.r, Color.g, Color.b, Color.a * AlphaMultiplier);
}

void DrawCross(IGraphics *pGraphics, vec2 Pos, float Size)
{
	const IGraphics::CLineItem Lines[2] = {
		{Pos.x - Size, Pos.y, Pos.x + Size, Pos.y},
		{Pos.x, Pos.y - Size, Pos.x, Pos.y + Size},
	};
	pGraphics->LinesDraw(Lines, 2);
}

void DrawCircle(IGraphics *pGraphics, vec2 Center, float Radius, int Segments)
{
	if(Segments < 3)
		return;
	std::vector<IGraphics::CLineItem> Lines;
	Lines.reserve(static_cast<size_t>(Segments));
	for(int i = 0; i < Segments; ++i)
	{
		const float A0 = VERTEX_TWO_PI * static_cast<float>(i) / static_cast<float>(Segments);
		const float A1 = VERTEX_TWO_PI * static_cast<float>(i + 1) / static_cast<float>(Segments);
		Lines.emplace_back(
			Center.x + std::cos(A0) * Radius,
			Center.y + std::sin(A0) * Radius,
			Center.x + std::cos(A1) * Radius,
			Center.y + std::sin(A1) * Radius);
	}
	pGraphics->LinesDraw(Lines.data(), static_cast<int>(Lines.size()));
}

void DrawFovCone(IGraphics *pGraphics, CCollision *pCollision, vec2 Origin, vec2 AimDirection,
	float FovDegrees, float DrawLength, float HookSpeed, ColorRGBA Color)
{
	if(!pGraphics || !pCollision || length(AimDirection) <= AIM_EPSILON || DrawLength <= 1.0f)
		return;
	(void)HookSpeed;
	const float Mid = std::atan2(AimDirection.y, AimDirection.x);
	const float Half = HalfFovRad(FovDegrees);
	IGraphics::CLineItem aLines[2];
	for(int Side = 0; Side < 2; ++Side)
	{
		const float A = Mid + (Side == 0 ? -Half : Half);
		const vec2 Far = Origin + vec2(std::cos(A), std::sin(A)) * DrawLength;
		vec2 Hit, Before;
		const vec2 End = pCollision->IntersectLineTeleHook(Origin, Far, &Hit, &Before) != 0 ? Hit : Far;
		aLines[Side] = IGraphics::CLineItem{Origin.x, Origin.y, End.x, End.y};
	}
	SetGraphicsColor(pGraphics, Color, 0.78f);
	pGraphics->LinesDraw(aLines, 2);
	SetGraphicsColor(pGraphics, Color, 1.0f);
}

// Restored helper used by the original s_aEspOffsets ESP check.
bool TraceStraightHookToPoint(CCollision *pCollision, vec2 Origin, vec2 Point,
	float HookSpeed, vec2 *pVisibleEnd)
{
	(void)HookSpeed;
	if(pVisibleEnd)
		*pVisibleEnd = Origin;
	if(!pCollision)
		return false;
	vec2 CollisionPos;
	vec2 BeforeCollisionPos;
	const int Hit = pCollision->IntersectLineTeleHook(Origin, Point, &CollisionPos, &BeforeCollisionPos);
	if(Hit != 0)
	{
		if(pVisibleEnd)
			*pVisibleEnd = BeforeCollisionPos;
		return false;
	}
	if(pVisibleEnd)
		*pVisibleEnd = Point;
	return true;
}

bool IsHookLineVisible(CCollision *pCollision, vec2 Origin, vec2 Point, float HookSpeed)
{
	return TraceStraightHookToPoint(pCollision, Origin, Point, HookSpeed, nullptr);
}

void DrawPlayerBox(IGraphics *pGraphics, vec2 Center, ColorRGBA Color)
{
	// Compact full-tee outline: narrower sides and less empty space under feet.
	constexpr float HalfWidth = 17.0f;
	constexpr float TopOffset = -26.0f;
	constexpr float BottomOffset = 15.0f;
	const float Left = Center.x - HalfWidth;
	const float Right = Center.x + HalfWidth;
	const float Top = Center.y + TopOffset;
	const float Bottom = Center.y + BottomOffset;
	const IGraphics::CLineItem Box[4] = {
		{Left, Top, Right, Top},
		{Right, Top, Right, Bottom},
		{Right, Bottom, Left, Bottom},
		{Left, Bottom, Left, Top},
	};
	pGraphics->LinesDraw(Box, 4);
}

void DrawProjectilePath(IGraphics *pGraphics, CCollision *pCollision, vec2 Start, vec2 Direction,
	float Curvature, float Speed, float LifetimeSeconds, ColorRGBA Color, bool Explosive,
	bool ShowExplosionCircle = false, float ExplosionStrength = 0.0f)
{
	if(!pCollision || Speed <= 1.0f || length(Direction) <= AIM_EPSILON)
		return;

	Direction = normalize(Direction);
	const int LifetimeTicks = std::clamp(static_cast<int>(std::lround(LifetimeSeconds * 50.0f)), 1, MAX_PROJECTILE_TICKS);
	vec2 Previous = Start;
	vec2 Impact = Start;
	std::vector<IGraphics::CLineItem> Lines;
	Lines.reserve(static_cast<size_t>(LifetimeTicks));

	for(int Tick = 1; Tick <= LifetimeTicks; ++Tick)
	{
		const vec2 Current = CalcPos(Start, Direction, Curvature, Speed, static_cast<float>(Tick) / 50.0f);
		vec2 CollisionPos;
		vec2 BeforeCollisionPos;
		const int Hit = pCollision->IntersectLine(Previous, Current, &CollisionPos, &BeforeCollisionPos);
		if(Hit != 0)
		{
			Lines.emplace_back(Previous.x, Previous.y, BeforeCollisionPos.x, BeforeCollisionPos.y);
			Impact = CollisionPos;
			break;
		}
		Lines.emplace_back(Previous.x, Previous.y, Current.x, Current.y);
		Previous = Current;
		Impact = Current;
	}

	SetGraphicsColor(pGraphics, Color, 0.85f);
	if(!Lines.empty())
		pGraphics->LinesDraw(Lines.data(), static_cast<int>(Lines.size()));
	if(Explosive && ShowExplosionCircle)
	{
		// The server computes explosion force as Strength * falloff and ignores
		// values whose integer part is zero. Draw the non-zero impulse boundary
		// for the active tuning zone as the main ring.
		const float EffectiveRadius = GrenadeNonZeroImpulseRadius(ExplosionStrength);
		// Draw one ring only: it marks the outer edge where the server's
		// integer explosion strength is still at least one. Do not draw the
		// larger visual-only 135/48 guide circles around it.
		if(EffectiveRadius > 0.0f)
		{
			SetGraphicsColor(pGraphics, Color, 0.78f);
			DrawCircle(pGraphics, Impact, EffectiveRadius, 56);
		}
	}
	SetGraphicsColor(pGraphics, Color, 1.0f);
	DrawCross(pGraphics, Impact, 6.0f);
}

void DrawBeamPath(IGraphics *pGraphics, CCollision *pCollision, vec2 Start, vec2 Direction,
	float Reach, float BounceCost, int MaxBounces, ColorRGBA Color)
{
	if(!pCollision || Reach <= 1.0f || length(Direction) <= AIM_EPSILON)
		return;

	vec2 Pos = Start;
	vec2 Dir = normalize(Direction);
	float Energy = Reach;
	std::vector<IGraphics::CLineItem> Lines;
	Lines.reserve(static_cast<size_t>(MAX_BEAM_BOUNCES + 1));
	vec2 Impact = Start;

	for(int Bounce = 0; Bounce <= std::clamp(MaxBounces, 0, MAX_BEAM_BOUNCES) && Energy > 8.0f; ++Bounce)
	{
		const vec2 To = Pos + Dir * Energy;
		vec2 CollisionPos;
		vec2 BeforeCollisionPos;
		const int Hit = pCollision->IntersectLineTeleWeapon(Pos, To, &CollisionPos, &BeforeCollisionPos);
		const vec2 SegmentEnd = Hit ? BeforeCollisionPos : To;
		Lines.emplace_back(Pos.x, Pos.y, SegmentEnd.x, SegmentEnd.y);
		Impact = Hit ? CollisionPos : SegmentEnd;
		if(!Hit)
			break;

		const float SegmentLength = distance(Pos, SegmentEnd);
		Energy -= SegmentLength + BounceCost;
		if(Energy <= 8.0f || Bounce == MaxBounces)
			break;

		vec2 NextPos = SegmentEnd;
		vec2 NextDir = Dir * 4.0f;
		pCollision->MovePoint(&NextPos, &NextDir, 1.0f, nullptr);
		if(length(NextDir) <= AIM_EPSILON)
			break;
		Pos = NextPos;
		Dir = normalize(NextDir);
	}

	SetGraphicsColor(pGraphics, Color, 0.90f);
	if(!Lines.empty())
		pGraphics->LinesDraw(Lines.data(), static_cast<int>(Lines.size()));
	SetGraphicsColor(pGraphics, Color, 1.0f);
	DrawCross(pGraphics, Impact, 6.0f);
}
} // namespace

bool CAimbot::HasLineOfSight(CCollision *pCollision, vec2 From, vec2 To, ELineCheck Check) const
{
	if(!pCollision)
		return false;
	vec2 CollisionPos;
	vec2 BeforeCollisionPos;
	int Hit = 0;
	switch(Check)
	{
	case ELineCheck::HOOK:
		Hit = pCollision->IntersectLineTeleHook(From, To, &CollisionPos, &BeforeCollisionPos);
		break;
	case ELineCheck::WEAPON:
		Hit = pCollision->IntersectLineTeleWeapon(From, To, &CollisionPos, &BeforeCollisionPos);
		break;
	case ELineCheck::NORMAL:
	default:
		Hit = pCollision->IntersectLine(From, To, &CollisionPos, &BeforeCollisionPos);
		break;
	}
	return Hit == 0;
}

bool CAimbot::FindTargetInFov(CGameClient *pGameClient, CCharacter *pLocalCharacter,
	vec2 AimDirection, float FovDegrees, float MaxDistance, ELineCheck LineCheck,
	CCharacter **ppTarget, int *pTargetId, vec2 *pTargetPoint, int HookSearchSamples, float AimInputScale, const CNetObj_PlayerInput *pLocalInput)
{
	if(!pGameClient || !pLocalCharacter || !ppTarget || !pTargetId || !pTargetPoint || length(AimDirection) <= AIM_EPSILON)
		return false;

	const int LocalId = pGameClient->m_Snap.m_LocalClientId;
	vec2 LocalPos = pLocalCharacter->Core()->m_Pos;
	if(LineCheck == ELineCheck::HOOK)
		LocalPos = vec2(std::round(LocalPos.x), std::round(LocalPos.y));
	const float HalfFov = ClampFov(FovDegrees) * 0.5f;

	// Hook targeting must match CCharacterCore::Tick, not a line-of-sight test
	// to the tee center. For HOOK, the output point is the exact tested ray
	// direction; for other line checks it remains a target point. The hook is
	// checked against the tee's circular hitbox
	// on the segment that remains after IntersectLineTeleHook clips the ray.
	// This is what allows a manual hook to catch a tee whose center is behind
	// a corner while the upper/side edge of its hit circle is still reachable.
	if(LineCheck == ELineCheck::HOOK)
	{
		CCollision *pCollision = pLocalCharacter->Collision();
		const CCharacterCore *pLocalCore = pLocalCharacter->Core();
		if(LocalId < 0 || !pCollision || MaxDistance <= HOOK_START_OFFSET + 1.0f ||
			pLocalCore->m_HookHitDisabled || !pLocalCore->m_Tuning.m_PlayerHooking ||
			pLocalCore->m_HookState != HOOK_IDLE)
			return false;

		const float AimAngle = std::atan2(AimDirection.y, AimDirection.x);
		const float HalfFov = HalfFovRad(FovDegrees);
		const float InitialHookSpeed = pLocalCore->m_Tuning.m_HookFireSpeed;
		if(InitialHookSpeed <= 0.01f)
			return false;

		const int RequestedSegments = std::clamp(static_cast<int>(std::ceil(
			(MaxDistance - HOOK_START_OFFSET) / InitialHookSpeed)) + 2, 1, 512);
		const bool PredictHookMovement = g_Config.m_VxAimHookPredict != 0;
		const int PredictionTicks = std::min(RequestedSegments - 1, MAX_HOOK_PREDICTION_TICKS);
		const int MaxSegments = PredictionTicks + 1;

		// Predict actual character motion with a detached copy of DDNet's predicted
		// world. CGameWorld::Tick runs DDRaceTick, CCharacterCore::Tick/Move and
		// Quantize, including map collisions, tune zones and player interactions.
		vec2 aPredictedLocalPos[MAX_HOOK_PREDICTION_TICKS + 1] = {};
		float aHookSpeed[MAX_HOOK_PREDICTION_TICKS + 1] = {};
		float aHookLength[MAX_HOOK_PREDICTION_TICKS + 1] = {};
		vec2 aPredictedTargetPos[MAX_CLIENTS][MAX_HOOK_PREDICTION_TICKS + 1] = {};
		bool aPredictedTargetValid[MAX_CLIENTS][MAX_HOOK_PREDICTION_TICKS + 1] = {};
		bool aPredictedHookAllowed[MAX_CLIENTS][MAX_HOOK_PREDICTION_TICKS + 1] = {};
		bool aPredictedLocalValid[MAX_HOOK_PREDICTION_TICKS + 1] = {};
		bool aPredictedLocalHookAllowed[MAX_HOOK_PREDICTION_TICKS + 1] = {};

		aPredictedLocalPos[0] = LocalPos;
		aHookSpeed[0] = pLocalCore->m_Tuning.m_HookFireSpeed;
		aHookLength[0] = pLocalCore->m_Tuning.m_HookLength;
		aPredictedLocalValid[0] = true;
		aPredictedLocalHookAllowed[0] = !pLocalCore->m_HookHitDisabled && pLocalCore->m_Tuning.m_PlayerHooking;
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
		{
			if(ClientId == LocalId)
				continue;
			CCharacter *pTarget = pGameClient->m_PredictedWorld.GetCharacterById(ClientId);
			if(!pTarget)
				continue;
			const vec2 TargetPos = pTarget->Core()->m_Pos;
			aPredictedTargetPos[ClientId][0] = vec2(std::round(TargetPos.x), std::round(TargetPos.y));
			aPredictedTargetValid[ClientId][0] = true;
			const CCharacterCore *pTargetCore = pTarget->Core();
			aPredictedHookAllowed[ClientId][0] = (pLocalCore->m_Super || pTargetCore->m_Super) ||
				(pGameClient->m_Teams.CanCollide(ClientId, LocalId) && !pLocalCore->m_Solo && !pTargetCore->m_Solo);
		}

		if(PredictHookMovement && PredictionTicks > 0)
		{
			// CopyWorld is the same detached prediction mechanism used by DDNet's
			// controls code. Tick() runs DDRaceTick, Core::Tick, Move and Quantize.
			CGameWorld SimWorld;
			SimWorld.CopyWorld(&pGameClient->m_PredictedWorld);
			// CopyWorld does not copy m_LocalClientId; initialize it before Tick().
			SimWorld.m_LocalClientId = LocalId;
			CCharacter *pSimLocal = SimWorld.GetCharacterById(LocalId);
			if(pSimLocal)
			{
				CNetObj_PlayerInput SimInput = pLocalInput ? *pLocalInput : pSimLocal->GetCore().m_Input;
				// Trace each candidate hook separately. Keep local movement/jump inputs,
				// but don't launch a second hook in the prediction world.
				SimInput.m_Hook = 0;
				// The simulation predicts movement only; never fire weapons in the copy.
				SimInput.m_Fire = 0;
				SimInput.m_NextWeapon = 0;
				SimInput.m_PrevWeapon = 0;
				pSimLocal->OnPredictedInput(&SimInput);

				const int StartTick = SimWorld.GameTick();
				for(int Tick = 1; Tick <= PredictionTicks; ++Tick)
				{
					SimWorld.m_GameTick = StartTick + Tick;
					SimWorld.Tick();
					pSimLocal = SimWorld.GetCharacterById(LocalId);
					if(!pSimLocal)
						break;

					// HandleTuneLayer() runs at the start of the next DDRace tick.
					// Resolve the tune zone at this predicted position now, so the hook
					// speed/length used for this track index match that next tick.
					const int TuneZone = SimWorld.m_WorldConfig.m_UseTuneZones ?
						SimWorld.Collision()->IsTune(SimWorld.Collision()->GetMapIndex(pSimLocal->m_Pos)) : 0;
					pSimLocal->SetTuneZone(TuneZone);
					CCharacterCore PredictedCore = pSimLocal->GetCore();
				PredictedCore.m_Tuning = *SimWorld.GetTuning(pSimLocal->GetOverriddenTuneZone());
				pSimLocal->SetCore(PredictedCore);

					aPredictedLocalPos[Tick] = pSimLocal->Core()->m_Pos;
					aHookSpeed[Tick] = pSimLocal->Core()->m_Tuning.m_HookFireSpeed;
					aHookLength[Tick] = pSimLocal->Core()->m_Tuning.m_HookLength;
					aPredictedLocalValid[Tick] = true;
					aPredictedLocalHookAllowed[Tick] = !pSimLocal->Core()->m_HookHitDisabled && pSimLocal->Core()->m_Tuning.m_PlayerHooking;
					for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
					{
						if(ClientId == LocalId || !aPredictedTargetValid[ClientId][Tick - 1])
							continue;
						CCharacter *pTarget = SimWorld.GetCharacterById(ClientId);
						if(!pTarget)
							continue;
						const CCharacterCore *pTargetCore = pTarget->Core();
						const vec2 TargetPos = pTargetCore->m_Pos;
						aPredictedTargetPos[ClientId][Tick] = vec2(std::round(TargetPos.x), std::round(TargetPos.y));
						aPredictedTargetValid[ClientId][Tick] = true;
						aPredictedHookAllowed[ClientId][Tick] = (pSimLocal->Core()->m_Super || pTargetCore->m_Super) ||
							(SimWorld.m_Teams.CanCollide(ClientId, LocalId) && !pSimLocal->Core()->m_Solo && !pTargetCore->m_Solo);
					}
				}
			}
		}
		else if(!PredictHookMovement)
		{
			// Prediction disabled: keep both tees at their current positions for the
			// complete hook flight, without copying/simulating the whole world.
			for(int Tick = 1; Tick <= PredictionTicks; ++Tick)
			{
				aPredictedLocalPos[Tick] = LocalPos;
				aHookSpeed[Tick] = InitialHookSpeed;
				aHookLength[Tick] = pLocalCore->m_Tuning.m_HookLength;
				aPredictedLocalValid[Tick] = true;
				aPredictedLocalHookAllowed[Tick] = aPredictedLocalHookAllowed[0];
				for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
				{
					if(ClientId == LocalId || !aPredictedTargetValid[ClientId][0])
						continue;
					aPredictedTargetPos[ClientId][Tick] = aPredictedTargetPos[ClientId][0];
					aPredictedTargetValid[ClientId][Tick] = true;
					aPredictedHookAllowed[ClientId][Tick] = aPredictedHookAllowed[ClientId][0];
				}
			}
		}

		auto TraceHookRay = [&](float Angle, CCharacter **ppHitTarget, int *pHitId,
			vec2 *pHitDirection, float *pHitDistance) -> bool {
			const float AngleFromCrosshair = std::fabs(std::remainder(Angle - AimAngle, VERTEX_TWO_PI));
			if(AngleFromCrosshair > HalfFov + 0.000001f)
				return false;

			int PacketX = 0;
			int PacketY = 0;
			vec2 PacketDirection;
			if(!QuantizeHookInputDirection(Angle, AimInputScale, &PacketX, &PacketY, &PacketDirection))
				return false;
			vec2 HookDirection = PacketDirection;
			vec2 HookPos = LocalPos + PacketDirection * HOOK_START_OFFSET;
			vec2 TeleHookBase(0.0f, 0.0f);
			bool HasTeleHookBase = false;
			// After the launch tick, CCharacterCore uses the current input target
			// direction if the hook enters a teleporter. SnapInput's current target
			// is the manual cursor direction; the launch tick itself uses PacketDirection.
			const vec2 PredictedTeleDirection = PacketDirection;

			for(int Segment = 0; Segment < MaxSegments; ++Segment)
			{
				if(!aPredictedLocalValid[Segment])
					break;
				const float HookSpeed = aHookSpeed[Segment];
				const float HookLength = aHookLength[Segment];
				if(HookSpeed <= 0.01f || HookLength <= 0.01f)
					break;
				const vec2 HookBase = HasTeleHookBase ? TeleHookBase : aPredictedLocalPos[Segment];
				vec2 NewPos = HookPos + HookDirection * HookSpeed;
				const bool RangeLimit = distance(HookBase, NewPos) > HookLength;
				if(RangeLimit)
					NewPos = HookBase + normalize(NewPos - HookBase) * HookLength;

				int TeleNr = 0;
				const int TileHit = pCollision->IntersectLineTeleHook(HookPos, NewPos, &NewPos, nullptr, &TeleNr);

				CCharacter *pHitTarget = nullptr;
				int HitId = -1;
				float BestDistance = std::numeric_limits<float>::max();
				float BestOriginDistance = 0.0f;
				for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
				{
					// In the native core a range-limited hook can still hit a player
					// on this clipped segment only before it has teleported (m_NewHook == false).
					if(ClientId == LocalId || !aPredictedLocalHookAllowed[Segment] || !aPredictedTargetValid[ClientId][Segment] || (RangeLimit && HasTeleHookBase))
						continue;
					CCharacter *pTarget = pGameClient->m_PredictedWorld.GetCharacterById(ClientId);
					if(!pTarget)
						continue;

					if(!aPredictedHookAllowed[ClientId][Segment])
						continue;

					const vec2 TargetPos = aPredictedTargetPos[ClientId][Segment];
					vec2 ClosestPoint;
					if(!closest_point_on_line(HookPos, NewPos, TargetPos, ClosestPoint))
						continue;
					if(distance(TargetPos, ClosestPoint) >= HOOK_PLAYER_HIT_RADIUS - 0.75f)
						continue;

					const float TargetDistance = distance(HookPos, TargetPos);
					if(TargetDistance < BestDistance)
					{
						BestDistance = TargetDistance;
						BestOriginDistance = distance(LocalPos, TargetPos);
						pHitTarget = pTarget;
						HitId = ClientId;
					}
				}

				// CCharacterCore::Tick tests players on the clipped part of this segment
				// before it processes solid/no-hook/teleport tiles.
				if(pHitTarget)
				{
					if(ppHitTarget)
						*ppHitTarget = pHitTarget;
					if(pHitId)
						*pHitId = HitId;
					if(pHitDirection)
						*pHitDirection = PacketDirection;
					if(pHitDistance)
						*pHitDistance = BestOriginDistance;
					return true;
				}

				if(!RangeLimit && TileHit == TILE_TELEINHOOK && TeleNr > 0)
				{
					const auto &vTeleOuts = pCollision->TeleOuts(TeleNr - 1);
					// Multiple exits use the world's random state. Do not guess which
					// exit the server will choose, or claim a hit beyond that teleport.
					if(vTeleOuts.size() != 1)
						break;
					const vec2 TeleDirection = Segment == 0 ? PacketDirection : PredictedTeleDirection;
					TeleHookBase = vTeleOuts[0] + TeleDirection * HOOK_START_OFFSET;
					HasTeleHookBase = true;
					HookPos.x = static_cast<float>(round_to_int(TeleHookBase.x));
					HookPos.y = static_cast<float>(round_to_int(TeleHookBase.y));
					HookDirection.x = static_cast<float>(round_to_int(TeleDirection.x * 256.0f)) / 256.0f;
					HookDirection.y = static_cast<float>(round_to_int(TeleDirection.y * 256.0f)) / 256.0f;
					continue;
				}
				if(TileHit != 0 || RangeLimit)
					break;

				// CCharacter::TickDeferred calls CCharacterCore::Quantize after each tick.
				NewPos.x = static_cast<float>(round_to_int(NewPos.x));
				NewPos.y = static_cast<float>(round_to_int(NewPos.y));
				if(distance(HookPos, NewPos) <= AIM_EPSILON)
					break;
				HookDirection.x = static_cast<float>(round_to_int(HookDirection.x * 256.0f)) / 256.0f;
				HookDirection.y = static_cast<float>(round_to_int(HookDirection.y * 256.0f)) / 256.0f;
				HookPos = NewPos;
			}
			return false;
		};
        const int BaseSamples = std::clamp(std::max(HookSearchSamples, 85), 85, 1024);
        // Generate candidate packet directions around every predicted target position.
        const float MaxAngularStep = std::clamp(0.35f / std::max(1.0f, AimInputScale), 0.0001f, 0.002f);
        static std::vector<std::pair<float, float>> s_vAngles;
        static std::unordered_set<std::uint64_t> s_SeenPacketDirections;
        auto &vAngles = s_vAngles;
        auto &SeenPacketDirections = s_SeenPacketDirections;
        vAngles.clear();
        SeenPacketDirections.clear();
        vAngles.reserve(static_cast<size_t>(MAX_CLIENTS * 128));
        SeenPacketDirections.reserve(4096);

        auto AddCandidateAngle = [&](float RelativeAngle) {
            float CandidateAngle = AimAngle + RelativeAngle;
            int TargetX = 0;
            int TargetY = 0;
            vec2 PacketDirection;
            if(!QuantizeHookInputDirection(CandidateAngle, AimInputScale, &TargetX, &TargetY, &PacketDirection))
                return;
            CandidateAngle = std::atan2(PacketDirection.y, PacketDirection.x);
            RelativeAngle = std::remainder(CandidateAngle - AimAngle, VERTEX_TWO_PI);
            if(std::fabs(RelativeAngle) > HalfFov + 0.000001f)
                return;
            const std::uint64_t Key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(TargetX)) << 32) |
                static_cast<std::uint32_t>(TargetY);
            if(!SeenPacketDirections.insert(Key).second)
                return;
            vAngles.emplace_back(std::fabs(RelativeAngle), CandidateAngle);
        };

        // Always test the cursor direction after conversion to the real packet vector.
        AddCandidateAngle(0.0f);
        for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
        {
            if(ClientId == LocalId || !pGameClient->m_aClients[ClientId].m_Active ||
                !pGameClient->m_Snap.m_aCharacters[ClientId].m_Active || !aPredictedTargetValid[ClientId][0])
                continue;
            CCharacter *pTarget = pGameClient->m_PredictedWorld.GetCharacterById(ClientId);
            if(!pTarget)
                continue;

            for(int TargetTick = 0; TargetTick <= PredictionTicks; ++TargetTick)
            {
                if(!PredictHookMovement && TargetTick > 0)
                    break;
                if(!aPredictedTargetValid[ClientId][TargetTick] || !aPredictedHookAllowed[ClientId][TargetTick])
                    continue;
                const vec2 TargetCenter = aPredictedTargetPos[ClientId][TargetTick];
                const vec2 ToCenter = TargetCenter - LocalPos;
                const float CenterDistance = length(ToCenter);
                const float SearchRadius = HOOK_PLAYER_HIT_RADIUS + 1.0f;
                if(CenterDistance <= AIM_EPSILON)
                    continue;

                const float CenterAngle = std::atan2(ToCenter.y, ToCenter.x);
                const float RelativeCenter = std::remainder(CenterAngle - AimAngle, VERTEX_TWO_PI);
                const float AngularRadius = CenterDistance <= SearchRadius ? VERTEX_PI :
                    std::asin(std::clamp(SearchRadius / CenterDistance, 0.0f, 0.99999f));
                const float Low = std::max(-HalfFov, RelativeCenter - AngularRadius);
                const float High = std::min(HalfFov, RelativeCenter + AngularRadius);
                if(High < Low)
                    continue;

                AddCandidateAngle(RelativeCenter);
                const int AdaptiveSamples = static_cast<int>(std::ceil((High - Low) / MaxAngularStep));
                const int Samples = std::clamp(std::max(BaseSamples, AdaptiveSamples), 16, 512);
                for(int Sample = 0; Sample <= Samples; ++Sample)
                {
                    const float T = static_cast<float>(Sample) / static_cast<float>(Samples);
                    AddCandidateAngle(Low + (High - Low) * T);
                }
            }
        }
        std::sort(vAngles.begin(), vAngles.end(), [](const auto &A, const auto &B) {
            if(A.first != B.first)
                return A.first < B.first;
            return A.second < B.second;
        });

		struct SRayRes { float Rel; float Angle; int Id; float Dist; CCharacter *pT; };
		static std::vector<SRayRes> s_vRes;
		s_vRes.clear();
		float LastAngle = std::numeric_limits<float>::quiet_NaN();
		int TestedAngles = 0;
		constexpr int MaxHookRayTests = 512;
		for(const auto &Candidate : vAngles)
		{
			const float Angle = Candidate.second;
			if(std::isfinite(LastAngle) && std::fabs(std::remainder(Angle - LastAngle, VERTEX_TWO_PI)) < 0.00001f)
				continue;
			LastAngle = Angle;
			if(++TestedAngles > MaxHookRayTests)
				break;
			SRayRes R;
			R.Rel = std::remainder(Angle - AimAngle, VERTEX_TWO_PI);
			R.Angle = Angle;
			R.Id = -1;
			R.Dist = 0.0f;
			R.pT = nullptr;
			vec2 Dir;
			TraceHookRay(Angle, &R.pT, &R.Id, &Dir, &R.Dist);
			s_vRes.push_back(R);
		}
		std::sort(s_vRes.begin(), s_vRes.end(), [](const SRayRes &A, const SRayRes &B) { return A.Rel < B.Rel; });

		int BestMid = -1;
		float BestScore = std::numeric_limits<float>::max();
		for(size_t i = 0; i < s_vRes.size();)
		{
			if(s_vRes[i].Id < 0)
			{
				++i;
				continue;
			}
			size_t j = i;
			while(j + 1 < s_vRes.size() && s_vRes[j + 1].Id == s_vRes[i].Id)
				++j;
			const size_t Mid = (i + j) / 2;
			const float WidthPx = (s_vRes[j].Rel - s_vRes[i].Rel) * s_vRes[Mid].Dist;
			if(WidthPx < 3.0f)
			{
				i = j + 1;
				continue;
			}
			const float Score = std::fabs(s_vRes[Mid].Rel);
			if(Score < BestScore)
			{
				BestScore = Score;
				BestMid = (int)Mid;
			}
			i = j + 1;
		}
		if(BestMid < 0)
			return false;
		*ppTarget = s_vRes[BestMid].pT;
		*pTargetId = s_vRes[BestMid].Id;
		*pTargetPoint = vec2(std::cos(s_vRes[BestMid].Angle), std::sin(s_vRes[BestMid].Angle));
		return true;
	}

	float BestScore = std::numeric_limits<float>::max();
	CCharacter *pBestTarget = nullptr;
	int BestTargetId = -1;
	vec2 BestPoint(0.0f, 0.0f);
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		if(ClientId == LocalId)
			continue;
		CCharacter *pTarget = pGameClient->m_PredictedWorld.GetCharacterById(ClientId);
		if(!pTarget)
			continue;

		const vec2 TargetCenter = pTarget->m_Pos;
		auto TestCandidate = [&](vec2 CandidatePoint) {
			const vec2 ToPoint = CandidatePoint - LocalPos;
			const float TargetDistance = length(ToPoint);
			if(TargetDistance <= AIM_EPSILON || (MaxDistance > 0.0f && TargetDistance > MaxDistance))
				return;
			const float AngleDiff = AngleDifferenceDegrees(AimDirection, ToPoint);
			if(AngleDiff > HalfFov || !HasLineOfSight(pLocalCharacter->Collision(), LocalPos, CandidatePoint, LineCheck))
				return;
			const float Score = AngleDiff + TargetDistance * 0.0001f;
			if(Score < BestScore)
			{
				BestScore = Score;
				pBestTarget = pTarget;
				BestTargetId = ClientId;
				BestPoint = CandidatePoint;
			}
		};
		TestCandidate(TargetCenter);
		static constexpr int s_aRingSamples[4] = {8, 16, 24, 36};
		const float AimSampleAngle = std::atan2(AimDirection.y, AimDirection.x);
		for(int Ring = 0; Ring < 4; ++Ring)
		{
			const float Radius = CCharacterCore::PhysicalSize() * static_cast<float>(Ring + 1) / 4.0f;
			const int Count = s_aRingSamples[Ring];
			for(int Sample = 0; Sample < Count; ++Sample)
			{
				const float Angle = AimSampleAngle + VERTEX_TWO_PI * static_cast<float>(Sample) / static_cast<float>(Count);
				TestCandidate(TargetCenter + vec2(std::cos(Angle), std::sin(Angle)) * Radius);
			}
		}
	}

	if(!pBestTarget)
		return false;
	*ppTarget = pBestTarget;
	*pTargetId = BestTargetId;
	*pTargetPoint = BestPoint;
	return true;
}

vec2 CAimbot::PredictTargetPosition(CCharacter *pTarget, float LeadTicks) const
{
	if(!pTarget)
		return vec2(0.0f, 0.0f);

	// A frozen/deep-frozen tee does not continue moving. This is important
	// for laser unfreeze: aim at its current body position rather than leading
	// past it using stale velocity.
	if(pTarget->m_FreezeTime > 0 || pTarget->Core()->m_DeepFrozen)
		return pTarget->m_Pos;

	const float Ticks = std::clamp(LeadTicks, 0.0f, 90.0f);
	return pTarget->m_Pos + pTarget->Core()->m_Vel * Ticks;
}

bool CAimbot::SolveGrenadeAim(CGameClient *pGameClient, CCharacter *pLocalCharacter,
	CCharacter *pTarget, vec2 TargetOffset, vec2 *pAimDirection, vec2 *pPredictedPoint) const
{
	if(!pGameClient || !pLocalCharacter || !pTarget || !pAimDirection || !pPredictedPoint)
		return false;

	const CTuningParams &Tuning = pGameClient->m_aTuning[g_Config.m_ClDummy];
	const float Speed = Tuning.m_GrenadeSpeed;
	const float Curvature = Tuning.m_GrenadeCurvature;
	const int LifetimeTicks = std::clamp(static_cast<int>(std::lround(Tuning.m_GrenadeLifetime * 50.0f)), 1, MAX_PROJECTILE_TICKS);
	const float BlastRadius = GrenadeNonZeroImpulseRadius(Tuning.m_ExplosionStrength);
	if(Speed <= 1.0f || BlastRadius <= 0.0f)
		return false;

	const vec2 LocalPos = pLocalCharacter->m_Pos;
	const bool Predict = g_Config.m_VxAimGrenadePredict != 0;
	const auto Evaluate = [&](float TimeSeconds, vec2 *pRequired, vec2 *pTargetAtTime) {
		const float Ticks = TimeSeconds * 50.0f;
		const vec2 TargetAtTime = (Predict ? PredictTargetPosition(pTarget, Ticks) : pTarget->m_Pos) + TargetOffset;
		const float Travel = Speed * TimeSeconds;
		const float GravityOffset = Curvature / 10000.0f * Travel * Travel;
		const vec2 Required = TargetAtTime - LocalPos - vec2(0.0f, GravityOffset);
		if(pRequired)
			*pRequired = Required;
		if(pTargetAtTime)
			*pTargetAtTime = TargetAtTime;
		return length(Required) - (PROJECTILE_SPAWN_OFFSET + Speed * TimeSeconds);
	};

	float PreviousTime = 1.0f / 50.0f;
	float PreviousError = Evaluate(PreviousTime, nullptr, nullptr);
	for(int Tick = 2; Tick <= LifetimeTicks; ++Tick)
	{
		const float CurrentTime = static_cast<float>(Tick) / 50.0f;
		const float CurrentError = Evaluate(CurrentTime, nullptr, nullptr);
		if((PreviousError <= 0.0f && CurrentError >= 0.0f) || (PreviousError >= 0.0f && CurrentError <= 0.0f))
		{
			float Low = PreviousTime;
			float High = CurrentTime;
			float LowError = PreviousError;
			for(int Iteration = 0; Iteration < 16; ++Iteration)
			{
				const float Mid = (Low + High) * 0.5f;
				const float MidError = Evaluate(Mid, nullptr, nullptr);
				if((LowError <= 0.0f && MidError >= 0.0f) || (LowError >= 0.0f && MidError <= 0.0f))
					High = Mid;
				else
				{
					Low = Mid;
					LowError = MidError;
				}
			}

			const float FlightTime = (Low + High) * 0.5f;
			vec2 Required;
			vec2 PredictedTarget;
			Evaluate(FlightTime, &Required, &PredictedTarget);
			if(length(Required) > AIM_EPSILON)
			{
				const vec2 Direction = normalize(Required);
				const vec2 Spawn = LocalPos + Direction * PROJECTILE_SPAWN_OFFSET;
				vec2 Previous = Spawn;
				vec2 Impact = Spawn;
				const int FlightTicks = std::clamp(static_cast<int>(std::lround(FlightTime * 50.0f)), 1, LifetimeTicks);
				int ImpactTick = FlightTicks;
				for(int FlightTick = 1; FlightTick <= FlightTicks; ++FlightTick)
				{
					const vec2 Current = CalcPos(Spawn, Direction, Curvature, Speed, static_cast<float>(FlightTick) / 50.0f);
					vec2 CollisionPos;
					vec2 BeforeCollisionPos;
					if(pLocalCharacter->Collision()->IntersectLine(Previous, Current, &CollisionPos, &BeforeCollisionPos) != 0)
					{
						Impact = CollisionPos;
						ImpactTick = FlightTick;
						break;
					}
					Previous = Current;
					Impact = Current;
				}
				const float DetonationTicks = static_cast<float>(ImpactTick);
				const vec2 BlastTarget = (Predict ? PredictTargetPosition(pTarget, DetonationTicks) : pTarget->m_Pos) + TargetOffset;
				if(distance(Impact, BlastTarget) <= BlastRadius)
				{
					*pAimDirection = Direction;
					*pPredictedPoint = BlastTarget;
					return true;
				}
			}
		}
		PreviousTime = CurrentTime;
		PreviousError = CurrentError;
	}
	return false;
}

bool CAimbot::SolveShotgunAim(CGameClient *pGameClient, CCharacter *pLocalCharacter,
	CCharacter *pTarget, vec2 TargetOffset, vec2 *pAimDirection, vec2 *pPredictedPoint) const
{
	if(!pGameClient || !pLocalCharacter || !pTarget || !pAimDirection || !pPredictedPoint)
		return false;

	const CTuningParams &Tuning = pGameClient->m_aTuning[g_Config.m_ClDummy];
	const float Speed = Tuning.m_ShotgunSpeed;
	const float Curvature = Tuning.m_ShotgunCurvature;
	const int LifetimeTicks = std::clamp(static_cast<int>(std::lround(Tuning.m_ShotgunLifetime * 50.0f)), 1, MAX_PROJECTILE_TICKS);
	if(Speed <= 1.0f)
		return false;

	const vec2 LocalPos = pLocalCharacter->m_Pos;
	const bool Predict = g_Config.m_VxAimbotShotgunPredict != 0;
	const auto Evaluate = [&](float TimeSeconds, vec2 *pRequired, vec2 *pTargetAtTime) {
		const float Ticks = TimeSeconds * 50.0f;
		const vec2 TargetAtTime = (Predict ? PredictTargetPosition(pTarget, Ticks) : pTarget->m_Pos) + TargetOffset;
		const float Travel = Speed * TimeSeconds;
		const float GravityOffset = Curvature / 10000.0f * Travel * Travel;
		const vec2 Required = TargetAtTime - LocalPos - vec2(0.0f, GravityOffset);
		if(pRequired)
			*pRequired = Required;
		if(pTargetAtTime)
			*pTargetAtTime = TargetAtTime;
		return length(Required) - (PROJECTILE_SPAWN_OFFSET + Speed * TimeSeconds);
	};

	float PreviousTime = 1.0f / 50.0f;
	float PreviousError = Evaluate(PreviousTime, nullptr, nullptr);
	for(int Tick = 2; Tick <= LifetimeTicks; ++Tick)
	{
		const float CurrentTime = static_cast<float>(Tick) / 50.0f;
		const float CurrentError = Evaluate(CurrentTime, nullptr, nullptr);
		if((PreviousError <= 0.0f && CurrentError >= 0.0f) || (PreviousError >= 0.0f && CurrentError <= 0.0f))
		{
			float Low = PreviousTime;
			float High = CurrentTime;
			float LowError = PreviousError;
			for(int Iteration = 0; Iteration < 16; ++Iteration)
			{
				const float Mid = (Low + High) * 0.5f;
				const float MidError = Evaluate(Mid, nullptr, nullptr);
				if((LowError <= 0.0f && MidError >= 0.0f) || (LowError >= 0.0f && MidError <= 0.0f))
					High = Mid;
				else
				{
					Low = Mid;
					LowError = MidError;
				}
			}
			const float FlightTime = (Low + High) * 0.5f;
			vec2 Required;
			vec2 PredictedTarget;
			Evaluate(FlightTime, &Required, &PredictedTarget);
			if(length(Required) <= AIM_EPSILON)
				continue;
			const vec2 Direction = normalize(Required);
			const vec2 Spawn = LocalPos + Direction * PROJECTILE_SPAWN_OFFSET;
			vec2 Previous = Spawn;
			const int FlightTicks = std::clamp(static_cast<int>(std::lround(FlightTime * 50.0f)), 1, LifetimeTicks);
			for(int FlightTick = 1; FlightTick <= FlightTicks; ++FlightTick)
			{
				const float Time = static_cast<float>(FlightTick) / 50.0f;
				const vec2 Current = CalcPos(Spawn, Direction, Curvature, Speed, Time);
				vec2 CollisionPos;
				vec2 BeforeCollisionPos;
				const int Hit = pLocalCharacter->Collision()->IntersectLineTeleWeapon(Previous, Current, &CollisionPos, &BeforeCollisionPos);
				const vec2 SegmentEnd = Hit ? BeforeCollisionPos : Current;
				vec2 ClosestPoint;
				if(closest_point_on_line(Previous, SegmentEnd, PredictedTarget, ClosestPoint) &&
					distance(ClosestPoint, PredictedTarget) <= CCharacterCore::PhysicalSize() * 0.5f)
				{
					*pAimDirection = Direction;
					*pPredictedPoint = PredictedTarget;
					return true;
				}
				if(Hit != 0)
					break;
				Previous = Current;
			}
		}
		PreviousTime = CurrentTime;
		PreviousError = CurrentError;
	}
	return false;
}

void CAimbot::ApplyAimbot(CNetObj_PlayerInput *pInput, CControls *pControls, CGameClient *pGameClient)
{
	m_LockedTargetId = -1;
	m_SkinHighlightTargetId = -1;
	m_WeaponAimActive = false;
	if(!m_Enabled || !pInput || !pControls || !pGameClient || !pGameClient->m_Snap.m_pLocalCharacter)
		return;
	if(pGameClient->m_Chat.IsActive() || pGameClient->m_Menus.IsActive() || pControls->m_LaserUnfreezeAimActive)
		return;

	CCharacter *pLocalCharacter = pGameClient->m_PredictedWorld.GetCharacterById(pGameClient->m_Snap.m_LocalClientId);
	if(!pLocalCharacter || !pLocalCharacter->Collision())
		return;

	const int Dummy = g_Config.m_ClDummy;
	const vec2 LocalPos = pLocalCharacter->Core()->m_Pos;
	const vec2 AimDirection = pControls->m_aMousePos[Dummy];
	if(length(AimDirection) <= AIM_EPSILON)
		return;
	m_CursorAimDirection = AimDirection;

	// Resolve the hook target once per input snapshot, not from OnRender().
	// OnRender can run hundreds of times per second; keep hook search in SnapInput
	// and let ESP/highlight consume the confirmed results below.
	CCharacter *pHookTarget = nullptr;
	int HookTargetId = -1;
	vec2 HookTargetDirection(0.0f, 0.0f);
	const bool WantsHookTarget = (g_Config.m_VxAimbotHook && pInput->m_Hook != 0) ||
		g_Config.m_VxAimbotSkinHighlight;
	const bool HasHookTarget = WantsHookTarget && FindTargetInFov(pGameClient, pLocalCharacter,
		AimDirection, static_cast<float>(g_Config.m_VxAimbotHookFov),
		pLocalCharacter->Core()->m_Tuning.m_HookLength, ELineCheck::HOOK,
		&pHookTarget, &HookTargetId, &HookTargetDirection, 85,
		pControls->GetMaxMouseDistance(), pInput);
	if(HasHookTarget && g_Config.m_VxAimbotSkinHighlight)
		m_SkinHighlightTargetId = HookTargetId;

	auto WriteSilentDirection = [&](vec2 Direction, bool WeaponAim) {
		if(length(Direction) <= AIM_EPSILON)
			return false;
		Direction = normalize(Direction);
		const float MouseMax = std::max(1.0f, pControls->GetMaxMouseDistance());
		pInput->m_TargetX = static_cast<int>(std::lround(Direction.x * MouseMax));
		pInput->m_TargetY = static_cast<int>(std::lround(Direction.y * MouseMax));
		m_LockedAimDirection = Direction;
		m_WeaponAimActive = WeaponAim;
		m_WeaponAimDirection = Direction;
		return true;
	};

	const CTuningParams &Tuning = pGameClient->m_aTuning[g_Config.m_ClDummy];

	// CNetObj_PlayerInput has a single TargetX/TargetY vector shared by the
	// hook and the active weapon. Do not silently redirect both in different
	// ways on the same tick: while firing and hooking together, preserve the
	// user's manual direction so the grenade solver cannot steer the hook.
	if(pInput->m_Hook != 0 && (pInput->m_Fire & 1) != 0)
		return;

	// HOOK: only redirects the hook while the user is holding the hook input.
	// Read the same persisted hook toggle/FOV that the visible Vertex menu edits.
	if((pInput->m_Fire & 1) == 0 && pInput->m_Hook != 0 && g_Config.m_VxAimbotHook &&
		pLocalCharacter->Core()->m_HookState == HOOK_IDLE && HasHookTarget)
	{
		// Final guard uses the exact cursor direction used to build the candidates.
		const float AllowedAngle = HalfFovRad(static_cast<float>(g_Config.m_VxAimbotHookFov)) * 180.0f / VERTEX_PI;
		const float ActualAngle = AngleDifferenceDegrees(AimDirection, HookTargetDirection);
		if(ActualAngle <= AllowedAngle + 0.0001f)
		{
			m_LockedTargetId = HookTargetId;
			// The ray was quantized to the actual outgoing input scale before tracing.
			// Always write it when the packet currently points somewhere else.
			int PacketX = 0;
			int PacketY = 0;
			vec2 PacketDirection;
			const float PacketAngle = std::atan2(HookTargetDirection.y, HookTargetDirection.x);
			if(QuantizeHookInputDirection(PacketAngle, pControls->GetMaxMouseDistance(),
				&PacketX, &PacketY, &PacketDirection))
			{
				pInput->m_TargetX = PacketX;
				pInput->m_TargetY = PacketY;
				m_LockedAimDirection = PacketDirection;
			}
			return;
		}
	}

	if((pInput->m_Fire & 1) == 0)
		return;

	const int ActiveWeapon = pLocalCharacter->Core()->m_ActiveWeapon;
	CCharacter *pTarget = nullptr;
	int TargetId = -1;
	vec2 AimPoint(0.0f, 0.0f);
	vec2 TargetPoint(0.0f, 0.0f);

	switch(ActiveWeapon)
	{
	case WEAPON_HAMMER:
		if(!g_Config.m_VxAimbotHammer)
			return;
		if(!FindTargetInFov(pGameClient, pLocalCharacter, AimDirection,
			static_cast<float>(g_Config.m_VxAimbotHammerFov), HAMMER_REACH,
			ELineCheck::NORMAL, &pTarget, &TargetId, &TargetPoint))
			return;
		AimPoint = PredictTargetPosition(pTarget, g_Config.m_VxAimHammerPredict ? 1.0f : 0.0f) + (TargetPoint - pTarget->m_Pos);
		if(distance(LocalPos, AimPoint) > HAMMER_REACH ||
			!HasLineOfSight(pLocalCharacter->Collision(), LocalPos, AimPoint, ELineCheck::NORMAL))
			return;
		if(WriteSilentDirection(AimPoint - LocalPos, true))
			m_LockedTargetId = TargetId;
		return;

	case WEAPON_GRENADE:
		if(!g_Config.m_VxAimbotGrenade)
			return;
		if(!FindTargetInFov(pGameClient, pLocalCharacter, AimDirection,
			static_cast<float>(g_Config.m_VxAimbotGrenadeFov), 0.0f,
			ELineCheck::NORMAL, &pTarget, &TargetId, &TargetPoint))
			return;
		{
			vec2 GrenadeDirection;
			vec2 PredictedPoint;
			if(SolveGrenadeAim(pGameClient, pLocalCharacter, pTarget, TargetPoint - pTarget->m_Pos, &GrenadeDirection, &PredictedPoint) &&
				WriteSilentDirection(GrenadeDirection, true))
				m_LockedTargetId = TargetId;
		}
		return;

	case WEAPON_LASER:
		if(!g_Config.m_VxAimbotLaser)
			return;
		if(!FindTargetInFov(pGameClient, pLocalCharacter, AimDirection,
			static_cast<float>(g_Config.m_VxAimbotLaserFov), Tuning.m_LaserReach,
			ELineCheck::WEAPON, &pTarget, &TargetId, &TargetPoint))
			return;
		AimPoint = PredictTargetPosition(pTarget, g_Config.m_VxAimbotLaserPredict ? 2.0f : 0.0f) + (TargetPoint - pTarget->m_Pos);
		if(HasLineOfSight(pLocalCharacter->Collision(), LocalPos, AimPoint, ELineCheck::WEAPON) &&
			WriteSilentDirection(AimPoint - LocalPos, true))
			m_LockedTargetId = TargetId;
		return;

	case WEAPON_SHOTGUN:
		if(!g_Config.m_VxAimbotShotgun)
			return;
		if(!FindTargetInFov(pGameClient, pLocalCharacter, AimDirection,
			static_cast<float>(g_Config.m_VxAimbotShotgunFov), 0.0f,
			ELineCheck::WEAPON, &pTarget, &TargetId, &TargetPoint))
			return;
		if(pGameClient->m_PredictedWorld.m_WorldConfig.m_IsVanilla && g_Config.m_VxAimbotShotgunPredict)
		{
			vec2 ShotgunDirection;
			vec2 PredictedPoint;
			if(SolveShotgunAim(pGameClient, pLocalCharacter, pTarget,
				TargetPoint - pTarget->m_Pos, &ShotgunDirection, &PredictedPoint) &&
				WriteSilentDirection(ShotgunDirection, true))
				m_LockedTargetId = TargetId;
		}
		else
		{
			AimPoint = PredictTargetPosition(pTarget, g_Config.m_VxAimbotShotgunPredict ? 2.0f : 0.0f) +
				(TargetPoint - pTarget->m_Pos);
			if(HasLineOfSight(pLocalCharacter->Collision(), LocalPos, AimPoint, ELineCheck::WEAPON) &&
				WriteSilentDirection(AimPoint - LocalPos, true))
				m_LockedTargetId = TargetId;
		}
		return;

	default:
		return;
	}
}

void CAimbot::OnRender()
{
	CGameClient *pGameClient = GameClient();
	IGraphics *pGraphics = Graphics();
	if(!m_Enabled || !pGameClient || !pGraphics || !pGameClient->m_Snap.m_pLocalCharacter)
	{
		m_LockedTargetId = -1;
		m_TrajectoryRenderOriginInitialized = false;
		return;
	}
	if(pGameClient->m_Chat.IsActive() || pGameClient->m_Menus.IsActive())
	{
		m_LockedTargetId = -1;
		m_TrajectoryRenderOriginInitialized = false;
		return;
	}

	CCharacter *pLocalCharacter = pGameClient->m_PredictedWorld.GetCharacterById(pGameClient->m_Snap.m_LocalClientId);
	if(!pLocalCharacter || !pLocalCharacter->Collision())
	{
		m_LockedTargetId = -1;
		m_TrajectoryRenderOriginInitialized = false;
		return;
	}

	// m_LocalCharacterPos is the render-interpolated local tee position
	// (including predicted render interpolation when prediction is enabled).
	// Use this same origin for every preview so walking does not produce tick steps.
	const vec2 LocalPos = pGameClient->m_LocalCharacterPos;
	// Smooth only the visual trajectory origin. Silent Aim and FOV/ESP remain
	// attached to the exact current positions and mouse direction.
	const float FrameTime = std::clamp(Client() ? Client()->RenderFrameTime() : (1.0f / 60.0f), 0.001f, 0.05f);
	const float OriginSmoothing = 1.0f - std::exp(-FrameTime * 38.0f);
	if(!m_TrajectoryRenderOriginInitialized || distance(m_TrajectoryRenderOrigin, LocalPos) > 64.0f)
	{
		m_TrajectoryRenderOrigin = LocalPos;
		m_TrajectoryRenderOriginInitialized = true;
	}
	else
		m_TrajectoryRenderOrigin += (LocalPos - m_TrajectoryRenderOrigin) * OriginSmoothing;
	const vec2 TrajectoryOrigin = m_TrajectoryRenderOrigin;
	// m_aMousePos is the relative cursor direction used to build outgoing input.
	// Keep the drawn FOV centered on that exact direction.
	vec2 MouseDirection = pGameClient->m_Controls.m_aMousePos[g_Config.m_ClDummy];
	if(length(MouseDirection) <= AIM_EPSILON)
		MouseDirection = vec2(1.0f, 0.0f);
	const int ActiveWeapon = pLocalCharacter->Core()->m_ActiveWeapon;
	const CTuningParams &Tuning = pGameClient->m_aTuning[g_Config.m_ClDummy];
	const CTuningParams &HookTuning = pLocalCharacter->Core()->m_Tuning;

	// This component is registered after menus/HUD, so the active projection
	// may be the interface projection rather than the game-world projection.
	// Explicitly restore the game's camera mapping before drawing any world-space
	// FOV/ESP/trajectory geometry, and restore the previous mapping afterwards.
	const CScreenRect PreviousScreen = pGraphics->GetScreen();
	float ParallaxX = 100.0f;
	float ParallaxY = 100.0f;
	float OffsetX = 0.0f;
	float OffsetY = 0.0f;
	if(pGameClient->Layers() && pGameClient->Layers()->GameGroup())
	{
		const CMapItemGroup *pGameGroup = pGameClient->Layers()->GameGroup();
		ParallaxX = static_cast<float>(pGameGroup->m_ParallaxX);
		ParallaxY = static_cast<float>(pGameGroup->m_ParallaxY);
		OffsetX = static_cast<float>(pGameGroup->m_OffsetX);
		OffsetY = static_cast<float>(pGameGroup->m_OffsetY);
	}
	const CScreenRect WorldScreen = pGraphics->MapScreenToWorld(
		pGameClient->m_Camera.m_Center.x, pGameClient->m_Camera.m_Center.y,
		ParallaxX, ParallaxY, 100.0f, OffsetX, OffsetY,
		pGraphics->ScreenAspect(), std::max(0.001f, pGameClient->m_Camera.m_Zoom));
	pGraphics->MapScreen(WorldScreen);

	pGraphics->TextureClear();
	pGraphics->LinesBegin();

	// Two clipped boundary rays visualize the same FOV used by hook targeting.
	if(g_Config.m_VxAimbotDrawFov)
	{
		ColorRGBA FovColor = ConfigColor(g_Config.m_VxAimbotFovColor);
		if(g_Config.m_VxAimbotFovHighlight)
			FovColor = ConfigColor(g_Config.m_VxAimbotFovHighlightColor);
		DrawFovCone(pGraphics, pLocalCharacter->Collision(), LocalPos, MouseDirection,
			static_cast<float>(g_Config.m_VxAimbotHookFov), HookTuning.m_HookLength, HookTuning.m_HookFireSpeed, FovColor);
	}

	// ESP Players uses exactly the hook's configured FOV and wall visibility.
	if(g_Config.m_VxAimbotEspPlayers && g_Config.m_VxAimbotHook)
	{
		const ColorRGBA EspColor = ConfigColor(g_Config.m_VxAimbotEspColor);
		SetGraphicsColor(pGraphics, EspColor, 1.0f);
		const int LocalId = pGameClient->m_Snap.m_LocalClientId;
		const float HookRange = HookTuning.m_HookLength;
		const float HalfFov = HalfFovRad(static_cast<float>(g_Config.m_VxAimbotHookFov)) * 180.0f / VERTEX_PI;
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
		{
			if(ClientId == LocalId)
				continue;
			CCharacter *pTarget = pGameClient->m_PredictedWorld.GetCharacterById(ClientId);
			if(!pTarget)
				continue;
            const vec2 TargetRenderPos = pGameClient->m_aClients[ClientId].m_RenderPos;
            if(distance(TargetRenderPos, LocalPos) - HOOK_PLAYER_HIT_RADIUS > HookRange)
            	continue;
			static const vec2 s_aEspOffsets[] = {
				{0.0f, 0.0f}, {0.0f, -14.0f}, {-14.0f, 0.0f}, {14.0f, 0.0f}, {0.0f, 14.0f},
				{-10.0f, -10.0f}, {10.0f, -10.0f}, {-10.0f, 10.0f}, {10.0f, 10.0f},
			};
			bool VisibleInHookFov = false;
			for(const vec2 Offset : s_aEspOffsets)
			{
				const vec2 Candidate = TargetRenderPos + Offset;
				const vec2 ToCandidate = Candidate - LocalPos;
				if(length(ToCandidate) <= AIM_EPSILON || AngleDifferenceDegrees(MouseDirection, ToCandidate) > HalfFov)
					continue;
				if(IsHookLineVisible(pLocalCharacter->Collision(), LocalPos, Candidate, HookTuning.m_HookFireSpeed))
				{
					VisibleInHookFov = true;
					break;
				}
			}
			if(!VisibleInHookFov)
				continue;
			if(g_Config.m_VxAimbotShowBox)
				DrawPlayerBox(pGraphics, TargetRenderPos, EspColor);
		}
	}

	pGraphics->LinesEnd();

	// Weapon trajectories follow the predicted lock direction when the aim has
	// acquired a target; otherwise they preview the user's mouse direction.
	// They remain independent of whether an aim target is currently locked.
	const vec2 Direction = normalize(m_WeaponAimActive ? m_WeaponAimDirection : MouseDirection);
	if(ActiveWeapon == WEAPON_GRENADE && g_Config.m_VxAimbotGrenadeTrajectory)
	{
		const ColorRGBA TrajectoryColor = ConfigColor(g_Config.m_VxAimbotGrenadeTrajectoryColor);
		pGraphics->TextureClear();
		pGraphics->LinesBegin();
		const vec2 Spawn = TrajectoryOrigin + Direction * PROJECTILE_SPAWN_OFFSET;
		DrawProjectilePath(pGraphics, pLocalCharacter->Collision(), Spawn, Direction,
			Tuning.m_GrenadeCurvature, Tuning.m_GrenadeSpeed, Tuning.m_GrenadeLifetime,
			TrajectoryColor, true, g_Config.m_VxAimbotGrenadeBlastCircle != 0, Tuning.m_ExplosionStrength);
		pGraphics->LinesEnd();
	}
	else if(ActiveWeapon == WEAPON_LASER && g_Config.m_VxAimbotLaserTrajectory)
	{
		const ColorRGBA TrajectoryColor = ConfigColor(g_Config.m_VxAimbotLaserTrajectoryColor);
		pGraphics->TextureClear();
		pGraphics->LinesBegin();
		DrawBeamPath(pGraphics, pLocalCharacter->Collision(), TrajectoryOrigin, Direction,
			Tuning.m_LaserReach, Tuning.m_LaserBounceCost,
			static_cast<int>(Tuning.m_LaserBounceNum), TrajectoryColor);
		pGraphics->LinesEnd();
	}
	else if(ActiveWeapon == WEAPON_SHOTGUN && g_Config.m_VxAimbotShotgunTrajectory)
	{
		const ColorRGBA TrajectoryColor = ConfigColor(g_Config.m_VxAimbotShotgunTrajectoryColor);
		pGraphics->TextureClear();
		pGraphics->LinesBegin();
		if(pGameClient->m_PredictedWorld.m_WorldConfig.m_IsVanilla)
		{
			const vec2 Spawn = TrajectoryOrigin + Direction * PROJECTILE_SPAWN_OFFSET;
			DrawProjectilePath(pGraphics, pLocalCharacter->Collision(), Spawn, Direction,
				Tuning.m_ShotgunCurvature, Tuning.m_ShotgunSpeed, Tuning.m_ShotgunLifetime,
				TrajectoryColor, false);
		}
		else
		{
			DrawBeamPath(pGraphics, pLocalCharacter->Collision(), TrajectoryOrigin, Direction,
				Tuning.m_LaserReach, Tuning.m_LaserBounceCost,
				std::min(MAX_BEAM_BOUNCES, static_cast<int>(Tuning.m_LaserBounceNum)), TrajectoryColor);
		}
		pGraphics->LinesEnd();
	}

	pGraphics->MapScreen(PreviousScreen);
}
