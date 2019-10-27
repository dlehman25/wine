/*
 * Shared Memory functions
 *
 * Copyright 2019 Daniel Lehman
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

#include "config.h"
#include "wine/port.h"

#include "ntstatus.h"
#define WIN32_NO_STATUS
#define NONAMELESSUNION
#include "ntdll_misc.h"

#include "wine/debug.h"
#include "wine/shmlib.h"

void test_shmlib(void)
{
    MESSAGE("%s: pid 0x%x unix %u\n", __FUNCTION__, GetCurrentProcessId(), getpid());
}
