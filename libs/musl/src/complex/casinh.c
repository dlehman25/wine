#include "complex_impl.h"

/* asinh(z) = -i asin(i z) */

_Dcomplex casinh(_Dcomplex z)
{
	z = casin(CMPLX(-cimag(z), creal(z)));
	return CMPLX(cimag(z), -creal(z));
}
