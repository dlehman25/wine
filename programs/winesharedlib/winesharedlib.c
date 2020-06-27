/* https://www.winehq.org/pipermail/wine-devel/2004-March/025351.html */

#include <wine/library.h>
#include <stdio.h>
#include <windows.h>
#include <winternl.h>
#include <setjmp.h>

static jmp_buf jmpbuf;

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpszCmdLine, int nCmdShow)
{
    longjmp( jmpbuf, 1 );
}

int SharedWineInit(void)
{
    char Error[1024]="";
    char *WineArguments[2];
    char *cmdline;

    cmdline = strdup("sharedapp " LIBPATH "/wine-sharedlib.exe.so");
    WineArguments[0] = cmdline;
    WineArguments[1] = strstr(cmdline, LIBPATH);

    if (!setjmp( jmpbuf ))
    {
        wine_init(2, WineArguments, Error, sizeof(Error));
        printf( "should not get here\n" );
    }
    NtCurrentTeb()->Tib.ExceptionList = (void *)~0UL;
    VirtualFree( NtCurrentTeb()->DeallocationStack, 0, MEM_RELEASE );
    NtCurrentTeb()->DeallocationStack = NULL;
    NtCurrentTeb()->Tib.StackBase = (void*)~0UL;  /* FIXME: should find actual limits here */
    NtCurrentTeb()->Tib.StackLimit = 0;
    return(0);
}
