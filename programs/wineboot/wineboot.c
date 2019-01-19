/*
 * Copyright (C) 2002 Andreas Mohr
 * Copyright (C) 2002 Shachar Shemesh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */
/* Wine "bootup" handler application
 *
 * This app handles the various "hooks" windows allows for applications to perform
 * as part of the bootstrap process. These are roughly divided into three types.
 * Knowledge base articles that explain this are 137367, 179365, 232487 and 232509.
 * Also, 119941 has some info on grpconv.exe
 * The operations performed are (by order of execution):
 *
 * Preboot (prior to fully loading the Windows kernel):
 * - wininit.exe (rename operations left in wininit.ini - Win 9x only)
 * - PendingRenameOperations (rename operations left in the registry - Win NT+ only)
 *
 * Startup (before the user logs in)
 * - Services (NT)
 * - HKEY_LOCAL_MACHINE\Software\Microsoft\Windows\CurrentVersion\RunServicesOnce (9x, asynch)
 * - HKEY_LOCAL_MACHINE\Software\Microsoft\Windows\CurrentVersion\RunServices (9x, asynch)
 * 
 * After log in
 * - HKEY_LOCAL_MACHINE\Software\Microsoft\Windows\CurrentVersion\RunOnce (all, synch)
 * - HKEY_LOCAL_MACHINE\Software\Microsoft\Windows\CurrentVersion\Run (all, asynch)
 * - HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run (all, asynch)
 * - Startup folders (all, ?asynch?)
 * - HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\RunOnce (all, asynch)
 *
 * Somewhere in there is processing the RunOnceEx entries (also no imp)
 * 
 * Bugs:
 * - If a pending rename registry does not start with \??\ the entry is
 *   processed anyways. I'm not sure that is the Windows behaviour.
 * - Need to check what is the windows behaviour when trying to delete files
 *   and directories that are read-only
 * - In the pending rename registry processing - there are no traces of the files
 *   processed (requires translations from Unicode to Ansi).
 */

#include "config.h"
#include "wine/port.h"

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifdef HAVE_GETOPT_H
# include <getopt.h>
#endif
#ifdef HAVE_SYS_STAT_H
# include <sys/stat.h>
#endif
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#include <windows.h>
#include <winternl.h>
#include <wine/svcctl.h>
#include <wine/unicode.h>
#include <wine/library.h>
#include <wine/debug.h>
#include <wine/list.h>
#include <wine/heap.h>

#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <shellapi.h>
#include "resource.h"

WINE_DEFAULT_DEBUG_CHANNEL(wineboot);

extern BOOL shutdown_close_windows( BOOL force );
extern BOOL shutdown_all_desktops( BOOL force );
extern void kill_processes( BOOL kill_desktop );

static WCHAR windowsdir[MAX_PATH];

/* retrieve the (unix) path to the wine.inf file */
static char *get_wine_inf_path(void)
{
    const char *build_dir, *data_dir;
    char *name = NULL;

    if ((data_dir = wine_get_data_dir()))
    {
        if (!(name = HeapAlloc( GetProcessHeap(), 0, strlen(data_dir) + sizeof("/wine.inf") )))
            return NULL;
        strcpy( name, data_dir );
        strcat( name, "/wine.inf" );
    }
    else if ((build_dir = wine_get_build_dir()))
    {
        if (!(name = HeapAlloc( GetProcessHeap(), 0, strlen(build_dir) + sizeof("/loader/wine.inf") )))
            return NULL;
        strcpy( name, build_dir );
        strcat( name, "/loader/wine.inf" );
    }
    return name;
}

/* update the timestamp if different from the reference time */
static BOOL update_timestamp( const char *config_dir, unsigned long timestamp )
{
    BOOL ret = FALSE;
    int fd, count;
    char buffer[100];
    char *file = HeapAlloc( GetProcessHeap(), 0, strlen(config_dir) + sizeof("/.update-timestamp") );

    if (!file) return FALSE;
    strcpy( file, config_dir );
    strcat( file, "/.update-timestamp" );

    if ((fd = open( file, O_RDWR )) != -1)
    {
        if ((count = read( fd, buffer, sizeof(buffer) - 1 )) >= 0)
        {
            buffer[count] = 0;
            if (!strncmp( buffer, "disable", sizeof("disable")-1 )) goto done;
            if (timestamp == strtoul( buffer, NULL, 10 )) goto done;
        }
        lseek( fd, 0, SEEK_SET );
        ftruncate( fd, 0 );
    }
    else
    {
        if (errno != ENOENT) goto done;
        if ((fd = open( file, O_WRONLY | O_CREAT | O_TRUNC, 0666 )) == -1) goto done;
    }

    count = sprintf( buffer, "%lu\n", timestamp );
    if (write( fd, buffer, count ) != count)
    {
        WINE_WARN( "failed to update timestamp in %s\n", file );
        ftruncate( fd, 0 );
    }
    else ret = TRUE;

done:
    if (fd != -1) close( fd );
    HeapFree( GetProcessHeap(), 0, file );
    return ret;
}

/* wrapper for RegSetValueExW */
static DWORD set_reg_value( HKEY hkey, const WCHAR *name, const WCHAR *value )
{
    return RegSetValueExW( hkey, name, 0, REG_SZ, (const BYTE *)value, (strlenW(value) + 1) * sizeof(WCHAR) );
}

/* create the volatile hardware registry keys */
static void create_hardware_registry_keys(void)
{
    static const WCHAR SystemW[] = {'H','a','r','d','w','a','r','e','\\',
                                    'D','e','s','c','r','i','p','t','i','o','n','\\',
                                    'S','y','s','t','e','m',0};
    static const WCHAR fpuW[] = {'F','l','o','a','t','i','n','g','P','o','i','n','t','P','r','o','c','e','s','s','o','r',0};
    static const WCHAR cpuW[] = {'C','e','n','t','r','a','l','P','r','o','c','e','s','s','o','r',0};
    static const WCHAR FeatureSetW[] = {'F','e','a','t','u','r','e','S','e','t',0};
    static const WCHAR IdentifierW[] = {'I','d','e','n','t','i','f','i','e','r',0};
    static const WCHAR ProcessorNameStringW[] = {'P','r','o','c','e','s','s','o','r','N','a','m','e','S','t','r','i','n','g',0};
    static const WCHAR SysidW[] = {'A','T',' ','c','o','m','p','a','t','i','b','l','e',0};
    static const WCHAR ARMSysidW[] = {'A','R','M',' ','p','r','o','c','e','s','s','o','r',' ','f','a','m','i','l','y',0};
    static const WCHAR mhzKeyW[] = {'~','M','H','z',0};
    static const WCHAR VendorIdentifierW[] = {'V','e','n','d','o','r','I','d','e','n','t','i','f','i','e','r',0};
    static const WCHAR VenidIntelW[] = {'G','e','n','u','i','n','e','I','n','t','e','l',0};
    /* static const WCHAR VenidAMDW[] = {'A','u','t','h','e','n','t','i','c','A','M','D',0}; */
    static const WCHAR PercentDW[] = {'%','d',0};
    static const WCHAR IntelCpuDescrW[] = {'x','8','6',' ','F','a','m','i','l','y',' ','%','d',' ','M','o','d','e','l',' ','%','d',
                                           ' ','S','t','e','p','p','i','n','g',' ','%','d',0};
    static const WCHAR ARMCpuDescrW[]  = {'A','R','M',' ','F','a','m','i','l','y',' ','%','d',' ','M','o','d','e','l',' ','%','d',
                                            ' ','R','e','v','i','s','i','o','n',' ','%','d',0};
    static const WCHAR IntelCpuStringW[] = {'I','n','t','e','l','(','R',')',' ','P','e','n','t','i','u','m','(','R',')',' ','4',' ',
                                            'C','P','U',' ','2','.','4','0','G','H','z',0};
    unsigned int i;
    HKEY hkey, system_key, cpu_key, fpu_key;
    SYSTEM_CPU_INFORMATION sci;
    PROCESSOR_POWER_INFORMATION* power_info;
    ULONG sizeof_power_info = sizeof(PROCESSOR_POWER_INFORMATION) * NtCurrentTeb()->Peb->NumberOfProcessors;
    WCHAR idW[60];

    NtQuerySystemInformation( SystemCpuInformation, &sci, sizeof(sci), NULL );

    power_info = HeapAlloc( GetProcessHeap(), 0, sizeof_power_info );
    if (power_info == NULL)
        return;
    if (NtPowerInformation( ProcessorInformation, NULL, 0, power_info, sizeof_power_info ))
        memset( power_info, 0, sizeof_power_info );

    /*TODO: report 64bit processors properly*/
    switch(sci.Architecture)
    {
    case PROCESSOR_ARCHITECTURE_ARM:
    case PROCESSOR_ARCHITECTURE_ARM64:
        sprintfW( idW, ARMCpuDescrW, sci.Level, HIBYTE(sci.Revision), LOBYTE(sci.Revision) );
        break;
    default:
    case PROCESSOR_ARCHITECTURE_INTEL:
        sprintfW( idW, IntelCpuDescrW, sci.Level, HIBYTE(sci.Revision), LOBYTE(sci.Revision) );
        break;
    }

    if (RegCreateKeyExW( HKEY_LOCAL_MACHINE, SystemW, 0, NULL, REG_OPTION_VOLATILE,
                         KEY_ALL_ACCESS, NULL, &system_key, NULL ))
    {
        HeapFree( GetProcessHeap(), 0, power_info );
        return;
    }

    switch(sci.Architecture)
    {
    case PROCESSOR_ARCHITECTURE_ARM:
    case PROCESSOR_ARCHITECTURE_ARM64:
        set_reg_value( system_key, IdentifierW, ARMSysidW );
        break;
    default:
    case PROCESSOR_ARCHITECTURE_INTEL:
        set_reg_value( system_key, IdentifierW, SysidW );
        break;
    }

    if (sci.Architecture == PROCESSOR_ARCHITECTURE_ARM ||
        sci.Architecture == PROCESSOR_ARCHITECTURE_ARM64 ||
        RegCreateKeyExW( system_key, fpuW, 0, NULL, REG_OPTION_VOLATILE,
                         KEY_ALL_ACCESS, NULL, &fpu_key, NULL ))
        fpu_key = 0;
    if (RegCreateKeyExW( system_key, cpuW, 0, NULL, REG_OPTION_VOLATILE,
                         KEY_ALL_ACCESS, NULL, &cpu_key, NULL ))
        cpu_key = 0;

    for (i = 0; i < NtCurrentTeb()->Peb->NumberOfProcessors; i++)
    {
        WCHAR numW[10];

        sprintfW( numW, PercentDW, i );
        if (!RegCreateKeyExW( cpu_key, numW, 0, NULL, REG_OPTION_VOLATILE,
                              KEY_ALL_ACCESS, NULL, &hkey, NULL ))
        {
            RegSetValueExW( hkey, FeatureSetW, 0, REG_DWORD, (BYTE *)&sci.FeatureSet, sizeof(DWORD) );
            set_reg_value( hkey, IdentifierW, idW );
            /*TODO; report ARM and AMD properly*/
            set_reg_value( hkey, ProcessorNameStringW, IntelCpuStringW );
            set_reg_value( hkey, VendorIdentifierW, VenidIntelW );
            RegSetValueExW( hkey, mhzKeyW, 0, REG_DWORD, (BYTE *)&power_info[i].MaxMhz, sizeof(DWORD) );
            RegCloseKey( hkey );
        }
        if (sci.Architecture != PROCESSOR_ARCHITECTURE_ARM &&
            sci.Architecture != PROCESSOR_ARCHITECTURE_ARM64 &&
            !RegCreateKeyExW( fpu_key, numW, 0, NULL, REG_OPTION_VOLATILE,
                              KEY_ALL_ACCESS, NULL, &hkey, NULL ))
        {
            set_reg_value( hkey, IdentifierW, idW );
            RegCloseKey( hkey );
        }
    }
    RegCloseKey( fpu_key );
    RegCloseKey( cpu_key );
    RegCloseKey( system_key );
    HeapFree( GetProcessHeap(), 0, power_info );
}


