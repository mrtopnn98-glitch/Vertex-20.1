#include "krx_misc_pre_tas.h"

#include <cstdint>

namespace OpenKrx
{
	namespace
	{
		thread_local uint32_t s_ReferenceRand = 1;
	}

	unsigned CKrxMiscPreTas::ReferenceRand()
	{
		s_ReferenceRand = s_ReferenceRand * 214013u + 2531011u;
		return (s_ReferenceRand >> 16) & 0x7fffu;
	}

	void CKrxMiscPreTas::ReferenceSrand(unsigned Seed)
	{
		s_ReferenceRand = Seed;
	}
}
