#include <float.h>
#include <math.h>
#include <stdint.h>
#include "libm.h"

double __cdecl rint(double x)
{
	union {double f; uint64_t i;} u = {x};
	int e = u.i>>52 & 0x7ff;
	int s = u.i>>63;
	double_t toint, y;

	if (e >= 0x3ff+52)
		return x;

	if ((_control87(0, 0) & _MCW_PC) == _PC_24) toint = 1 / FLT_EPSILON;
	else toint = 1 / DBL_EPSILON;

	if (s)
		y = fp_barrier(x - toint) + toint;
	else
		y = fp_barrier(x + toint) - toint;
	if (y == 0)
		return s ? -0.0 : 0;
	return y;
}