/* create the DynData registry keys */
static void create_dynamic_registry_keys(void)
{
    static const WCHAR StatDataW[] = {'P','e','r','f','S','t','a','t','s','\\',
                                      'S','t','a','t','D','a','t','a',0};
    static const WCHAR ConfigManagerW[] = {'C','o','n','f','i','g',' ','M','a','n','a','g','e','r','\\',
                                           'E','n','u','m',0};
    HKEY key;

    if (!RegCreateKeyExW( HKEY_DYN_DATA, StatDataW, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL ))
        RegCloseKey( key );
    if (!RegCreateKeyExW( HKEY_DYN_DATA, ConfigManagerW, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL ))
        RegCloseKey( key );
}

/* create the platform-specific environment registry keys */
static void create_environment_registry_keys( void )
{
    static const WCHAR EnvironW[]  = {'S','y','s','t','e','m','\\',
                                      'C','u','r','r','e','n','t','C','o','n','t','r','o','l','S','e','t','\\',
                                      'C','o','n','t','r','o','l','\\',
                                      'S','e','s','s','i','o','n',' ','M','a','n','a','g','e','r','\\',
                                      'E','n','v','i','r','o','n','m','e','n','t',0};
    static const WCHAR NumProcW[]  = {'N','U','M','B','E','R','_','O','F','_','P','R','O','C','E','S','S','O','R','S',0};
    static const WCHAR ProcArchW[] = {'P','R','O','C','E','S','S','O','R','_','A','R','C','H','I','T','E','C','T','U','R','E',0};
    static const WCHAR x86W[]      = {'x','8','6',0};
    static const WCHAR armW[]      = {'A','R','M',0};
    static const WCHAR arm64W[]    = {'A','R','M','6','4',0};
    static const WCHAR AMD64W[]    = {'A','M','D','6','4',0};
    static const WCHAR ProcIdW[]   = {'P','R','O','C','E','S','S','O','R','_','I','D','E','N','T','I','F','I','E','R',0};
    static const WCHAR ProcLvlW[]  = {'P','R','O','C','E','S','S','O','R','_','L','E','V','E','L',0};
    static const WCHAR ProcRevW[]  = {'P','R','O','C','E','S','S','O','R','_','R','E','V','I','S','I','O','N',0};
    static const WCHAR PercentDW[] = {'%','d',0};
    static const WCHAR Percent04XW[] = {'%','0','4','x',0};
    static const WCHAR IntelCpuDescrW[]  = {'%','s',' ','F','a','m','i','l','y',' ','%','d',' ','M','o','d','e','l',' ','%','d',
                                            ' ','S','t','e','p','p','i','n','g',' ','%','d',',',' ','G','e','n','u','i','n','e','I','n','t','e','l',0};
    static const WCHAR ARMCpuDescrW[]  = {'A','R','M',' ','F','a','m','i','l','y',' ','%','d',' ','M','o','d','e','l',' ','%','d',
                                            ' ','R','e','v','i','s','i','o','n',' ','%','d',0};

    HKEY env_key;
    SYSTEM_CPU_INFORMATION sci;
    WCHAR buffer[60];
    const WCHAR *arch;

    if (RegCreateKeyW( HKEY_LOCAL_MACHINE, EnvironW, &env_key )) return;

    NtQuerySystemInformation( SystemCpuInformation, &sci, sizeof(sci), NULL );

    sprintfW( buffer, PercentDW, NtCurrentTeb()->Peb->NumberOfProcessors );
    set_reg_value( env_key, NumProcW, buffer );

    switch(sci.Architecture)
    {
    case PROCESSOR_ARCHITECTURE_AMD64: arch = AMD64W; break;
    case PROCESSOR_ARCHITECTURE_ARM:   arch = armW; break;
    case PROCESSOR_ARCHITECTURE_ARM64: arch = arm64W; break;
    default:
    case PROCESSOR_ARCHITECTURE_INTEL: arch = x86W; break;
    }
    set_reg_value( env_key, ProcArchW, arch );

    switch(sci.Architecture)
    {
    case PROCESSOR_ARCHITECTURE_ARM:
    case PROCESSOR_ARCHITECTURE_ARM64:
        sprintfW( buffer, ARMCpuDescrW, sci.Level, HIBYTE(sci.Revision), LOBYTE(sci.Revision) );
        break;
    default:
    case PROCESSOR_ARCHITECTURE_INTEL:
        sprintfW( buffer, IntelCpuDescrW, arch, sci.Level, HIBYTE(sci.Revision), LOBYTE(sci.Revision) );
        break;
    }
    set_reg_value( env_key, ProcIdW, buffer );

    sprintfW( buffer, PercentDW, sci.Level );
    set_reg_value( env_key, ProcLvlW, buffer );

    /* Properly report model/stepping */
    sprintfW( buffer, Percent04XW, sci.Revision );
    set_reg_value( env_key, ProcRevW, buffer );

    RegCloseKey( env_key );
}

