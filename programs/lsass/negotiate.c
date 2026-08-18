/*
 * Copyright 2005 Kai Blin
 * Copyright 2012 Hans Leidekker for CodeWeavers
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

#include "wtypes.h"
#include "winternl.h"
#include "sspi.h"
#include "ntsecapi.h"
#include "ntsecpkg.h"
#include "ntstatus.h"
#include "rpc.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(secur32);

#define NEGO_MAX_TOKEN 48256

static WCHAR nego_name_W[] = L"Negotiate";
static char nego_name_A[] = "Negotiate";
static WCHAR negotiate_comment_W[] = L"Microsoft Package Negotiator";

#define CAPS ( \
    SECPKG_FLAG_INTEGRITY  | \
    SECPKG_FLAG_PRIVACY    | \
    SECPKG_FLAG_CONNECTION | \
    SECPKG_FLAG_MULTI_REQUIRED | \
    SECPKG_FLAG_EXTENDED_ERROR | \
    SECPKG_FLAG_IMPERSONATION  | \
    SECPKG_FLAG_ACCEPT_WIN32_NAME | \
    SECPKG_FLAG_NEGOTIABLE        | \
    SECPKG_FLAG_GSS_COMPATIBLE    | \
    SECPKG_FLAG_LOGON             | \
    SECPKG_FLAG_RESTRICTED_TOKENS )

static NTSTATUS NTAPI nego_LsaApInitializePackage( ULONG package_id, PLSA_DISPATCH_TABLE dispatch,
    PLSA_STRING database, PLSA_STRING confidentiality, PLSA_STRING *package_name )
{
    char *name;

    name = dispatch->AllocateLsaHeap( sizeof(nego_name_A) );
    if (!name) return STATUS_NO_MEMORY;

    memcpy(name, nego_name_A, sizeof(nego_name_A));

    *package_name = dispatch->AllocateLsaHeap( sizeof(**package_name) );
    if (!*package_name)
    {
        dispatch->FreeLsaHeap( name );
        return STATUS_NO_MEMORY;
    }

    RtlInitString( *package_name, name );

    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI nego_SpInitialize( ULONG_PTR package_id, SECPKG_PARAMETERS *params,
    LSA_SECPKG_FUNCTION_TABLE *lsa_function_table )
{
    TRACE( "%Iu, %p, %p\n", package_id, params, lsa_function_table );
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI nego_SpGetInfo( SecPkgInfoW *info )
{
    const SecPkgInfoW infoW = {CAPS, 1, RPC_C_AUTHN_GSS_NEGOTIATE, NEGO_MAX_TOKEN,
                               nego_name_W, negotiate_comment_W};

    TRACE( "%p\n", info );

    /* LSA will make a copy before forwarding the structure, so
     * it's safe to put pointers to dynamic or constant data there.
     */
    *info = infoW;
    return STATUS_SUCCESS;
}

static SECPKG_FUNCTION_TABLE nego_lsa_table =
{
    nego_LsaApInitializePackage,
    NULL, /* LsaLogonUser */
    NULL, /* CallPackage */
    NULL, /* LogonTerminated */
    NULL, /* LsaApCallPackageUntrusted */
    NULL, /* CallPackagePassthrough */
    NULL, /* LogonUserEx */
    NULL, /* LogonUserEx2 */
    nego_SpInitialize,
    NULL, /* SpShutdown */
    nego_SpGetInfo,
    NULL, /* AcceptCredentials */
    NULL, /* SpAcquireCredentialsHandle */
    NULL, /* SpQueryCredentialsAttributes */
    NULL, /* SpFreeCredentialsHandle */
    NULL, /* SaveCredentials */
    NULL, /* GetCredentials */
    NULL, /* DeleteCredentials */
    NULL, /* SpInitLsaModeContext */
    NULL, /* SpAcceptLsaModeContext */
    NULL, /* SpDeleteContext */
    NULL, /* ApplyControlToken */
    NULL, /* GetUserInfo */
    NULL, /* GetExtendedInformation */
    NULL, /* SpQueryContextAttributes */
    NULL, /* SpAddCredentials */
    NULL, /* SetExtendedInformation */
    NULL, /* SetContextAttributes */
    NULL, /* SetCredentialsAttributes */
    NULL, /* ChangeAccountPassword */
    NULL, /* QueryMetaData */
    NULL, /* ExchangeMetaData */
    NULL, /* GetCredUIContext */
    NULL, /* UpdateCredentials */
    NULL, /* ValidateTargetInfo */
    NULL, /* PostLogonUser */
};

NTSTATUS NTAPI nego_SpLsaModeInitialize(ULONG lsa_version, PULONG package_version,
    PSECPKG_FUNCTION_TABLE *table, PULONG table_count)
{
    TRACE("%#lx, %p, %p, %p\n", lsa_version, package_version, table, table_count);

    *package_version = SECPKG_INTERFACE_VERSION;
    *table = &nego_lsa_table;
    *table_count = 1;
    return STATUS_SUCCESS;
}
