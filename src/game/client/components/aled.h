#ifndef GAME_CLIENT_ALED_H
#define GAME_CLIENT_ALED_H

class CGameClient;
class IClient;
struct CNetObj_PlayerInput;

// Vertex Auto Aled: the user positions the tee (moves, jumps, hooks) and Auto Aled does the rest:
// it notices that an aled is being set up (me / FREEZE tiles / frozen player), pre-selects the hammer and aims,
// and hits on the very first tick on which a simulation proves that the hit frees the player for good.
// Only one setting exists: on / off (g_Config.m_VxAledEnable). Everything else is a constant in aled.cpp.
class CAutoAled
{
public:
	void Update(CGameClient *pGame, IClient *pClient, CNetObj_PlayerInput *pSlot);
	void Cancel(CNetObj_PlayerInput *pSlot);
	void Reset();
bool IsBusy() const { return m_PlanActive || m_ReleasePending; }
const char *Status() const { return m_pStatus; }
private:
	void EndPlan(CNetObj_PlayerInput *pSlot, bool RestoreWeapon, int NowTick, bool Cooldown);

	// plan state
	bool m_PlanActive = false;
	int m_PlanTargetId = -1;
	int m_LastWorkTick = -1000000;
	int m_NextPlanTick = -1000000;
	int m_UserWantedWeapon = 0;
	int m_LastWrittenFire = -1;
	int m_HammerPresses = 0;

	// fire / verification state
	int m_LastPressTick = -1000000; // tick of the last REAL press written into the input
	bool m_ReleasePending = false; // the fire counter is odd (pressed) and has to be released on the next tick
	int m_LastReadyTick = -1000000; // last tick on which an aled was possible or being set up
	int m_LastDummy = -1;

	// the heavy search runs once per game tick; repeated calls inside the same tick only re-apply the aim
	int m_CacheTick = -1000000;
	int m_CacheAimX = 0;
	int m_CacheAimY = 0;

	// the wanted weapon written to restore the user's weapon is cleared again a few ticks later
	int m_ClearWantedTick = -1;
	int m_RestoredWanted = 0;

	const char *m_pStatus = "Auto Aled: disabled";
	const char *m_pCooldownStatus = "Searching for frozen players";
};

// One client-wide instance. Keeps state between SnapInput calls without
// adding another component to CGameClient's render/input lists.
CAutoAled &VertexAutoAled();

#endif // GAME_CLIENT_ALED_H