static void create_volatile_environment_registry_key(void)
{
    static const WCHAR VolatileEnvW[] = {'V','o','l','a','t','i','l','e',' ','E','n','v','i','r','o','n','m','e','n','t',0};
    static const WCHAR AppDataW[] = {'A','P','P','D','A','T','A',0};
    static const WCHAR ClientNameW[] = {'C','L','I','E','N','T','N','A','M','E',0};
    static const WCHAR HomeDriveW[] = {'H','O','M','E','D','R','I','V','E',0};
    static const WCHAR HomePathW[] = {'H','O','M','E','P','A','T','H',0};
    static const WCHAR HomeShareW[] = {'H','O','M','E','S','H','A','R','E',0};
    static const WCHAR LocalAppDataW[] = {'L','O','C','A','L','A','P','P','D','A','T','A',0};
    static const WCHAR LogonServerW[] = {'L','O','G','O','N','S','E','R','V','E','R',0};
    static const WCHAR SessionNameW[] = {'S','E','S','S','I','O','N','N','A','M','E',0};
    static const WCHAR UserNameW[] = {'U','S','E','R','N','A','M','E',0};
    static const WCHAR UserDomainW[] = {'U','S','E','R','D','O','M','A','I','N',0};
    static const WCHAR UserProfileW[] = {'U','S','E','R','P','R','O','F','I','L','E',0};
    static const WCHAR ConsoleW[] = {'C','o','n','s','o','l','e',0};
    static const WCHAR EmptyW[] = {0};
    WCHAR path[MAX_PATH];
    WCHAR computername[MAX_COMPUTERNAME_LENGTH + 1 + 2];
    DWORD size;
    HKEY hkey;
    HRESULT hr;

    if (RegCreateKeyExW( HKEY_CURRENT_USER, VolatileEnvW, 0, NULL, REG_OPTION_VOLATILE,
                         KEY_ALL_ACCESS, NULL, &hkey, NULL ))
        return;

    hr = SHGetFolderPathW( NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, path );
    if (SUCCEEDED(hr)) set_reg_value( hkey, AppDataW, path );

    set_reg_value( hkey, ClientNameW, ConsoleW );

    /* Write the profile path's drive letter and directory components into
     * HOMEDRIVE and HOMEPATH respectively. */
    hr = SHGetFolderPathW( NULL, CSIDL_PROFILE, NULL, SHGFP_TYPE_CURRENT, path );
    if (SUCCEEDED(hr))
    {
        set_reg_value( hkey, UserProfileW, path );
        set_reg_value( hkey, HomePathW, path + 2 );
        path[2] = '\0';
        set_reg_value( hkey, HomeDriveW, path );
    }

    size = ARRAY_SIZE(path);
    if (GetUserNameW( path, &size )) set_reg_value( hkey, UserNameW, path );

    set_reg_value( hkey, HomeShareW, EmptyW );

    hr = SHGetFolderPathW( NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, path );
    if (SUCCEEDED(hr))
        set_reg_value( hkey, LocalAppDataW, path );

    size = ARRAY_SIZE(computername) - 2;
    if (GetComputerNameW(&computername[2], &size))
    {
        set_reg_value( hkey, UserDomainW, &computername[2] );
        computername[0] = computername[1] = '\\';
        set_reg_value( hkey, LogonServerW, computername );
    }

    set_reg_value( hkey, SessionNameW, ConsoleW );
    RegCloseKey( hkey );
}

/* Performs the rename operations dictated in %SystemRoot%\Wininit.ini.
 * Returns FALSE if there was an error, or otherwise if all is ok.
 */
static BOOL wininit(void)
{
    static const WCHAR nulW[] = {'N','U','L',0};
    static const WCHAR renameW[] = {'r','e','n','a','m','e',0};
    static const WCHAR wininitW[] = {'w','i','n','i','n','i','t','.','i','n','i',0};
    static const WCHAR wininitbakW[] = {'w','i','n','i','n','i','t','.','b','a','k',0};
    WCHAR initial_buffer[1024];
    WCHAR *str, *buffer = initial_buffer;
    DWORD size = ARRAY_SIZE(initial_buffer);
    DWORD res;

    for (;;)
    {
        if (!(res = GetPrivateProfileSectionW( renameW, buffer, size, wininitW ))) return TRUE;
        if (res < size - 2) break;
        if (buffer != initial_buffer) HeapFree( GetProcessHeap(), 0, buffer );
        size *= 2;
        if (!(buffer = HeapAlloc( GetProcessHeap(), 0, size * sizeof(WCHAR) ))) return FALSE;
    }

    for (str = buffer; *str; str += strlenW(str) + 1)
    {
        WCHAR *value;

        if (*str == ';') continue;  /* comment */
        if (!(value = strchrW( str, '=' ))) continue;

        /* split the line into key and value */
        *value++ = 0;

        if (!lstrcmpiW( nulW, str ))
        {
            WINE_TRACE("Deleting file %s\n", wine_dbgstr_w(value) );
            if( !DeleteFileW( value ) )
                WINE_WARN("Error deleting file %s\n", wine_dbgstr_w(value) );
        }
        else
        {
            WINE_TRACE("Renaming file %s to %s\n", wine_dbgstr_w(value), wine_dbgstr_w(str) );

            if( !MoveFileExW(value, str, MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING) )
                WINE_WARN("Error renaming %s to %s\n", wine_dbgstr_w(value), wine_dbgstr_w(str) );
        }
        str = value;
    }

    if (buffer != initial_buffer) HeapFree( GetProcessHeap(), 0, buffer );

    if( !MoveFileExW( wininitW, wininitbakW, MOVEFILE_REPLACE_EXISTING) )
    {
        WINE_ERR("Couldn't rename wininit.ini, error %d\n", GetLastError() );

        return FALSE;
    }

    return TRUE;
}

static BOOL pendingRename(void)
{
    static const WCHAR ValueName[] = {'P','e','n','d','i','n','g',
                                      'F','i','l','e','R','e','n','a','m','e',
                                      'O','p','e','r','a','t','i','o','n','s',0};
    static const WCHAR SessionW[] = { 'S','y','s','t','e','m','\\',
                                     'C','u','r','r','e','n','t','C','o','n','t','r','o','l','S','e','t','\\',
                                     'C','o','n','t','r','o','l','\\',
                                     'S','e','s','s','i','o','n',' ','M','a','n','a','g','e','r',0};
    WCHAR *buffer=NULL;
    const WCHAR *src=NULL, *dst=NULL;
    DWORD dataLength=0;
    HKEY hSession=NULL;
    DWORD res;

    WINE_TRACE("Entered\n");

    if( (res=RegOpenKeyExW( HKEY_LOCAL_MACHINE, SessionW, 0, KEY_ALL_ACCESS, &hSession ))
            !=ERROR_SUCCESS )
    {
        WINE_TRACE("The key was not found - skipping\n");
        return TRUE;
    }

    res=RegQueryValueExW( hSession, ValueName, NULL, NULL /* The value type does not really interest us, as it is not
                                                             truly a REG_MULTI_SZ anyways */,
            NULL, &dataLength );
    if( res==ERROR_FILE_NOT_FOUND )
    {
        /* No value - nothing to do. Great! */
        WINE_TRACE("Value not present - nothing to rename\n");
        res=TRUE;
        goto end;
    }

    if( res!=ERROR_SUCCESS )
    {
        WINE_ERR("Couldn't query value's length (%d)\n", res );
        res=FALSE;
        goto end;
    }

    buffer=HeapAlloc( GetProcessHeap(),0,dataLength );
    if( buffer==NULL )
    {
        WINE_ERR("Couldn't allocate %u bytes for the value\n", dataLength );
        res=FALSE;
        goto end;
    }

    res=RegQueryValueExW( hSession, ValueName, NULL, NULL, (LPBYTE)buffer, &dataLength );
    if( res!=ERROR_SUCCESS )
    {
        WINE_ERR("Couldn't query value after successfully querying before (%u),\n"
                "please report to wine-devel@winehq.org\n", res);
        res=FALSE;
        goto end;
    }

    /* Make sure that the data is long enough and ends with two NULLs. This
     * simplifies the code later on.
     */
    if( dataLength<2*sizeof(buffer[0]) ||
            buffer[dataLength/sizeof(buffer[0])-1]!='\0' ||
            buffer[dataLength/sizeof(buffer[0])-2]!='\0' )
    {
        WINE_ERR("Improper value format - doesn't end with NULL\n");
        res=FALSE;
        goto end;
    }

    for( src=buffer; (src-buffer)*sizeof(src[0])<dataLength && *src!='\0';
            src=dst+lstrlenW(dst)+1 )
    {
        DWORD dwFlags=0;

        WINE_TRACE("processing next command\n");

        dst=src+lstrlenW(src)+1;

        /* We need to skip the \??\ header */
        if( src[0]=='\\' && src[1]=='?' && src[2]=='?' && src[3]=='\\' )
            src+=4;

        if( dst[0]=='!' )
        {
            dwFlags|=MOVEFILE_REPLACE_EXISTING;
            dst++;
        }

        if( dst[0]=='\\' && dst[1]=='?' && dst[2]=='?' && dst[3]=='\\' )
            dst+=4;

        if( *dst!='\0' )
        {
            /* Rename the file */
            MoveFileExW( src, dst, dwFlags );
        } else
        {
            /* Delete the file or directory */
            if (!RemoveDirectoryW( src ) && GetLastError() == ERROR_DIRECTORY) DeleteFileW( src );
        }
    }

    if((res=RegDeleteValueW(hSession, ValueName))!=ERROR_SUCCESS )
    {
        WINE_ERR("Error deleting the value (%u)\n", GetLastError() );
        res=FALSE;
    } else
        res=TRUE;
    
end:
    HeapFree(GetProcessHeap(), 0, buffer);

    if( hSession!=NULL )
        RegCloseKey( hSession );

    return res;
}

