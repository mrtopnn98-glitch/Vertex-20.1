#ifndef GAME_CLIENT_COMPONENTS_AIMBOT_H
#define GAME_CLIENT_COMPONENTS_AIMBOT_H

#include <type_traits>

#include <base/vmath.h>
#include <engine/shared/protocol.h>
#include <game/client/component.h>
#include <generated/protocol.h>

class CCharacter;
class CCollision;
class CControls;
class CGameClient;

class CAimbot : public CComponent
{
public:
	// The existing Vertex bind may toggle the whole module. Individual features
	// are controlled by the VxAimbot* config variables documented separately.
	bool m_Enabled = true;

	CAimbot() = default;

	void OnRender() override;
	int Sizeof() const override { return sizeof(*this); }

	// Called from CControls::SnapInput after the regular mouse target has been
	// copied into m_aInputData and before the final input is copied to pData.
	void ApplyAimbot(CNetObj_PlayerInput *pInput, CControls *pControls, CGameClient *pGameClient);
	bool IsSkinHighlightTarget(int ClientId) const { return ClientId >= 0 && ClientId == m_SkinHighlightTargetId; }

private:
	enum class ELineCheck
	{
		NORMAL,
		HOOK,
		WEAPON,
	};

	bool HasLineOfSight(CCollision *pCollision, vec2 From, vec2 To, ELineCheck Check) const;
	bool FindTargetInFov(CGameClient *pGameClient, CCharacter *pLocalCharacter,
		vec2 AimDirection, float FovDegrees, float MaxDistance, ELineCheck LineCheck,
		CCharacter **ppTarget, int *pTargetId, vec2 *pTargetPoint, int HookSearchSamples = 85, float AimInputScale = 0.0f, const CNetObj_PlayerInput *pLocalInput = nullptr);
	vec2 PredictTargetPosition(CCharacter *pTarget, float LeadTicks) const;
	bool SolveGrenadeAim(CGameClient *pGameClient, CCharacter *pLocalCharacter,
		CCharacter *pTarget, vec2 TargetOffset, vec2 *pAimDirection, vec2 *pPredictedPoint) const;
	bool SolveShotgunAim(CGameClient *pGameClient, CCharacter *pLocalCharacter,
		CCharacter *pTarget, vec2 TargetOffset, vec2 *pAimDirection, vec2 *pPredictedPoint) const;

	// Target selected by the most recent SnapInput pass; used for the skin overlay.
	int m_LockedTargetId = -1;
	vec2 m_LockedAimDirection = vec2(1.0f, 0.0f);
	int m_SkinHighlightTargetId = -1;
	vec2 m_CursorAimDirection = vec2(1.0f, 0.0f);
	bool m_WeaponAimActive = false;
	vec2 m_WeaponAimDirection = vec2(1.0f, 0.0f);

	// Render-only trajectory filter. Silent-aim input is never smoothed.
	vec2 m_TrajectoryRenderOrigin = vec2(0.0f, 0.0f);
	bool m_TrajectoryRenderOriginInitialized = false;
};

#endif // GAME_CLIENT_COMPONENTS_AIMBOT_H
