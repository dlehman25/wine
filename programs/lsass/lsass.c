/*
 * Copyright 2004 Juan Lang
 * Copyright 2007 Kai Blin
 * Copyright 2017, 2018 Dmitry Timoshkov
 * Copyright 2026 Piotr Caban
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

#include <stdarg.h>
#include <stdlib.h>

#include "wtypes.h"
#include "sspi.h"
#include "ntstatus.h"
#include "lsass_private.h"
#include "lsass.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(secur32);

struct package
{
    WCHAR module[MAX_PATH];
    ULONG table_no;
    SecPkgInfoW info;
    SECPKG_FUNCTION_TABLE *funcs;
};

static struct package *packages;
static ULONG packages_count, packages_size;

static const char *debugstr_as( const LSA_STRING *str )
{
    if (!str) return "<null>";
    return debugstr_an( str->Buffer, str->Length );
}

static NTSTATUS NTAPI lsa_CreateLogonSession( LUID *logon_id )
{
    FIXME( "%p: stub\n", logon_id );
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS NTAPI lsa_DeleteLogonSession( LUID *logon_id )
{
    FIXME( "%p: stub\n", logon_id );
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS NTAPI lsa_AddCredential( LUID *logon_id, ULONG package_id,
        LSA_STRING *primary_key, LSA_STRING *credentials )
{
    FIXME( "%p,%lu,%s,%s: stub\n", logon_id, package_id,
            debugstr_as(primary_key), debugstr_as(credentials) );
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS NTAPI lsa_GetCredentials( LUID *logon_id, ULONG package_id,
        ULONG *context, BOOLEAN retrieve_all, LSA_STRING *primary_key,
        ULONG *primary_key_len, LSA_STRING *credentials )
{
    FIXME( "%p,%#lx,%p,%d,%p,%p,%p: stub\n", logon_id, package_id, context,
            retrieve_all, primary_key, primary_key_len, credentials );
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS NTAPI lsa_DeleteCredential( LUID *logon_id,
        ULONG package_id, LSA_STRING *primary_key )
{
    FIXME( "%p,%#lx,%s: stub\n", logon_id, package_id, debugstr_as(primary_key) );
    return STATUS_NOT_IMPLEMENTED;
}

static void * NTAPI lsa_AllocateLsaHeap( ULONG size )
{
    TRACE( "%lu\n", size );
    return malloc( size );
}

static void NTAPI lsa_FreeLsaHeap( void *p )
{
    TRACE( "%p\n", p );
    free( p );
}

static NTSTATUS NTAPI lsa_AllocateClientBuffer( PLSA_CLIENT_REQUEST req, ULONG size, void **p )
{
    TRACE( "%p,%lu,%p\n", req, size, p );
    *p = malloc( size );
    return *p ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}

static NTSTATUS NTAPI lsa_FreeClientBuffer( PLSA_CLIENT_REQUEST req, void *p )
{
    TRACE( "%p,%p\n", req, p );
    free( p );
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI lsa_CopyToClientBuffer( PLSA_CLIENT_REQUEST req, ULONG size, void *client, void *buf )
{
    TRACE( "%p,%lu,%p,%p\n", req, size, client, buf );
    memcpy( client, buf, size );
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI lsa_CopyFromClientBuffer( PLSA_CLIENT_REQUEST req, ULONG size, void *buf, void *client )
{
    TRACE( "%p,%lu,%p,%p\n", req, size, buf, client );
    memcpy( buf, client, size );
    return STATUS_SUCCESS;
}

static LSA_DISPATCH_TABLE lsa_dispatch =
{
    lsa_CreateLogonSession,
    lsa_DeleteLogonSession,
    lsa_AddCredential,
    lsa_GetCredentials,
    lsa_DeleteCredential,
    lsa_AllocateLsaHeap,
    lsa_FreeLsaHeap,
    lsa_AllocateClientBuffer,
    lsa_FreeClientBuffer,
    lsa_CopyToClientBuffer,
    lsa_CopyFromClientBuffer
};

static BOOL init_package( const WCHAR *module, SpLsaModeInitializeFn init )
{
    SECPKG_FUNCTION_TABLE *tables;
    ULONG api_version, count, i;
    SecPkgInfoW info;
    LSA_STRING *name;
    BOOL ret = FALSE;

    if (!init) return FALSE;
    if (init( SECPKG_INTERFACE_VERSION, &api_version, &tables, &count ))
        return FALSE;

    if (!packages_size)
    {
        packages = malloc( sizeof(*packages) * max(count, 8) );
        if (!packages) return FALSE;
        packages_size = max( count, 8 );
    }
    else if (packages_count + count > packages_size)
    {
        struct packages *new_packages;

        new_packages = realloc( packages, sizeof(*packages) *
                max(packages_size * 2, packages_count + count) );
        if (!new_packages) return FALSE;
        packages_size = max( packages_size * 2, packages_count + count );
    }

    for (i = 0; i < count; i++)
    {
        if (tables[i].InitializePackage( packages_count, &lsa_dispatch, NULL, NULL, &name ))
            continue;

        TRACE( "name %s, version %#lx, api table %p\n", debugstr_as(name), api_version, &tables[i] );
        lsa_FreeLsaHeap( name );

        if (tables[i].Initialize( packages_count, NULL /* FIXME: params */, NULL ))
            continue;
        if (tables[i].GetInfo( &info )) continue;

        wcscpy( packages[packages_count].module, module );
        packages[packages_count].table_no = i;
        packages[packages_count].funcs = tables + i;
        packages[packages_count].info = info;
        packages_count++;
        ret = TRUE;
    }

    return ret;
}

void load_auth_packages( void )
{
    DWORD err, i;
    HKEY root;

    /* "Negotiate" has package id 0, .Net depends on this. */
    init_package( L"", nego_SpLsaModeInitialize );

    err = RegOpenKeyExW( HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Control\\Lsa", 0, KEY_READ, &root );
    if (err != ERROR_SUCCESS) return;

    for (i = 0;; i++)
    {
        WCHAR name[MAX_PATH];
        SpLsaModeInitializeFn init;
        HMODULE hmod;

        err = RegEnumKeyW( root, i, name, MAX_PATH );
        if (err == ERROR_NO_MORE_ITEMS) break;
        if (err != ERROR_SUCCESS) continue;

        hmod = LoadLibraryW( name );
        if (!hmod) continue;
        init = (void *)GetProcAddress( hmod, "SpLsaModeInitialize" );
        if (!init || !init_package( name, init ))
            FreeLibrary( hmod );
    }

    RegCloseKey( root );
}

NTSTATUS __cdecl get_packages( handle_t binding, ULONG *count, package_info **out )
{
    ULONG i;

    *count = 0;
    *out = MIDL_user_allocate( sizeof(*(*out)) * packages_count );
    if (!*out) return SEC_E_INSUFFICIENT_MEMORY;

    *count = packages_count;
    for (i = 0; i < packages_count; i++)
    {
        (*out)[i].module_name = packages[i].module[0] ? packages[i].module : NULL;
        (*out)[i].table_no = packages[i].table_no;
        (*out)[i].info = packages[i].info;
    }
    return SEC_E_OK;
}