#define INVALID_RUNCMD_RETURN -1
/*
 * This function runs the specified command in the specified dir.
 * [in,out] cmdline - the command line to run. The function may change the passed buffer.
 * [in] dir - the dir to run the command in. If it is NULL, then the current dir is used.
 * [in] wait - whether to wait for the run program to finish before returning.
 * [in] minimized - Whether to ask the program to run minimized.
 *
 * Returns:
 * If running the process failed, returns INVALID_RUNCMD_RETURN. Use GetLastError to get the error code.
 * If wait is FALSE - returns 0 if successful.
 * If wait is TRUE - returns the program's return value.
 */
static DWORD runCmd(LPWSTR cmdline, LPCWSTR dir, BOOL wait, BOOL minimized)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION info;
    DWORD exit_code=0;

    memset(&si, 0, sizeof(si));
    si.cb=sizeof(si);
    if( minimized )
    {
        si.dwFlags=STARTF_USESHOWWINDOW;
        si.wShowWindow=SW_MINIMIZE;
    }
    memset(&info, 0, sizeof(info));

    if( !CreateProcessW(NULL, cmdline, NULL, NULL, FALSE, 0, NULL, dir, &si, &info) )
    {
        WINE_WARN("Failed to run command %s (%d)\n", wine_dbgstr_w(cmdline), GetLastError() );
        return INVALID_RUNCMD_RETURN;
    }

    WINE_TRACE("Successfully ran command %s - Created process handle %p\n",
               wine_dbgstr_w(cmdline), info.hProcess );

    if(wait)
    {   /* wait for the process to exit */
        WaitForSingleObject(info.hProcess, INFINITE);
        GetExitCodeProcess(info.hProcess, &exit_code);
    }

    CloseHandle( info.hThread );
    CloseHandle( info.hProcess );

    return exit_code;
}

/*
 * Process a "Run" type registry key.
 * hkRoot is the HKEY from which "Software\Microsoft\Windows\CurrentVersion" is
 *      opened.
 * szKeyName is the key holding the actual entries.
 * bDelete tells whether we should delete each value right before executing it.
 * bSynchronous tells whether we should wait for the prog to complete before
 *      going on to the next prog.
 */
static BOOL ProcessRunKeys( HKEY hkRoot, LPCWSTR szKeyName, BOOL bDelete,
        BOOL bSynchronous )
{
    static const WCHAR WINKEY_NAME[]={'S','o','f','t','w','a','r','e','\\',
        'M','i','c','r','o','s','o','f','t','\\','W','i','n','d','o','w','s','\\',
        'C','u','r','r','e','n','t','V','e','r','s','i','o','n',0};
    HKEY hkWin, hkRun;
    DWORD res, dispos;
    DWORD i, nMaxCmdLine=0, nMaxValue=0;
    WCHAR *szCmdLine=NULL;
    WCHAR *szValue=NULL;

    if (hkRoot==HKEY_LOCAL_MACHINE)
        WINE_TRACE("processing %s entries under HKLM\n",wine_dbgstr_w(szKeyName) );
    else
        WINE_TRACE("processing %s entries under HKCU\n",wine_dbgstr_w(szKeyName) );

    if (RegCreateKeyExW( hkRoot, WINKEY_NAME, 0, NULL, 0, KEY_READ, NULL, &hkWin, NULL ) != ERROR_SUCCESS)
        return TRUE;

    if ((res = RegCreateKeyExW( hkWin, szKeyName, 0, NULL, 0, bDelete ? KEY_ALL_ACCESS : KEY_READ,
                                NULL, &hkRun, &dispos )) != ERROR_SUCCESS)
    {
        RegCloseKey( hkWin );
        return TRUE;
    }
    RegCloseKey( hkWin );
    if (dispos == REG_CREATED_NEW_KEY) goto end;

    if( (res=RegQueryInfoKeyW( hkRun, NULL, NULL, NULL, NULL, NULL, NULL, &i, &nMaxValue,
                    &nMaxCmdLine, NULL, NULL ))!=ERROR_SUCCESS )
        goto end;

    if( i==0 )
    {
        WINE_TRACE("No commands to execute.\n");

        res=ERROR_SUCCESS;
        goto end;
    }
    
    if( (szCmdLine=HeapAlloc(GetProcessHeap(),0,nMaxCmdLine))==NULL )
    {
        WINE_ERR("Couldn't allocate memory for the commands to be executed\n");

        res=ERROR_NOT_ENOUGH_MEMORY;
        goto end;
    }

    if( (szValue=HeapAlloc(GetProcessHeap(),0,(++nMaxValue)*sizeof(*szValue)))==NULL )
    {
        WINE_ERR("Couldn't allocate memory for the value names\n");

        res=ERROR_NOT_ENOUGH_MEMORY;
        goto end;
    }
    
    while( i>0 )
    {
        DWORD nValLength=nMaxValue, nDataLength=nMaxCmdLine;
        DWORD type;

        --i;

        if( (res=RegEnumValueW( hkRun, i, szValue, &nValLength, 0, &type,
                        (LPBYTE)szCmdLine, &nDataLength ))!=ERROR_SUCCESS )
        {
            WINE_ERR("Couldn't read in value %d - %d\n", i, res );

            continue;
        }

        if( bDelete && (res=RegDeleteValueW( hkRun, szValue ))!=ERROR_SUCCESS )
        {
            WINE_ERR("Couldn't delete value - %d, %d. Running command anyways.\n", i, res );
        }
        
        if( type!=REG_SZ )
        {
            WINE_ERR("Incorrect type of value #%d (%d)\n", i, type );

            continue;
        }

        if( (res=runCmd(szCmdLine, NULL, bSynchronous, FALSE ))==INVALID_RUNCMD_RETURN )
        {
            WINE_ERR("Error running cmd %s (%d)\n", wine_dbgstr_w(szCmdLine), GetLastError() );
        }

        WINE_TRACE("Done processing cmd #%d\n", i);
    }

    res=ERROR_SUCCESS;

end:
    HeapFree( GetProcessHeap(), 0, szValue );
    HeapFree( GetProcessHeap(), 0, szCmdLine );

    if( hkRun!=NULL )
        RegCloseKey( hkRun );

    WINE_TRACE("done\n");

    return res==ERROR_SUCCESS;
}

/*
 * WFP is Windows File Protection, in NT5 and Windows 2000 it maintains a cache
 * of known good dlls and scans through and replaces corrupted DLLs with these
 * known good versions. The only programs that should install into this dll
 * cache are Windows Updates and IE (which is treated like a Windows Update)
 *
 * Implementing this allows installing ie in win2k mode to actually install the
 * system dlls that we expect and need
 */
