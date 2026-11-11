#ifndef GAME_CLIENT_ALED_H
#define GAME_CLIENT_ALED_H

class CGameClient;
class IClient;
struct CNetObj_PlayerInput;

// Vertex Auto Aled: finds ANY frozen player that a hammer hit can free (own Dummy, teammates, or everybody
// when "teammates only" is off), aims at it, selects the hammer and taps the fire counter at the right moment.
// Movement, jump and hook are never touched: the user positions the tee.
class CAutoAled
{
public:
  void Update(CGameClient *pGame, IClient *pClient, CNetObj_PlayerInput *pSlot);
  void Cancel(CNetObj_PlayerInput *pSlot);
  void Reset();
  const char *Status() const { return m_pStatus; }

private:
  void EndPlan(CNetObj_PlayerInput *pSlot, bool RestoreWeapon, int NowTick, bool Cooldown);
  void SetStatus(const char *pStatus, int NowTick, bool Debug);

  // plan state
  bool m_PlanActive = false;
  int m_PlanTargetId = -1;
  int m_LastWorkTick = -1000000;
  int m_NextPlanTick = -1000000;
  int m_LogGateTick = -1000000;
  int m_UserWantedWeapon = 0;
  int m_LastWrittenFire = -1;
  int m_HammerPresses = 0;

  // fire / verification state
  int m_LastPressTick = -1000000; // tick of the last REAL press written into the input
  bool m_ReleasePending = false; // the fire counter is odd (pressed) and has to be released on the next tick
  int m_LastReadyTick = -1000000; // last tick on which the target was inside a safe hammer hit
  int m_LastDummy = -1;

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