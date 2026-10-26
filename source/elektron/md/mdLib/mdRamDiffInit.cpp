#include "mdramdiff.h"

#include "mdRamLabels.h"

#if GEARMULATOR_MDMM_RAM_DIAGNOSTICS

namespace md
{
namespace ramDiff
{

void initialize()
{
	initRamLabels();
}

}
}

#endif