static int ProcessWindowsFileProtection(void)
{
    static const WCHAR winlogonW[] = {'S','o','f','t','w','a','r','e','\\',
                                      'M','i','c','r','o','s','o','f','t','\\',
                                      'W','i','n','d','o','w','s',' ','N','T','\\',
                                      'C','u','r','r','e','n','t','V','e','r','s','i','o','n','\\',
                                      'W','i','n','l','o','g','o','n',0};
    static const WCHAR cachedirW[] = {'S','F','C','D','l','l','C','a','c','h','e','D','i','r',0};
    static const WCHAR dllcacheW[] = {'\\','d','l','l','c','a','c','h','e','\\','*',0};
    static const WCHAR wildcardW[] = {'\\','*',0};
    WIN32_FIND_DATAW finddata;
    HANDLE find_handle;
    BOOL find_rc;
    DWORD rc;
    HKEY hkey;
    LPWSTR dllcache = NULL;

    if (!RegOpenKeyW( HKEY_LOCAL_MACHINE, winlogonW, &hkey ))
    {
        DWORD sz = 0;
        if (!RegQueryValueExW( hkey, cachedirW, 0, NULL, NULL, &sz))
        {
            sz += sizeof(WCHAR);
            dllcache = HeapAlloc(GetProcessHeap(),0,sz + sizeof(wildcardW));
            RegQueryValueExW( hkey, cachedirW, 0, NULL, (LPBYTE)dllcache, &sz);
            strcatW( dllcache, wildcardW );
        }
    }
    RegCloseKey(hkey);

    if (!dllcache)
    {
        DWORD sz = GetSystemDirectoryW( NULL, 0 );
        dllcache = HeapAlloc( GetProcessHeap(), 0, sz * sizeof(WCHAR) + sizeof(dllcacheW));
        GetSystemDirectoryW( dllcache, sz );
        strcatW( dllcache, dllcacheW );
    }

    find_handle = FindFirstFileW(dllcache,&finddata);
    dllcache[ strlenW(dllcache) - 2] = 0; /* strip off wildcard */
    find_rc = find_handle != INVALID_HANDLE_VALUE;
    while (find_rc)
    {
        static const WCHAR dotW[] = {'.',0};
        static const WCHAR dotdotW[] = {'.','.',0};
        WCHAR targetpath[MAX_PATH];
        WCHAR currentpath[MAX_PATH];
        UINT sz;
        UINT sz2;
        WCHAR tempfile[MAX_PATH];

        if (strcmpW(finddata.cFileName,dotW) == 0 || strcmpW(finddata.cFileName,dotdotW) == 0)
        {
            find_rc = FindNextFileW(find_handle,&finddata);
            continue;
        }

        sz = MAX_PATH;
        sz2 = MAX_PATH;
        VerFindFileW(VFFF_ISSHAREDFILE, finddata.cFileName, windowsdir,
                     windowsdir, currentpath, &sz, targetpath, &sz2);
        sz = MAX_PATH;
        rc = VerInstallFileW(0, finddata.cFileName, finddata.cFileName,
                             dllcache, targetpath, currentpath, tempfile, &sz);
        if (rc != ERROR_SUCCESS)
        {
            WINE_WARN("WFP: %s error 0x%x\n",wine_dbgstr_w(finddata.cFileName),rc);
            DeleteFileW(tempfile);
        }

        /* now delete the source file so that we don't try to install it over and over again */
        lstrcpynW( targetpath, dllcache, MAX_PATH - 1 );
        sz = strlenW( targetpath );
        targetpath[sz++] = '\\';
        lstrcpynW( targetpath + sz, finddata.cFileName, MAX_PATH - sz );
        if (!DeleteFileW( targetpath ))
            WINE_WARN( "failed to delete %s: error %u\n", wine_dbgstr_w(targetpath), GetLastError() );

        find_rc = FindNextFileW(find_handle,&finddata);
    }
    FindClose(find_handle);
    HeapFree(GetProcessHeap(),0,dllcache);
    return 1;
}

static BOOL start_services_process(void)
{
    static const WCHAR svcctl_started_event[] = SVCCTL_STARTED_EVENT;
    static const WCHAR services[] = {'\\','s','e','r','v','i','c','e','s','.','e','x','e',0};
    PROCESS_INFORMATION pi;
    STARTUPINFOW si;
    HANDLE wait_handles[2];
    WCHAR path[MAX_PATH];

    if (!GetSystemDirectoryW(path, MAX_PATH - strlenW(services)))
        return FALSE;
    strcatW(path, services);
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessW(path, path, NULL, NULL, TRUE, DETACHED_PROCESS, NULL, NULL, &si, &pi))
    {
        WINE_ERR("Couldn't start services.exe: error %u\n", GetLastError());
        return FALSE;
    }
    CloseHandle(pi.hThread);

    wait_handles[0] = CreateEventW(NULL, TRUE, FALSE, svcctl_started_event);
    wait_handles[1] = pi.hProcess;

    /* wait for the event to become available or the process to exit */
    if ((WaitForMultipleObjects(2, wait_handles, FALSE, INFINITE)) == WAIT_OBJECT_0 + 1)
    {
        DWORD exit_code;
        GetExitCodeProcess(pi.hProcess, &exit_code);
        WINE_ERR("Unexpected termination of services.exe - exit code %d\n", exit_code);
        CloseHandle(pi.hProcess);
        CloseHandle(wait_handles[0]);
        return FALSE;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(wait_handles[0]);
    return TRUE;
}

static INT_PTR CALLBACK wait_dlgproc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    switch (msg)
    {
    case WM_INITDIALOG:
        {
            WCHAR *buffer, text[1024];
            const WCHAR *name = (WCHAR *)lp;
            HICON icon = LoadImageW( 0, (LPCWSTR)IDI_WINLOGO, IMAGE_ICON, 48, 48, LR_SHARED );
            SendDlgItemMessageW( hwnd, IDC_WAITICON, STM_SETICON, (WPARAM)icon, 0 );
            SendDlgItemMessageW( hwnd, IDC_WAITTEXT, WM_GETTEXT, 1024, (LPARAM)text );
            buffer = HeapAlloc( GetProcessHeap(), 0, (strlenW(text) + strlenW(name) + 1) * sizeof(WCHAR) );
            sprintfW( buffer, text, name );
            SendDlgItemMessageW( hwnd, IDC_WAITTEXT, WM_SETTEXT, 0, (LPARAM)buffer );
            HeapFree( GetProcessHeap(), 0, buffer );
        }
        break;
    }
    return 0;
}

static HWND show_wait_window(void)
{
    const char *config_dir = wine_get_config_dir();
    WCHAR *name;
    HWND hwnd;
    DWORD len;

    len = MultiByteToWideChar( CP_UNIXCP, 0, config_dir, -1, NULL, 0 );
    name = HeapAlloc( GetProcessHeap(), 0, len * sizeof(WCHAR) );
    MultiByteToWideChar( CP_UNIXCP, 0, config_dir, -1, name, len );
    hwnd = CreateDialogParamW( GetModuleHandleW(0), MAKEINTRESOURCEW(IDD_WAITDLG), 0,
                               wait_dlgproc, (LPARAM)name );
    ShowWindow( hwnd, SW_SHOWNORMAL );
    HeapFree( GetProcessHeap(), 0, name );
    return hwnd;
}

static HANDLE start_rundll32( const char *inf_path, BOOL wow64 )
{
    static const WCHAR rundll[] = {'\\','r','u','n','d','l','l','3','2','.','e','x','e',0};
    static const WCHAR setupapi[] = {' ','s','e','t','u','p','a','p','i',',',
                                     'I','n','s','t','a','l','l','H','i','n','f','S','e','c','t','i','o','n',0};
    static const WCHAR definstall[] = {' ','D','e','f','a','u','l','t','I','n','s','t','a','l','l',0};
    static const WCHAR wowinstall[] = {' ','W','o','w','6','4','I','n','s','t','a','l','l',0};
    static const WCHAR inf[] = {' ','1','2','8',' ','\\','\\','?','\\','u','n','i','x',0 };

    WCHAR app[MAX_PATH + ARRAY_SIZE(rundll)];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    WCHAR *buffer;
    DWORD inf_len, cmd_len;

    memset( &si, 0, sizeof(si) );
    si.cb = sizeof(si);

    if (wow64)
    {
        if (!GetSystemWow64DirectoryW( app, MAX_PATH )) return 0;  /* not on 64-bit */
    }
    else GetSystemDirectoryW( app, MAX_PATH );

    strcatW( app, rundll );

    cmd_len = strlenW(app) * sizeof(WCHAR) + sizeof(setupapi) + sizeof(definstall) + sizeof(inf);
    inf_len = MultiByteToWideChar( CP_UNIXCP, 0, inf_path, -1, NULL, 0 );

    if (!(buffer = HeapAlloc( GetProcessHeap(), 0, cmd_len + inf_len * sizeof(WCHAR) ))) return 0;

    strcpyW( buffer, app );
    strcatW( buffer, setupapi );
    strcatW( buffer, wow64 ? wowinstall : definstall );
    strcatW( buffer, inf );
    MultiByteToWideChar( CP_UNIXCP, 0, inf_path, -1, buffer + strlenW(buffer), inf_len );

    if (CreateProcessW( app, buffer, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi ))
        CloseHandle( pi.hThread );
    else
        pi.hProcess = 0;

    HeapFree( GetProcessHeap(), 0, buffer );
    return pi.hProcess;
}

/* execute rundll32 on the wine.inf file if necessary */
static void update_wineprefix( BOOL force )
{
    const char *config_dir = wine_get_config_dir();
    char *inf_path = get_wine_inf_path();
    int fd;
    struct stat st;

    if (!inf_path)
    {
        WINE_MESSAGE( "wine: failed to update %s, wine.inf not found\n", config_dir );
        return;
    }
    if ((fd = open( inf_path, O_RDONLY )) == -1)
    {
        WINE_MESSAGE( "wine: failed to update %s with %s: %s\n",
                      config_dir, inf_path, strerror(errno) );
        goto done;
    }
    fstat( fd, &st );
    close( fd );

    if (update_timestamp( config_dir, st.st_mtime ) || force)
    {
        HANDLE process;
        DWORD count = 0;

        if ((process = start_rundll32( inf_path, FALSE )))
        {
            HWND hwnd = show_wait_window();
            for (;;)
            {
                MSG msg;
                DWORD res = MsgWaitForMultipleObjects( 1, &process, FALSE, INFINITE, QS_ALLINPUT );
                if (res == WAIT_OBJECT_0)
                {
                    CloseHandle( process );
                    if (count++ || !(process = start_rundll32( inf_path, TRUE ))) break;
                }
                else while (PeekMessageW( &msg, 0, 0, 0, PM_REMOVE )) DispatchMessageW( &msg );
            }
            DestroyWindow( hwnd );
        }
        WINE_MESSAGE( "wine: configuration in '%s' has been updated.\n", config_dir );
    }

done:
    HeapFree( GetProcessHeap(), 0, inf_path );
}

