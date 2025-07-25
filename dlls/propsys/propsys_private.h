/*
 * propsys private definitions
 *
 * Copyright 2012 Vincent Povirk for CodeWeavers
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

HRESULT PropertyStore_CreateInstance(IUnknown *outer, REFIID riid, void **ppv);

static inline const char *debugstr_propkey(const PROPERTYKEY *key)
{
    if (!key)
        return "(null)";
    return wine_dbg_sprintf("{%s,%04lx}", wine_dbgstr_guid(&key->fmtid), key->pid);
}
