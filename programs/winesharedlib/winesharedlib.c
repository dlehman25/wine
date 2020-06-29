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
typedef void (*__wine_main2_t)( int argc, char *argv[], char *envp[] );

static NTSTATUS (*NTDLL_LdrGetDllPath)(PCWSTR,ULONG,PWSTR*,PWSTR*);
static NTSTATUS (*NTDLL_LdrGetProcedureAddress)(HMODULE,const ANSI_STRING*,ULONG, PVOID*);
static NTSTATUS (*NTDLL_LdrLoadDll)(LPCWSTR,DWORD,const UNICODE_STRING*,HMODULE*);

void *WineLoadLibrary(const char *dll)
{
    /* see dlls/kernelbase/loader.c */
    WCHAR *load_path, *dummy;
    UNICODE_STRING str;
    NTSTATUS status;
    HMODULE module;
    USHORT i;

    printf("%s: %s\n", __FUNCTION__, dll);

    /* Rtl functions? */
    /* TODO: A -> W properly */
    str.Length = strlen(dll) * sizeof(WCHAR);
    str.MaximumLength = str.Length + sizeof(WCHAR);
    str.Buffer = malloc(str.MaximumLength);
    for (i = 0; i < str.Length / sizeof(WCHAR); i++)
        str.Buffer[i] = dll[i];
    str.Buffer[i] = 0;

    printf("%s: NTDLL_LdrGetDllPath %p\n", __FUNCTION__, NTDLL_LdrGetDllPath);
    status = NTDLL_LdrGetDllPath(str.Buffer, 0, &load_path, &dummy);
    printf("LdrGetDllPath %x: \n", status);
    status = NTDLL_LdrLoadDll(load_path, 0, &str, &module);
    printf("LdrLoadDll %x: \n", status);

    free(str.Buffer);

    return module;
}

void *WineGetProcAddress(void *handle, const char *path)
{
    printf("%s: handle %p path %s\n", __FUNCTION__, handle, path);
    return NULL;
}

void wine_adopt_thread(void)
{
    printf("%s:\n", __FUNCTION__);
}

int wine_is_thread_adopted(void)
{
    printf("%s:\n", __FUNCTION__);
    return 0;
}

int SharedWineInit(void)
{
    char *WineArguments[2];
    char *cmdline;
    char **envp;
    void *ntdll;
    __wine_main2_t __wine_main2;
    char shared[] = "WINESHAREDLIB=1";

    putenv(shared);
    ntdll = dlopen(DLLPATH "/ntdll.so", RTLD_NOW);
    printf("ntdll %p\n", ntdll);
    __wine_main2 = (__wine_main2_t)dlsym(ntdll, "__wine_main2");
    printf("__wine_main2 %p\n", __wine_main2);
    NTDLL_LdrGetDllPath = dlsym(ntdll, "NTDLL_LdrGetDllPath");
    printf("NTDLL_LdrGetDllPath %p\n", NTDLL_LdrGetDllPath);fflush(stdout);
    NTDLL_LdrLoadDll = dlsym(ntdll, "NTDLL_LdrLoadDll");
    printf("NTDLL_LdrLoadDll %p\n", NTDLL_LdrLoadDll);fflush(stdout);
    NTDLL_LdrGetProcedureAddress = dlsym(ntdll, "NTDLL_LdrGetProcedureAddress");
    printf("NTDLL_LdrGetProcedureAddress %p\n", NTDLL_LdrGetProcedureAddress);fflush(stdout);

    cmdline = strdup("/home/phantom/stuff/wine/64/output/bin/wine64 " DLLPATH "/wineconsole.exe.so");
    printf("%s\n", cmdline);
    WineArguments[0] = cmdline;
    WineArguments[1] = strstr(cmdline, DLLPATH);
    WineArguments[1][-1] = 0;

    envp = malloc(1 * sizeof(char*));
    envp[0] = 0;
    __wine_main2(2, WineArguments, envp);
    return 0;
}

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

int SharedWineInit3(void)
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