/* Process items in the StartUp group of the user's Programs under the Start Menu. Some installers put
 * shell links here to restart themselves after boot. */
static BOOL ProcessStartupItems(void)
{
    BOOL ret = FALSE;
    HRESULT hr;
    IMalloc *ppM = NULL;
    IShellFolder *psfDesktop = NULL, *psfStartup = NULL;
    LPITEMIDLIST pidlStartup = NULL, pidlItem;
    ULONG NumPIDLs;
    IEnumIDList *iEnumList = NULL;
    STRRET strret;
    WCHAR wszCommand[MAX_PATH];

    WINE_TRACE("Processing items in the StartUp folder.\n");

    hr = SHGetMalloc(&ppM);
    if (FAILED(hr))
    {
	WINE_ERR("Couldn't get IMalloc object.\n");
	goto done;
    }

    hr = SHGetDesktopFolder(&psfDesktop);
    if (FAILED(hr))
    {
	WINE_ERR("Couldn't get desktop folder.\n");
	goto done;
    }

    hr = SHGetSpecialFolderLocation(NULL, CSIDL_STARTUP, &pidlStartup);
    if (FAILED(hr))
    {
	WINE_TRACE("Couldn't get StartUp folder location.\n");
	goto done;
    }

    hr = IShellFolder_BindToObject(psfDesktop, pidlStartup, NULL, &IID_IShellFolder, (LPVOID*)&psfStartup);
    if (FAILED(hr))
    {
	WINE_TRACE("Couldn't bind IShellFolder to StartUp folder.\n");
	goto done;
    }

    hr = IShellFolder_EnumObjects(psfStartup, NULL, SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN, &iEnumList);
    if (FAILED(hr))
    {
	WINE_TRACE("Unable to enumerate StartUp objects.\n");
	goto done;
    }

    while (IEnumIDList_Next(iEnumList, 1, &pidlItem, &NumPIDLs) == S_OK &&
	   (NumPIDLs) == 1)
    {
	hr = IShellFolder_GetDisplayNameOf(psfStartup, pidlItem, SHGDN_FORPARSING, &strret);
	if (FAILED(hr))
	    WINE_TRACE("Unable to get display name of enumeration item.\n");
	else
	{
	    hr = StrRetToBufW(&strret, pidlItem, wszCommand, MAX_PATH);
	    if (FAILED(hr))
		WINE_TRACE("Unable to parse display name.\n");
	    else
            {
                HINSTANCE hinst;

                hinst = ShellExecuteW(NULL, NULL, wszCommand, NULL, NULL, SW_SHOWNORMAL);
                if (PtrToUlong(hinst) <= 32)
                    WINE_WARN("Error %p executing command %s.\n", hinst, wine_dbgstr_w(wszCommand));
            }
	}

	IMalloc_Free(ppM, pidlItem);
    }

    /* Return success */
    ret = TRUE;

done:
    if (iEnumList) IEnumIDList_Release(iEnumList);
    if (psfStartup) IShellFolder_Release(psfStartup);
    if (pidlStartup) IMalloc_Free(ppM, pidlStartup);

    return ret;
}

/* copied from dlls/ntdll/time.c, with the following changes
    - cache is removed
    - can pass specific year
*/
static time_t find_dst_change(unsigned long min, unsigned long max, int *is_dst)
{
    time_t start;
    struct tm *tm;

    start = min;
    tm = localtime(&start);
    *is_dst = !tm->tm_isdst;
    WINE_TRACE("starting date isdst %d, %s", !*is_dst, ctime(&start));

    while (min <= max)
    {
        time_t pos = (min + max) / 2;
        tm = localtime(&pos);

        if (tm->tm_isdst != *is_dst)
            min = pos + 1;
        else
            max = pos - 1;
    }
    return min;
}

static inline void convert_to_non_absolute(RTL_SYSTEM_TIME *st)
{
    /* see dlls/kernel32/time.c TIME_DayLightCompareDate */
    WORD first;

    /* if on 4th week of a 4-week month, there's no way to determine
       if the rule is always for the 4th week, or if for the 'last'
       week (usually 5) and it happens to be the 4th week this year */
    st->wYear = 0;
    first = (6 + st->wDay) % 7 + 1;
    st->wDay = (st->wDay - first) / 7 + 1;
}

static int init_tz_info(RTL_DYNAMIC_TIME_ZONE_INFORMATION *tzi, int year)
{
    struct tm *tm;
    time_t year_start, year_end, tmp, dlt = 0, std = 0;
    int is_dst, current_is_dst;

    year_start = time(NULL);
    tm = localtime(&year_start);
    current_is_dst = tm->tm_isdst;

    memset(tzi, 0, sizeof(*tzi));

    if (year)
        tm->tm_year = year - 1900;
    WINE_TRACE("tz data will be valid through year %d\n", tm->tm_year + 1900);

    tm->tm_isdst = 0;
    tm->tm_mday = 1;
    tm->tm_mon = tm->tm_hour = tm->tm_min = tm->tm_sec = tm->tm_wday = tm->tm_yday = 0;
    year_start = mktime(tm);
    WINE_TRACE("year_start: %s", ctime(&year_start));

    tm->tm_mday = tm->tm_wday = tm->tm_yday = 0;
    tm->tm_mon = 12;
    tm->tm_hour = 23;
    tm->tm_min = tm->tm_sec = 59;
    year_end = mktime(tm);
    WINE_TRACE("year_end: %s", ctime(&year_end));

    tm = gmtime(&year_start);
    tzi->Bias = (LONG)(mktime(tm) - year_start) / 60;

    tmp = find_dst_change(year_start, year_end, &is_dst);
    if (is_dst)
        dlt = tmp;
    else
        std = tmp;

    tmp = find_dst_change(tmp, year_end, &is_dst);
    if (is_dst)
        dlt = tmp;
    else
        std = tmp;

    WINE_TRACE("std: %s", ctime(&std));
    WINE_TRACE("dlt: %s", ctime(&dlt));

    if (dlt == std || !dlt || !std)
        WINE_TRACE("there is no daylight saving rules in this time zone\n");
    else
    {
        tmp = dlt - tzi->Bias * 60;
        tm = gmtime(&tmp);
        WINE_TRACE("dlt gmtime: %s", asctime(tm));

        tzi->DaylightBias = -60;
        tzi->DaylightDate.wYear = tm->tm_year + 1900;
        tzi->DaylightDate.wMonth = tm->tm_mon + 1;
        tzi->DaylightDate.wDayOfWeek = tm->tm_wday;
        tzi->DaylightDate.wDay = tm->tm_mday;
        tzi->DaylightDate.wHour = tm->tm_hour;
        tzi->DaylightDate.wMinute = tm->tm_min;
        tzi->DaylightDate.wSecond = tm->tm_sec;
        tzi->DaylightDate.wMilliseconds = 0;

        WINE_TRACE("daylight (d/m/y): %u/%02u/%04u day of week %u %u:%02u:%02u.%03u bias %d\n",
            tzi->DaylightDate.wDay, tzi->DaylightDate.wMonth,
            tzi->DaylightDate.wYear, tzi->DaylightDate.wDayOfWeek,
            tzi->DaylightDate.wHour, tzi->DaylightDate.wMinute,
            tzi->DaylightDate.wSecond, tzi->DaylightDate.wMilliseconds,
            tzi->DaylightBias);

        tmp = std - tzi->Bias * 60 - tzi->DaylightBias * 60;
        tm = gmtime(&tmp);
        WINE_TRACE("std gmtime: %s", asctime(tm));

        tzi->StandardBias = 0;
        tzi->StandardDate.wYear = tm->tm_year + 1900;
        tzi->StandardDate.wMonth = tm->tm_mon + 1;
        tzi->StandardDate.wDayOfWeek = tm->tm_wday;
        tzi->StandardDate.wDay = tm->tm_mday;
        tzi->StandardDate.wHour = tm->tm_hour;
        tzi->StandardDate.wMinute = tm->tm_min;
        tzi->StandardDate.wSecond = tm->tm_sec;
        tzi->StandardDate.wMilliseconds = 0;

        WINE_TRACE("standard (d/m/y): %u/%02u/%04u day of week %u %u:%02u:%02u.%03u bias %d\n",
            tzi->StandardDate.wDay, tzi->StandardDate.wMonth,
            tzi->StandardDate.wYear, tzi->StandardDate.wDayOfWeek,
            tzi->StandardDate.wHour, tzi->StandardDate.wMinute,
            tzi->StandardDate.wSecond, tzi->StandardDate.wMilliseconds,
            tzi->StandardBias);

        convert_to_non_absolute( &tzi->DaylightDate );
        convert_to_non_absolute( &tzi->StandardDate );
    }

    return current_is_dst;
}

