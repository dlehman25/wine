/* https://www.winehq.org/pipermail/wine-devel/2004-March/025351.html */

#include <wine/library.h>
#include <stdio.h>
#include <windows.h>
#include <winternl.h>
#include <dlfcn.h>
#include <setjmp.h>

static jmp_buf jmpbuf;

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpszCmdLine, int nCmdShow)
{
    longjmp( jmpbuf, 1 );
}

typedef void (*__wine_main_t)( int argc, char *argv[], char *envp[] );

int SharedWineInit(void)
{
    char *WineArguments[2];
    char *cmdline;
    void *ntdll;
    __wine_main_t __wine_main;

    ntdll = dlopen(DLLPATH "/ntdll.so", RTLD_NOW);
    printf("ntdll %p\n", ntdll);
    __wine_main = (__wine_main_t)dlsym(ntdll, "__wine_main");
    printf("__wine_main %p\n", __wine_main);

    cmdline = strdup("sharedapp " DLLPATH "/winesharedlib.exe.so");
    printf("%s\n", cmdline);
    WineArguments[0] = cmdline;
    WineArguments[1] = strstr(cmdline, LIBPATH);

    if (!setjmp( jmpbuf ))
    {
        __wine_main(2, WineArguments, NULL);
        printf( "should not get here\n" );
    }
    NtCurrentTeb()->Tib.ExceptionList = (void *)~0UL;
    VirtualFree( NtCurrentTeb()->DeallocationStack, 0, MEM_RELEASE );
    NtCurrentTeb()->DeallocationStack = NULL;
    NtCurrentTeb()->Tib.StackBase = (void*)~0UL;  /* FIXME: should find actual limits here */
    NtCurrentTeb()->Tib.StackLimit = 0;
    return(0);
}

int SharedWineInit2(void)
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
