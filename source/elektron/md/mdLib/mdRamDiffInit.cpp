#include "mdramdiff.h"

#include "mdRamLabels.h"

#if GEARULATOR_MDMM_RAM_DIAGNOSTICS

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