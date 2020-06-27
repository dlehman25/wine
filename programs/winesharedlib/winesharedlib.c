/* https://www.winehq.org/pipermail/wine-devel/2004-March/025351.html */

#include <wine/library.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <winternl.h>
#include <dlfcn.h>
#include <setjmp.h>
#include <sys/types.h>
#include <unistd.h>

static jmp_buf jmpbuf;

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpszCmdLine, int nCmdShow)
{
    longjmp( jmpbuf, 1 );
    return 0;
}

typedef void (*__wine_main_t)( int argc, char *argv[], char *envp[] );

int SharedWineInit2(void)
{
    char *WineArguments[2];
    char *cmdline;
    char **envp;
    void *ntdll;
    __wine_main_t __wine_main;

    ntdll = dlopen(DLLPATH "/ntdll.so", RTLD_NOW);
    printf("ntdll %p\n", ntdll);
    __wine_main = (__wine_main_t)dlsym(ntdll, "__wine_main");
    printf("__wine_main %p\n", __wine_main);

    if (!setjmp( jmpbuf ))
    {
        char noexec[] = "WINELOADERNOEXEC=1";
        if (!fork())
        {
            cmdline = strdup("/home/phantom/stuff/wine/64/output/bin/wine64 " DLLPATH "/wineconsole.exe.so");
            printf("%s\n", cmdline);
            WineArguments[0] = cmdline;
            WineArguments[1] = strstr(cmdline, DLLPATH);
            WineArguments[1][-1] = 0;

            envp = malloc(1 * sizeof(char*));
            envp[0] = 0;

            __wine_main(2, WineArguments, envp);
        }

        cmdline = strdup("/home/phantom/stuff/wine/64/output/bin/wine64 " DLLPATH "/winesharedlib.exe.so");
        printf("%s\n", cmdline);
        WineArguments[0] = cmdline;
        WineArguments[1] = strstr(cmdline, DLLPATH);
        WineArguments[1][-1] = 0;
        putenv(noexec);
        envp = malloc(1 * sizeof(char*));
        envp[0] = 0;
        __wine_main(2, WineArguments, envp);
        printf( "should not get here\n" );
    }
    NtCurrentTeb()->Tib.ExceptionList = (void *)~0UL;
    VirtualFree( NtCurrentTeb()->DeallocationStack, 0, MEM_RELEASE );
    NtCurrentTeb()->DeallocationStack = NULL;
    NtCurrentTeb()->Tib.StackBase = (void*)~0UL;  /* FIXME: should find actual limits here */
    NtCurrentTeb()->Tib.StackLimit = 0;
    return(0);
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