/* this MUST be called after wine.inf has installed the static time zones */

struct tzmap
{
    struct list entry;
    WCHAR *win_tz;
    char *unix_tz;
};

static inline void free_timezones(struct list *tzs)
{
    struct list *head;
    struct tzmap *tzentry;

    while ((head = list_head( tzs )))
    {
        list_remove( head );
        tzentry = LIST_ENTRY( head, struct tzmap, entry );
        heap_free( tzentry );
    }
}

#define TIME_ZONE_KEY_SIZE  (ARRAY_SIZE(((RTL_DYNAMIC_TIME_ZONE_INFORMATION*)0)->TimeZoneKeyName))

static int read_timezones(struct list *tzs)
{
    char buffer[TIME_ZONE_KEY_SIZE];
    const char *config_dir = wine_get_config_dir();
    char *win_unix_map, *unix_tz;
    size_t win_len, unix_len;
    struct tzmap *tzentry;
    FILE *file;

    list_init( tzs );
    if (!(win_unix_map = heap_alloc( strlen(config_dir) + sizeof("/win_unix_map") )))
        return FALSE;

    strcpy( win_unix_map, config_dir );
    strcat( win_unix_map, "/win_unix_map" );
    if (!(file = fopen( win_unix_map, "rt" )))
    {
        WINE_WARN("wine: failed to open %s, time zones will not be updated\n", win_unix_map );
        heap_free( win_unix_map );
        return FALSE;
    }

    while (fgets( buffer, sizeof(buffer), file ))
    {
        if (!(unix_tz = strchr( buffer, ',' )))
        {
            WINE_WARN("wine: invalid time zone map entry %s\n", debugstr_a(buffer));
            continue;
        }

        *unix_tz++ = 0;
        unix_len = strlen( unix_tz );
        if (!unix_len)
        {
            WINE_WARN("wine: invalid time zone map entry %s\n", debugstr_a(buffer));
            continue;
        }
        unix_tz[--unix_len] = 0; /* chop \n */

        win_len = MultiByteToWideChar( CP_UTF8, 0, buffer, -1, NULL, 0 );
        if (!win_len)
        {
            WINE_WARN("wine: invalid time zone map entry %s (%d)\n", debugstr_a(buffer), GetLastError());
            continue;
        }

        tzentry = heap_alloc( sizeof(*tzentry) + unix_len + 1 + 1 + /* extra +1 for ':' */
                              (win_len + 1) * sizeof(WCHAR) );
        if (!tzentry)
        {
            WINE_ERR("wine: out of memory updating time zone %s\n", debugstr_a(buffer));
            goto error;
        }

        tzentry->win_tz = (WCHAR *)((char *)tzentry + sizeof(*tzentry));
        tzentry->unix_tz = ((char *)tzentry->win_tz) + (win_len + 1) * sizeof(WCHAR);

        MultiByteToWideChar( CP_UTF8, 0, buffer, -1, tzentry->win_tz, strlen(buffer)+1 );
        tzentry->unix_tz[0] = ':';
        strcpy( tzentry->unix_tz+1, unix_tz );
        list_add_tail( tzs, &tzentry->entry );
    }
    heap_free( win_unix_map );
    fclose( file );
    return TRUE;

error:
    free_timezones( tzs );
    heap_free( win_unix_map );
    fclose( file );
    return FALSE;
}

static inline struct tzmap *find_timezone(struct list *tzs, const WCHAR *win_tz)
{
    struct tzmap *tzentry;
    LIST_FOR_EACH_ENTRY( tzentry, tzs, struct tzmap, entry )
    {
        if (!lstrcmpW( tzentry->win_tz, win_tz ))
            return tzentry;
    }
    return NULL;
}

static void fill_timezone(HKEY tzkey, struct tzmap *tzentry)
{
    const DWORD dynamic_start = 2004;
    static const WCHAR firstW[] = { 'F','i','r','s','t','E','n','t','r','y',0 };
    static const WCHAR lastW[] = { 'L','a','s','t','E','n','t','r','y',0 };
    static const WCHAR ddstW[] = { 'D','y','n','a','m','i','c',' ','D','S','T',0 };
    static const WCHAR tziW[] = { 'T','Z','I',0 };
    static const WCHAR fmtW[] = { '%','u',0 };
    WCHAR valname[5];
    RTL_DYNAMIC_TIME_ZONE_INFORMATION dtzi, last;
    DWORD ret, last_year, first_year, cur_year;
    struct tm *tm;
    time_t today;
    HKEY dstkey;
    struct tz_reg_data
    {
        LONG bias;
        LONG std_bias;
        LONG dlt_bias;
        RTL_SYSTEM_TIME std_date;
        RTL_SYSTEM_TIME dlt_date;
    } tz_data;

    setenv("TZ", tzentry->unix_tz, TRUE);
    tzset();

    init_tz_info( &dtzi, 0 );

    tz_data.bias = dtzi.Bias;
    tz_data.std_bias = dtzi.StandardBias;
    tz_data.dlt_bias = dtzi.DaylightBias;
    tz_data.std_date = dtzi.StandardDate;
    tz_data.dlt_date = dtzi.DaylightDate;
    RegSetValueExW( tzkey, tziW, 0, REG_BINARY, (const BYTE*)&tz_data, sizeof(tz_data) );

    RegDeleteTreeA( tzkey, "Dynamic DST" );

    today = time( NULL );
    tm = localtime( &today );
    cur_year = tm->tm_year + 1900;

    init_tz_info( &last, dynamic_start );
    for (first_year = dynamic_start + 1; first_year <= cur_year; first_year++)
    {
        init_tz_info( &dtzi, first_year );
        if (memcmp( &dtzi, &last, sizeof(last) ))
        {
            --first_year;
            break;
        }
    }

    if (first_year > cur_year)
    {
        WINE_TRACE("time zone %s has no dynamic dst\n", debugstr_w(tzentry->win_tz));
        return;
    }

    init_tz_info( &last, cur_year );
    for (last_year = cur_year - 1; last_year >= first_year; last_year--)
    {
        init_tz_info( &dtzi, last_year );
        if (memcmp( &dtzi, &last, sizeof(last) ))
        {
            ++last_year;
            break;
        }
    }

    if ((ret = RegCreateKeyExW( tzkey, ddstW, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &dstkey, NULL )))
    {
        WINE_WARN("failed to create '%s\\%s'\n", debugstr_w(tzentry->win_tz), debugstr_w(ddstW));
        return;
    }

    RegSetValueExW( dstkey, firstW, 0, REG_DWORD, (const BYTE *)&first_year, sizeof(first_year) );
    RegSetValueExW( dstkey, lastW, 0, REG_DWORD, (const BYTE *)&last_year, sizeof(last_year) );

    while (first_year <= last_year)
    {
        init_tz_info( &dtzi, first_year );

        snprintfW( valname, sizeof(valname), fmtW, first_year );
        tz_data.bias = dtzi.Bias;
        tz_data.std_bias = dtzi.StandardBias;
        tz_data.dlt_bias = dtzi.DaylightBias;
        tz_data.std_date = dtzi.StandardDate;
        tz_data.dlt_date = dtzi.DaylightDate;
        RegSetValueExW( dstkey, valname, 0, REG_BINARY, (const BYTE*)&tz_data, sizeof(tz_data) );

        first_year++;
    }

    RegCloseKey( dstkey );
}

