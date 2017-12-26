
#pragma once

#define NP_DBG 0

#if NP_DBG
	#include <stdio.h>
	#define NP_FPRINTF(_x) { fprintf _x; }
#else
	#define NP_FPRINTF(_x) {}
#endif
