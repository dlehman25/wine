/**
 * This file has no copyright assigned and is placed in the Public Domain.
 * This file is part of the Wine project.
 */

#ifndef _COMPLEX_H_DEFINED
#define _COMPLEX_H_DEFINED

#include <corecrt.h>

#ifndef _C_COMPLEX_T
#define _C_COMPLEX_T
typedef struct _C_double_complex
{
    double _Val[2];
} _C_double_complex;

typedef struct _C_float_complex
{
    float _Val[2];
} _C_float_complex;
#endif

typedef _C_double_complex _Dcomplex;
typedef _C_float_complex _Fcomplex;

double    __cdecl carg(_Dcomplex);
_Dcomplex __cdecl cexp(_Dcomplex);
double    __cdecl cimag(_Dcomplex);
double    __cdecl creal(_Dcomplex);

float     __cdecl cargf(_Fcomplex);
_Fcomplex __cdecl cexpf(_Fcomplex);
float     __cdecl cimagf(_Fcomplex);
float     __cdecl crealf(_Fcomplex);

#endif /* _COMPLEX_H_DEFINED */
