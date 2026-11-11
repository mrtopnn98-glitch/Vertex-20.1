#ifndef GAME_CLIENT_COMPONENTS_KRX_MISC_PRE_TAS_H
#define GAME_CLIENT_COMPONENTS_KRX_MISC_PRE_TAS_H

namespace OpenKrx
{
	// Минимальный порт Vertex: из CKrxMiscPreTas взят только референсный LCG
	// (ReferenceRand/ReferenceSrand), который использует classic_agents.cpp.
	// Остальные фичи компонента (vote-spam, бенчмарки и т.д.) не переносятся.
	class CKrxMiscPreTas
	{
	public:
		static unsigned ReferenceRand();
		static void ReferenceSrand(unsigned Seed);
	};
}

#endif
