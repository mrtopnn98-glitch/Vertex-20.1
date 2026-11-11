/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_CONTROLS_H
#define GAME_CLIENT_COMPONENTS_CONTROLS_H

#include <vector>
#include <base/vmath.h>

#include <engine/client.h>
#include <engine/console.h>

#include <generated/protocol.h>

#include <game/client/component.h>

class CControls : public CComponent
{
public:
	float GetMinMouseDistance() const;
	float GetMaxMouseDistance() const;

	enum class EMouseInputType
	{
		ABSOLUTE,
		RELATIVE,
		AUTOMATED,
	};

	vec2 m_aMousePos[NUM_DUMMIES];
	vec2 m_aMousePosOnAction[NUM_DUMMIES];
	vec2 m_aTargetPos[NUM_DUMMIES];

	EMouseInputType m_aMouseInputType[NUM_DUMMIES];

	// Stage 7 debug: temporary raycast visualization
	struct SDebugRay
	{
		vec2 m_From;
		vec2 m_To;
		bool m_Hit;
		SDebugRay(vec2 From, vec2 To, bool Hit) : m_From(From), m_To(To), m_Hit(Hit) {}
	};
	std::vector<SDebugRay> m_vDebugRays;
	int m_VertexAimTargetId = -1;

	// === unfreeze bot: silent aim override + path debug ===
	bool m_LaserUnfreezeAimActive = false;
	vec2 m_LaserUnfreezeAimOffset = vec2(0.0f, 0.0f);
	std::vector<vec2> m_LaserUnfreezePath;
	int64_t m_LaserUnfreezePathTime = 0;
	// === end unfreeze bot ===

	int m_aAmmoCount[NUM_WEAPONS];

	int64_t m_LastSendTime;
	bool m_ManualJumpConsumed = false;
	int m_PhysicalJump = 0;
	int m_aPhysicalHook[NUM_DUMMIES] = {};
	// === vertex glue (step2): physical key tracking ===
	CNetObj_PlayerInput m_aInputData[NUM_DUMMIES];
	CNetObj_PlayerInput m_aLastData[NUM_DUMMIES];
	int m_aInputDirectionLeft[NUM_DUMMIES];
	int m_aInputDirectionRight[NUM_DUMMIES];
	int m_aShowHookColl[NUM_DUMMIES];

	CControls();
	int Sizeof() const override { return sizeof(*this); }

	void OnReset() override;
	void OnRender() override;
	void OnMessage(int MsgType, void *pRawMsg) override;
	bool OnCursorMove(float x, float y, IInput::ECursorType CursorType) override;
	void OnConsoleInit() override;
	virtual void OnPlayerDeath();

	int SnapInput(int *pData);
	void VertexUpdateUnfreeze();
	void ClampMousePos();
	void ResetInput(int Dummy);
    void RenderDummyLaserEsp();
private:
	static void ConKeyInputState(IConsole::IResult *pResult, void *pUserData);
	static void ConKeyInputCounter(IConsole::IResult *pResult, void *pUserData);
	static void ConKeyInputSet(IConsole::IResult *pResult, void *pUserData);
	static void ConKeyInputNextPrevWeapon(IConsole::IResult *pResult, void *pUserData);
};
#endif
