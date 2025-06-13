#include <float.h>
#include <math.h>
#include <stdint.h>
#include "libm.h"

#if FLT_EVAL_METHOD==0 || FLT_EVAL_METHOD==1
#define EPS DBL_EPSILON
#elif FLT_EVAL_METHOD==2
#define EPS LDBL_EPSILON
#endif
static const double_t toint = 1/EPS;

double __cdecl rint(double x)
{
	union {double f; uint64_t i;} u = {x};
	int e = u.i>>52 & 0x7ff;
	int s = u.i>>63;
	unsigned cw;
	double_t y;

	if (e >= 0x3ff+52)
		return x;
	cw = _controlfp(0, 0);
	if ((cw & _MCW_PC) != _PC_53)
		_controlfp(_PC_53, _MCW_PC);
	if (s)
		y = fp_barrier(x - toint) + toint;
	else
		y = fp_barrier(x + toint) - toint;
	if ((cw & _MCW_PC) != _PC_53)
		_controlfp(cw, _MCW_PC);
	if (y == 0)
		return s ? -0.0 : 0;
	return y;
}
