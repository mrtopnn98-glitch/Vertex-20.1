/* Vertex client — Avoid Freeze (blatant).
 *
 * Automatic rescue of the tee I control. New module, lives in its own files:
 *   avoidfreeze.h / avoidfreeze.cpp
 * The settings tab calls StartBind()/m_BindMode, CControls calls Update()
 * (once per tick, from SnapInput) and RenderBinds() (once per frame, from
 * OnRender), the world overlay is drawn by RenderWorld() (players.cpp).
 * Debug log tag: "avoid-freeze" (vx_af_debug 1).
 */
#ifndef GAME_CLIENT_AVOIDFREEZE_H
#define GAME_CLIENT_AVOIDFREEZE_H
#include <base/vmath.h>
#include <cstdint>
class CGameClient;
class IClient;
class CNetObj_PlayerInput;
class CAvoidFreeze
{
public:
	enum
	{
		BIND_NONE = 0,
		BIND_BASIC = 1,
		BIND_DRAG = 2,
		BIND_TRACKPOINT = 3,
	};
	// read by the settings tab (bind pill "waiting for a key" state)
	int m_BindMode = BIND_NONE;
	// called by the settings tab when a bind pill is clicked
	void StartBind(int Mode);
	// call once per render frame from CControls::OnRender: bind capture + key binds
	void RenderBinds(CGameClient *pGame);
	// call once per game tick from CControls::SnapInput (after the dummy unfreeze block):
	// the whole rescue logic, writes the plan inputs into pSlot when active
	void Update(CGameClient *pGame, IClient *pClient, CNetObj_PlayerInput *pSlot);
	// call from the world render (players.cpp, where the dummy unfreeze ESP is drawn):
	// plan path / aim line / track point overlay
	void RenderWorld(CGameClient *pGame);
	// full state reset (map change / reconnect)
	void Reset();
private:
	bool AnyActive() const;
	void RestoreSlot(CNetObj_PlayerInput *pSlot);
	// ---- plan (result of the last search, used for stickiness) ----
	int m_PlanType = 0; // AF_NONE / AF_DIR / AF_HOOK / AF_NAV / AF_DRAG / AF_ASSIST
	int m_PlanDir = 0;
	bool m_PlanJump = false;
	bool m_PlanHook = false;
	vec2 m_PlanAim = vec2(0.0f, 0.0f);
	float m_PlanAngle = 0.0f;
	float m_PlanScore = 0.0f;
	int m_PlanTick = -1000000;
	// ---- per tick state ----
	int m_LastWorkTick = -1000000;
	int m_LogGateTick = -1000000;
	int m_NextExitTick = -1000000;
	float m_WarmAngle = 0.0f;
	int m_WarmTick = -1000000;
	// ---- user input as it was before the module touched it (for restore) ----
	// jump/hook are sticky fields: while the plan holds them, the user's key events
	// still write through (the input system overwrites our value on press/release),
	// which is detected by comparing the slot with what the module wrote last tick.
	int m_UserDir = 0;
	int m_UserJump = 0;
	int m_UserHook = 0;
	int m_UserTargetX = 0;
	int m_UserTargetY = 0;
	int m_WroteJump = 0;
	int m_WroteHook = 0;
	// ---- afk detection ----
	int m_LastDir = 0;
	int m_LastJump = 0;
	int m_LastHook = 0;
	int m_LastInputChangeTick = -1000000;
	// ---- track point (set with the Track Point bind) ----
	bool m_TrackPointSet = false;
	vec2 m_TrackPoint = vec2(0.0f, 0.0f);
	// ---- auto drag (hold the Auto Drag bind) ----
	bool m_DragHeld = false;
	// ---- what was active on the previous tick (for restore + logs) ----
	bool m_RescueActive = false; // pre-freeze rescue
	bool m_ExitActive = false;   // getting out of an already taken freeze
	bool m_NavActive = false;    // walking/hooking back to the track point
	bool m_DragActive = false;   // auto drag towards the cursor
	bool m_AssistActive = false; // aim assist of a manual hook
	// ---- bind capture ----
	int64_t m_BindBlockUntil = 0;
};
#endif
