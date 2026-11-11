#ifndef GAME_CLIENT_COMPONENTS_KRX_VISUALS_PRE_TAS_H
#define GAME_CLIENT_COMPONENTS_KRX_VISUALS_PRE_TAS_H

#include <base/vmath.h>

#include <cstdint>

class CGameClient;

namespace OpenKrx
{
	// Минимальный порт Vertex: из CPreTasVisuals взят только AFK-гейт,
	// который вызывает CAvoid::AdjustInput (avoid.cpp) и CClassicAvoid
	// (classic_agents.cpp). ESP-траектории/аутлайнеры не переносятся.
	// Вызывай Init(this) один раз из CGameClient::OnInit().
	class CPreTasVisuals
	{
		// Original 0x1402cc7a0 uses one timer/mouse pair across both local slots.
		int64_t m_AfkLastActivity = 0;
		vec2 m_AfkMouse = vec2(0, 0);
		CGameClient *m_pGameClient = nullptr;

	public:
		void Init(CGameClient *pGameClient) { m_pGameClient = pGameClient; }
		void OnReset() {}
		bool ShouldSuspendAvoid();
		void PredictTrajectory() {}
	};
}

#endif