static void update_timezones_from_tzdata(void)
{
    static const WCHAR timezonesW[] = {
        'S','o','f','t','w','a','r','e','\\',
        'M','i','c','r','o','s','o','f','t','\\',
        'W','i','n','d','o','w','s',' ','N','T','\\',
        'C','u','r','r','e','n','t','V','e','r','s','i','o','n','\\',
        'T','i','m','e',' ','Z','o','n','e','s',0 };
    RTL_DYNAMIC_TIME_ZONE_INFORMATION dtzi;
    WCHAR keyname[TIME_ZONE_KEY_SIZE];
    struct list tzs, *head;
    struct tzmap *tzentry;
    HKEY hkey, subkey;
    DWORD index, ret;
    char *orig_tz;

    if (!read_timezones( &tzs ))
        return;

    orig_tz = getenv( "TZ" );
    if ((ret = RegOpenKeyExW( HKEY_LOCAL_MACHINE, timezonesW, 0, KEY_ENUMERATE_SUB_KEYS, &hkey )))
        goto error;

    index = 0;
    while (!(ret = RegEnumKeyW( hkey, index++, keyname, sizeof(keyname) )))
    {
        if (!(tzentry = find_timezone( &tzs, keyname )))
        {
            const DWORD obsolete = 1;

            WINE_TRACE("marking time zone %s obsolete\n", debugstr_w(keyname));
            if (RegOpenKeyExW( hkey, keyname, 0, KEY_SET_VALUE, &subkey ))
            {
                WINE_WARN("failed to open time zone %s to mark obsolete\n", debugstr_w(keyname));
                continue;
            }

            RegSetValueExA( subkey, "IsObsolete", 0, REG_DWORD, (const BYTE *)&obsolete, sizeof(obsolete) );
            RegCloseKey( subkey );
        }
        else
        {
            WINE_TRACE("updating time zone %s\n", debugstr_w(keyname));
            list_remove( &tzentry->entry );
            if (!RegOpenKeyExW( hkey, keyname, 0, KEY_SET_VALUE, &subkey ))
            {
                fill_timezone( subkey, tzentry );
                RegCloseKey( subkey );
            }
            heap_free( tzentry );
        }
    }

    while ((head = list_head( &tzs )))
    {
        static const WCHAR stdW[] = { 'S','t','d',0 };
        static const WCHAR dltW[] = { 'D','l','t',0 };
        static const WCHAR stdnameW[] = { 'S','t','a','n','d','a','r','d',0 };
        static const WCHAR dltnameW[] = { 'D','a','y','l','i','g','h','t' };
        WCHAR *stdstr;

        list_remove( head );
        tzentry = LIST_ENTRY( head, struct tzmap, entry );
        WINE_TRACE("adding new time zone %s\n", debugstr_w(tzentry->win_tz));

        if ((ret = RegCreateKeyExW(hkey, tzentry->win_tz, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &subkey, NULL)))
        {
            WINE_WARN("failed to add new time zone %s\n", debugstr_w(tzentry->win_tz));
            heap_free( tzentry );
            continue;
        }

        lstrcpynW( dtzi.StandardName, tzentry->win_tz, sizeof(dtzi.StandardName) );
        RegSetValueExW( subkey, stdW, 0, REG_SZ, (const BYTE *)dtzi.StandardName, sizeof(dtzi.StandardName) );

        lstrcpynW( dtzi.DaylightName, tzentry->win_tz, sizeof(dtzi.DaylightName) );
        if ((stdstr = strstrW( dtzi.DaylightName, stdnameW )))
            memcpy( stdstr, dltnameW, sizeof(dltnameW) );
        RegSetValueExW( subkey, dltW, 0, REG_SZ, (const BYTE *)dtzi.DaylightName, sizeof(dtzi.DaylightName) );

        fill_timezone( subkey, tzentry );
        RegCloseKey( subkey );
        heap_free( tzentry );
    }

    RegCloseKey( hkey );

error:
    if (orig_tz)
        setenv( "TZ", orig_tz, TRUE );
    else
        unsetenv( "TZ" );
    tzset();
    free_timezones( &tzs );
}

static void usage(void)
{
    WINE_MESSAGE( "Usage: wineboot [options]\n" );
    WINE_MESSAGE( "Options;\n" );
    WINE_MESSAGE( "    -h,--help         Display this help message\n" );
    WINE_MESSAGE( "    -e,--end-session  End the current session cleanly\n" );
    WINE_MESSAGE( "    -f,--force        Force exit for processes that don't exit cleanly\n" );
    WINE_MESSAGE( "    -i,--init         Perform initialization for first Wine instance\n" );
    WINE_MESSAGE( "    -k,--kill         Kill running processes without any cleanup\n" );
    WINE_MESSAGE( "    -r,--restart      Restart only, don't do normal startup operations\n" );
    WINE_MESSAGE( "    -s,--shutdown     Shutdown only, don't reboot\n" );
    WINE_MESSAGE( "    -u,--update       Update the wineprefix directory\n" );
}

static const char short_options[] = "efhikrsu";

static const struct option long_options[] =
{
    { "help",        0, 0, 'h' },
    { "end-session", 0, 0, 'e' },
    { "force",       0, 0, 'f' },
    { "init" ,       0, 0, 'i' },
    { "kill",        0, 0, 'k' },
    { "restart",     0, 0, 'r' },
    { "shutdown",    0, 0, 's' },
    { "update",      0, 0, 'u' },
    { NULL,          0, 0, 0 }
};

int main( int argc, char *argv[] )
{
    static const WCHAR RunW[] = {'R','u','n',0};
    static const WCHAR RunOnceW[] = {'R','u','n','O','n','c','e',0};
    static const WCHAR RunServicesW[] = {'R','u','n','S','e','r','v','i','c','e','s',0};
    static const WCHAR RunServicesOnceW[] = {'R','u','n','S','e','r','v','i','c','e','s','O','n','c','e',0};
    static const WCHAR wineboot_eventW[] = {'_','_','w','i','n','e','b','o','o','t','_','e','v','e','n','t',0};

    /* First, set the current directory to SystemRoot */
    int optc;
    BOOL end_session, force, init, kill, restart, shutdown, update;
    HANDLE event;
    SECURITY_ATTRIBUTES sa;
    BOOL is_wow64;

    end_session = force = init = kill = restart = shutdown = update = FALSE;
    GetWindowsDirectoryW( windowsdir, MAX_PATH );
    if( !SetCurrentDirectoryW( windowsdir ) )
        WINE_ERR("Cannot set the dir to %s (%d)\n", wine_dbgstr_w(windowsdir), GetLastError() );

    if (IsWow64Process( GetCurrentProcess(), &is_wow64 ) && is_wow64)
    {
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        WCHAR filename[MAX_PATH];
        void *redir;
        DWORD exit_code;

        memset( &si, 0, sizeof(si) );
        si.cb = sizeof(si);
        GetModuleFileNameW( 0, filename, MAX_PATH );

        Wow64DisableWow64FsRedirection( &redir );
        if (CreateProcessW( filename, GetCommandLineW(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi ))
        {
            WINE_TRACE( "restarting %s\n", wine_dbgstr_w(filename) );
            WaitForSingleObject( pi.hProcess, INFINITE );
            GetExitCodeProcess( pi.hProcess, &exit_code );
            ExitProcess( exit_code );
        }
        else WINE_ERR( "failed to restart 64-bit %s, err %d\n", wine_dbgstr_w(filename), GetLastError() );
        Wow64RevertWow64FsRedirection( redir );
    }

    while ((optc = getopt_long(argc, argv, short_options, long_options, NULL )) != -1)
    {
        switch(optc)
        {
        case 'e': end_session = TRUE; break;
        case 'f': force = TRUE; break;
        case 'i': init = TRUE; break;
        case 'k': kill = TRUE; break;
        case 'r': restart = TRUE; break;
        case 's': shutdown = TRUE; break;
        case 'u': update = TRUE; break;
        case 'h': usage(); return 0;
        case '?': usage(); return 1;
        }
    }

    if (end_session)
    {
        if (kill)
        {
            if (!shutdown_all_desktops( force )) return 1;
        }
        else if (!shutdown_close_windows( force )) return 1;
    }

    if (kill) kill_processes( shutdown );

    if (shutdown) return 0;

    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;  /* so that services.exe inherits it */
    event = CreateEventW( &sa, TRUE, FALSE, wineboot_eventW );

    ResetEvent( event );  /* in case this is a restart */

    create_hardware_registry_keys();
    create_dynamic_registry_keys();
    create_environment_registry_keys();
    wininit();
    pendingRename();

    ProcessWindowsFileProtection();
    ProcessRunKeys( HKEY_LOCAL_MACHINE, RunServicesOnceW, TRUE, FALSE );

    if (init || (kill && !restart))
    {
        ProcessRunKeys( HKEY_LOCAL_MACHINE, RunServicesW, FALSE, FALSE );
        start_services_process();
    }
    if (init || update) update_wineprefix( update );

    create_volatile_environment_registry_key();

    ProcessRunKeys( HKEY_LOCAL_MACHINE, RunOnceW, TRUE, TRUE );

    if (!init && !restart)
    {
        ProcessRunKeys( HKEY_LOCAL_MACHINE, RunW, FALSE, FALSE );
        ProcessRunKeys( HKEY_CURRENT_USER, RunW, FALSE, FALSE );
        ProcessStartupItems();
    }

    if (!restart)
        update_timezones_from_tzdata();

    WINE_TRACE("Operation done\n");

    SetEvent( event );
    return 0;
}
