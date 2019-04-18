/*
 * NTDLL directory functions
 *
 * Copyright 1993 Erik Bos
 * Copyright 2003 Eric Pouech
 * Copyright 1996, 2004 Alexandre Julliard
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

#include <assert.h>
#include <sys/types.h>
#ifdef HAVE_DIRENT_H
# include <dirent.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#ifdef HAVE_MNTENT_H
#include <mntent.h>
#endif
#ifdef HAVE_SYS_STAT_H
# include <sys/stat.h>
#endif
#ifdef HAVE_SYS_SYSCALL_H
# include <sys/syscall.h>
#endif
#ifdef HAVE_SYS_ATTR_H
#include <sys/attr.h>
#endif
#ifdef MAJOR_IN_MKDEV
# include <sys/mkdev.h>
#elif defined(MAJOR_IN_SYSMACROS)
# include <sys/sysmacros.h>
#endif
#ifdef HAVE_SYS_VNODE_H
# ifdef HAVE_STDINT_H
# include <stdint.h>  /* needed for kfreebsd */
# endif
/* Work around a conflict with Solaris' system list defined in sys/list.h. */
#define list SYSLIST
#define list_next SYSLIST_NEXT
#define list_prev SYSLIST_PREV
#define list_head SYSLIST_HEAD
#define list_tail SYSLIST_TAIL
#define list_move_tail SYSLIST_MOVE_TAIL
#define list_remove SYSLIST_REMOVE
#include <sys/vnode.h>
#undef list
#undef list_next
#undef list_prev
#undef list_head
#undef list_tail
#undef list_move_tail
#undef list_remove
#endif
#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif
#ifdef HAVE_LINUX_IOCTL_H
#include <linux/ioctl.h>
#endif
#ifdef HAVE_LINUX_MAJOR_H
# include <linux/major.h>
#endif
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#ifdef HAVE_SYS_MOUNT_H
#include <sys/mount.h>
#endif
#ifdef HAVE_SYS_STATFS_H
#include <sys/statfs.h>
#endif
#include <time.h>
#ifdef HAVE_SYS_TIME_H
# include <sys/time.h>
#endif
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#define NONAMELESSUNION
#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#include "ddk/wdm.h"
#include "ntdll_misc.h"
#include "wine/unicode.h"
#include "wine/server.h"
#include "wine/list.h"
#include "wine/library.h"
#include "wine/debug.h"
#include "wine/exception.h"
#include "pshpack4.h"
#include "wine/rbtree.h"
#include "poppack.h"

WINE_DEFAULT_DEBUG_CHANNEL(file);

/* just in case... */
#undef VFAT_IOCTL_READDIR_BOTH

#ifdef linux

/* We want the real kernel dirent structure, not the libc one */
typedef struct
{
    long d_ino;
    long d_off;
    unsigned short d_reclen;
    char d_name[256];
} KERNEL_DIRENT;

/* Define the VFAT ioctl to get both short and long file names */
#define VFAT_IOCTL_READDIR_BOTH  _IOR('r', 1, KERNEL_DIRENT [2] )

#ifndef O_DIRECTORY
# define O_DIRECTORY 0200000 /* must be directory */
#endif

#endif  /* linux */

#define IS_OPTION_TRUE(ch) ((ch) == 'y' || (ch) == 'Y' || (ch) == 't' || (ch) == 'T' || (ch) == '1')
#define IS_SEPARATOR(ch)   ((ch) == '\\' || (ch) == '/')

#define INVALID_NT_CHARS   '*','?','<','>','|','"'
#define INVALID_DOS_CHARS  INVALID_NT_CHARS,'+','=',',',';','[',']',' ','\345'

#define MAX_DIR_ENTRY_LEN 255  /* max length of a directory entry in chars */

#define MAX_IGNORED_FILES 4

struct file_identity
{
    dev_t dev;
    ino_t ino;
};

static struct file_identity ignored_files[MAX_IGNORED_FILES];
static unsigned int ignored_files_count;

union file_directory_info
{
    ULONG                              next;
    FILE_DIRECTORY_INFORMATION         dir;
    FILE_BOTH_DIRECTORY_INFORMATION    both;
    FILE_FULL_DIRECTORY_INFORMATION    full;
    FILE_ID_BOTH_DIRECTORY_INFORMATION id_both;
    FILE_ID_FULL_DIRECTORY_INFORMATION id_full;
    FILE_ID_GLOBAL_TX_DIR_INFORMATION  id_tx;
    FILE_NAMES_INFORMATION             names;
};

struct dir_data_buffer
{
    struct dir_data_buffer *next;    /* next buffer in the list */
    unsigned int            size;    /* total size of the buffer */
    unsigned int            pos;     /* current position in the buffer */
    char                    data[1];
};

struct dir_data_names
{
    const WCHAR *long_name;          /* long file name in Unicode */
    const WCHAR *short_name;         /* short file name in Unicode */
    const char  *unix_name;          /* Unix file name in host encoding */
};

struct dir_data
{
    unsigned int            size;    /* size of the names array */
    unsigned int            count;   /* count of used entries in the names array */
    unsigned int            pos;     /* current reading position in the names array */
    struct file_identity    id;      /* directory file identity */
    struct dir_data_names  *names;   /* directory file names */
    struct dir_data_buffer *buffer;  /* head of data buffers list */
};

static const unsigned int dir_data_buffer_initial_size = 4096;
static const unsigned int dir_data_cache_initial_size  = 256;
static const unsigned int dir_data_names_initial_size  = 64;

static struct dir_data **dir_data_cache;
static unsigned int dir_data_cache_size;

static BOOL show_dot_files;
static RTL_RUN_ONCE init_once = RTL_RUN_ONCE_INIT;

/* at some point we may want to allow Winelib apps to set this */
static const BOOL is_case_sensitive = FALSE;

static struct file_identity windir;

static RTL_CRITICAL_SECTION dir_section;
static RTL_CRITICAL_SECTION_DEBUG critsect_debug =
{
    0, 0, &dir_section,
    { &critsect_debug.ProcessLocksList, &critsect_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": dir_section") }
};
static RTL_CRITICAL_SECTION dir_section = { &critsect_debug, -1, 0, 0, 0, 0 };

/* dfcache forward declarations */
BOOL dcache_enabled;
static int dc_lookup_unix_name(const WCHAR *, int, char **, int, UINT, NTSTATUS *);
static int dc_find_file_in_dir(char *, int, const WCHAR *, int, NTSTATUS *);

/* check if a given Unicode char is OK in a DOS short name */
static inline BOOL is_invalid_dos_char( WCHAR ch )
{
    static const WCHAR invalid_chars[] = { INVALID_DOS_CHARS,'~','.',0 };
    if (ch > 0x7f) return TRUE;
    return strchrW( invalid_chars, ch ) != NULL;
}

/* check if the device can be a mounted volume */
static inline BOOL is_valid_mounted_device( const struct stat *st )
{
#if defined(linux) || defined(__sun__)
    return S_ISBLK( st->st_mode );
#else
    /* disks are char devices on *BSD */
    return S_ISCHR( st->st_mode );
#endif
}

static inline void ignore_file( const char *name )
{
    struct stat st;
    assert( ignored_files_count < MAX_IGNORED_FILES );
    if (!stat( name, &st ))
    {
        ignored_files[ignored_files_count].dev = st.st_dev;
        ignored_files[ignored_files_count].ino = st.st_ino;
        ignored_files_count++;
    }
}

static inline BOOL is_same_file( const struct file_identity *file, const struct stat *st )
{
    return st->st_dev == file->dev && st->st_ino == file->ino;
}

static inline BOOL is_ignored_file( const struct stat *st )
{
    unsigned int i;

    for (i = 0; i < ignored_files_count; i++)
        if (is_same_file( &ignored_files[i], st )) return TRUE;
    return FALSE;
}

static inline unsigned int dir_info_align( unsigned int len )
{
    return (len + 7) & ~7;
}

static inline unsigned int dir_info_size( FILE_INFORMATION_CLASS class, unsigned int len )
{
    switch (class)
    {
    case FileDirectoryInformation:
        return offsetof( FILE_DIRECTORY_INFORMATION, FileName[len] );
    case FileBothDirectoryInformation:
        return offsetof( FILE_BOTH_DIRECTORY_INFORMATION, FileName[len] );
    case FileFullDirectoryInformation:
        return offsetof( FILE_FULL_DIRECTORY_INFORMATION, FileName[len] );
    case FileIdBothDirectoryInformation:
        return offsetof( FILE_ID_BOTH_DIRECTORY_INFORMATION, FileName[len] );
    case FileIdFullDirectoryInformation:
        return offsetof( FILE_ID_FULL_DIRECTORY_INFORMATION, FileName[len] );
    case FileIdGlobalTxDirectoryInformation:
        return offsetof( FILE_ID_GLOBAL_TX_DIR_INFORMATION, FileName[len] );
    case FileNamesInformation:
        return offsetof( FILE_NAMES_INFORMATION, FileName[len] );
    default:
        assert(0);
        return 0;
    }
}

static inline BOOL has_wildcard( const UNICODE_STRING *mask )
{
    return (!mask ||
            memchrW( mask->Buffer, '*', mask->Length / sizeof(WCHAR) ) ||
            memchrW( mask->Buffer, '?', mask->Length / sizeof(WCHAR) ));
}

/* get space from the current directory data buffer, allocating a new one if necessary */
static void *get_dir_data_space( struct dir_data *data, unsigned int size )
{
    struct dir_data_buffer *buffer = data->buffer;
    void *ret;

    if (!buffer || size > buffer->size - buffer->pos)
    {
        unsigned int new_size = buffer ? buffer->size * 2 : dir_data_buffer_initial_size;
        if (new_size < size) new_size = size;
        if (!(buffer = RtlAllocateHeap( GetProcessHeap(), 0,
                                        offsetof( struct dir_data_buffer, data[new_size] ) ))) return NULL;
        buffer->pos  = 0;
        buffer->size = new_size;
        buffer->next = data->buffer;
        data->buffer = buffer;
    }
    ret = buffer->data + buffer->pos;
    buffer->pos += size;
    return ret;
}

/* add a string to the directory data buffer */
static const char *add_dir_data_nameA( struct dir_data *data, const char *name )
{
    /* keep buffer data WCHAR-aligned */
    char *ptr = get_dir_data_space( data, (strlen( name ) + sizeof(WCHAR)) & ~(sizeof(WCHAR) - 1) );
    if (ptr) strcpy( ptr, name );
    return ptr;
}

/* add a Unicode string to the directory data buffer */
static const WCHAR *add_dir_data_nameW( struct dir_data *data, const WCHAR *name )
{
    WCHAR *ptr = get_dir_data_space( data, (strlenW( name ) + 1) * sizeof(WCHAR) );
    if (ptr) strcpyW( ptr, name );
    return ptr;
}

/* add an entry to the directory names array */
static BOOL add_dir_data_names( struct dir_data *data, const WCHAR *long_name,
                                const WCHAR *short_name, const char *unix_name )
{
    static const WCHAR empty[1];
    struct dir_data_names *names = data->names;

    if (data->count >= data->size)
    {
        unsigned int new_size = max( data->size * 2, dir_data_names_initial_size );

        if (names) names = RtlReAllocateHeap( GetProcessHeap(), 0, names, new_size * sizeof(*names) );
        else names = RtlAllocateHeap( GetProcessHeap(), 0, new_size * sizeof(*names) );
        if (!names) return FALSE;
        data->size  = new_size;
        data->names = names;
    }

    if (short_name[0])
    {
        if (!(names[data->count].short_name = add_dir_data_nameW( data, short_name ))) return FALSE;
    }
    else names[data->count].short_name = empty;

    if (!(names[data->count].long_name = add_dir_data_nameW( data, long_name ))) return FALSE;
    if (!(names[data->count].unix_name = add_dir_data_nameA( data, unix_name ))) return FALSE;
    data->count++;
    return TRUE;
}

/* free the complete directory data structure */
static void free_dir_data( struct dir_data *data )
{
    struct dir_data_buffer *buffer, *next;

    if (!data) return;

    for (buffer = data->buffer; buffer; buffer = next)
    {
        next = buffer->next;
        RtlFreeHeap( GetProcessHeap(), 0, buffer );
    }
    RtlFreeHeap( GetProcessHeap(), 0, data->names );
    RtlFreeHeap( GetProcessHeap(), 0, data );
}


/* support for a directory queue for filesystem searches */

struct dir_name
{
    struct list entry;
    char name[1];
};

static struct list dir_queue = LIST_INIT( dir_queue );

static NTSTATUS add_dir_to_queue( const char *name )
{
    int len = strlen( name ) + 1;
    struct dir_name *dir = RtlAllocateHeap( GetProcessHeap(), 0,
                                            FIELD_OFFSET( struct dir_name, name[len] ));
    if (!dir) return STATUS_NO_MEMORY;
    strcpy( dir->name, name );
    list_add_tail( &dir_queue, &dir->entry );
    return STATUS_SUCCESS;
}

static NTSTATUS next_dir_in_queue( char *name )
{
    struct list *head = list_head( &dir_queue );
    if (head)
    {
        struct dir_name *dir = LIST_ENTRY( head, struct dir_name, entry );
        strcpy( name, dir->name );
        list_remove( &dir->entry );
        RtlFreeHeap( GetProcessHeap(), 0, dir );
        return STATUS_SUCCESS;
    }
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

static void flush_dir_queue(void)
{
    struct list *head;

    while ((head = list_head( &dir_queue )))
    {
        struct dir_name *dir = LIST_ENTRY( head, struct dir_name, entry );
        list_remove( &dir->entry );
        RtlFreeHeap( GetProcessHeap(), 0, dir );
    }
}


#ifdef __ANDROID__

static char *unescape_field( char *str )
{
    char *in, *out;

    for (in = out = str; *in; in++, out++)
    {
        *out = *in;
        if (in[0] == '\\')
        {
            if (in[1] == '\\')
            {
                out[0] = '\\';
                in++;
            }
            else if (in[1] == '0' && in[2] == '4' && in[3] == '0')
            {
                out[0] = ' ';
                in += 3;
            }
            else if (in[1] == '0' && in[2] == '1' && in[3] == '1')
            {
                out[0] = '\t';
                in += 3;
            }
            else if (in[1] == '0' && in[2] == '1' && in[3] == '2')
            {
                out[0] = '\n';
                in += 3;
            }
            else if (in[1] == '1' && in[2] == '3' && in[3] == '4')
            {
                out[0] = '\\';
                in += 3;
            }
        }
    }
    *out = '\0';

    return str;
}

static inline char *get_field( char **str )
{
    char *ret;

    ret = strsep( str, " \t" );
    if (*str) *str += strspn( *str, " \t" );

    return ret;
}
/************************************************************************
 *                    getmntent_replacement
 *
 * getmntent replacement for Android.
 *
 * NB returned static buffer is not thread safe; protect with dir_section.
 */
static struct mntent *getmntent_replacement( FILE *f )
{
    static struct mntent entry;
    static char buf[4096];
    char *p, *start;

    do
    {
        if (!fgets( buf, sizeof(buf), f )) return NULL;
        p = strchr( buf, '\n' );
        if (p) *p = '\0';
        else /* Partially unread line, move file ptr to end */
        {
            char tmp[1024];
            while (fgets( tmp, sizeof(tmp), f ))
                if (strchr( tmp, '\n' )) break;
        }
        start = buf + strspn( buf, " \t" );
    } while (start[0] == '\0' || start[0] == '#');

    p = get_field( &start );
    entry.mnt_fsname = p ? unescape_field( p ) : (char *)"";

    p = get_field( &start );
    entry.mnt_dir = p ? unescape_field( p ) : (char *)"";

    p = get_field( &start );
    entry.mnt_type = p ? unescape_field( p ) : (char *)"";

    p = get_field( &start );
    entry.mnt_opts = p ? unescape_field( p ) : (char *)"";

    p = get_field( &start );
    entry.mnt_freq = p ? atoi(p) : 0;

    p = get_field( &start );
    entry.mnt_passno = p ? atoi(p) : 0;

    return &entry;
}
#define getmntent getmntent_replacement
#endif

/***********************************************************************
 *           DIR_get_drives_info
 *
 * Retrieve device/inode number for all the drives. Helper for find_drive_root.
 */
unsigned int DIR_get_drives_info( struct drive_info info[MAX_DOS_DRIVES] )
{
    static struct drive_info cache[MAX_DOS_DRIVES];
    static time_t last_update;
    static unsigned int nb_drives;
    unsigned int ret;
    time_t now = time(NULL);

    RtlEnterCriticalSection( &dir_section );
    if (now != last_update)
    {
        const char *config_dir = wine_get_config_dir();
        char *buffer, *p;
        struct stat st;
        unsigned int i;

        if ((buffer = RtlAllocateHeap( GetProcessHeap(), 0,
                                       strlen(config_dir) + sizeof("/dosdevices/a:") )))
        {
            strcpy( buffer, config_dir );
            strcat( buffer, "/dosdevices/a:" );
            p = buffer + strlen(buffer) - 2;

            for (i = nb_drives = 0; i < MAX_DOS_DRIVES; i++)
            {
                *p = 'a' + i;
                if (!stat( buffer, &st ))
                {
                    cache[i].dev = st.st_dev;
                    cache[i].ino = st.st_ino;
                    nb_drives++;
                }
                else
                {
                    cache[i].dev = 0;
                    cache[i].ino = 0;
                }
            }
            RtlFreeHeap( GetProcessHeap(), 0, buffer );
        }
        last_update = now;
    }
    memcpy( info, cache, sizeof(cache) );
    ret = nb_drives;
    RtlLeaveCriticalSection( &dir_section );
    return ret;
}


/***********************************************************************
 *           parse_mount_entries
 *
 * Parse mount entries looking for a given device. Helper for get_default_drive_device.
 */

#ifdef sun
#include <sys/vfstab.h>
static char *parse_vfstab_entries( FILE *f, dev_t dev, ino_t ino)
{
    struct vfstab entry;
    struct stat st;
    char *device;

    while (! getvfsent( f, &entry ))
    {
        /* don't even bother stat'ing network mounts, there's no meaningful device anyway */
        if (!strcmp( entry.vfs_fstype, "nfs" ) ||
            !strcmp( entry.vfs_fstype, "smbfs" ) ||
            !strcmp( entry.vfs_fstype, "ncpfs" )) continue;

        if (stat( entry.vfs_mountp, &st ) == -1) continue;
        if (st.st_dev != dev || st.st_ino != ino) continue;
        if (!strcmp( entry.vfs_fstype, "fd" ))
        {
            if ((device = strstr( entry.vfs_mntopts, "dev=" )))
            {
                char *p = strchr( device + 4, ',' );
                if (p) *p = 0;
                return device + 4;
            }
        }
        else
            return entry.vfs_special;
    }
    return NULL;
}
#endif

#ifdef linux
static char *parse_mount_entries( FILE *f, dev_t dev, ino_t ino )
{
    struct mntent *entry;
    struct stat st;
    char *device;

    while ((entry = getmntent( f )))
    {
        /* don't even bother stat'ing network mounts, there's no meaningful device anyway */
        if (!strcmp( entry->mnt_type, "nfs" ) ||
            !strcmp( entry->mnt_type, "cifs" ) ||
            !strcmp( entry->mnt_type, "smbfs" ) ||
            !strcmp( entry->mnt_type, "ncpfs" )) continue;

        if (stat( entry->mnt_dir, &st ) == -1) continue;
        if (st.st_dev != dev || st.st_ino != ino) continue;
        if (!strcmp( entry->mnt_type, "supermount" ))
        {
            if ((device = strstr( entry->mnt_opts, "dev=" )))
            {
                char *p = strchr( device + 4, ',' );
                if (p) *p = 0;
                return device + 4;
            }
        }
        else if (!stat( entry->mnt_fsname, &st ) && S_ISREG(st.st_mode))
        {
            /* if device is a regular file check for a loop mount */
            if ((device = strstr( entry->mnt_opts, "loop=" )))
            {
                char *p = strchr( device + 5, ',' );
                if (p) *p = 0;
                return device + 5;
            }
        }
        else
            return entry->mnt_fsname;
    }
    return NULL;
}
#endif

#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || defined(__DragonFly__)
#include <fstab.h>
static char *parse_mount_entries( FILE *f, dev_t dev, ino_t ino )
{
    struct fstab *entry;
    struct stat st;

    while ((entry = getfsent()))
    {
        /* don't even bother stat'ing network mounts, there's no meaningful device anyway */
        if (!strcmp( entry->fs_vfstype, "nfs" ) ||
            !strcmp( entry->fs_vfstype, "smbfs" ) ||
            !strcmp( entry->fs_vfstype, "ncpfs" )) continue;

        if (stat( entry->fs_file, &st ) == -1) continue;
        if (st.st_dev != dev || st.st_ino != ino) continue;
        return entry->fs_spec;
    }
    return NULL;
}
#endif

#ifdef sun
#include <sys/mnttab.h>
static char *parse_mount_entries( FILE *f, dev_t dev, ino_t ino )
{
    struct mnttab entry;
    struct stat st;
    char *device;


    while (( ! getmntent( f, &entry) ))
    {
        /* don't even bother stat'ing network mounts, there's no meaningful device anyway */
        if (!strcmp( entry.mnt_fstype, "nfs" ) ||
            !strcmp( entry.mnt_fstype, "smbfs" ) ||
            !strcmp( entry.mnt_fstype, "ncpfs" )) continue;

        if (stat( entry.mnt_mountp, &st ) == -1) continue;
        if (st.st_dev != dev || st.st_ino != ino) continue;
        if (!strcmp( entry.mnt_fstype, "fd" ))
        {
            if ((device = strstr( entry.mnt_mntopts, "dev=" )))
            {
                char *p = strchr( device + 4, ',' );
                if (p) *p = 0;
                return device + 4;
            }
        }
        else
            return entry.mnt_special;
    }
    return NULL;
}
#endif

/***********************************************************************
 *           get_default_drive_device
 *
 * Return the default device to use for a given drive mount point.
 */
static char *get_default_drive_device( const char *root )
{
    char *ret = NULL;

#ifdef linux
    FILE *f;
    char *device = NULL;
    int fd, res = -1;
    struct stat st;

    /* try to open it first to force it to get mounted */
    if ((fd = open( root, O_RDONLY | O_DIRECTORY )) != -1)
    {
        res = fstat( fd, &st );
        close( fd );
    }
    /* now try normal stat just in case */
    if (res == -1) res = stat( root, &st );
    if (res == -1) return NULL;

    RtlEnterCriticalSection( &dir_section );

#ifdef __ANDROID__
    if ((f = fopen( "/proc/mounts", "r" )))
    {
        device = parse_mount_entries( f, st.st_dev, st.st_ino );
        fclose( f );
    }
#else
    if ((f = fopen( "/etc/mtab", "r" )))
    {
        device = parse_mount_entries( f, st.st_dev, st.st_ino );
        fclose( f );
    }
    /* look through fstab too in case it's not mounted (for instance if it's an audio CD) */
    if (!device && (f = fopen( "/etc/fstab", "r" )))
    {
        device = parse_mount_entries( f, st.st_dev, st.st_ino );
        fclose( f );
    }
#endif
    if (device)
    {
        ret = RtlAllocateHeap( GetProcessHeap(), 0, strlen(device) + 1 );
        if (ret) strcpy( ret, device );
    }
    RtlLeaveCriticalSection( &dir_section );

#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel__ ) || defined(__DragonFly__)
    char *device = NULL;
    int fd, res = -1;
    struct stat st;

    /* try to open it first to force it to get mounted */
    if ((fd = open( root, O_RDONLY )) != -1)
    {
        res = fstat( fd, &st );
        close( fd );
    }
    /* now try normal stat just in case */
    if (res == -1) res = stat( root, &st );
    if (res == -1) return NULL;

    RtlEnterCriticalSection( &dir_section );

    /* The FreeBSD parse_mount_entries doesn't require a file argument, so just
     * pass NULL.  Leave the argument in for symmetry.
     */
    device = parse_mount_entries( NULL, st.st_dev, st.st_ino );
    if (device)
    {
        ret = RtlAllocateHeap( GetProcessHeap(), 0, strlen(device) + 1 );
        if (ret) strcpy( ret, device );
    }
    RtlLeaveCriticalSection( &dir_section );

#elif defined( sun )
    FILE *f;
    char *device = NULL;
    int fd, res = -1;
    struct stat st;

    /* try to open it first to force it to get mounted */
    if ((fd = open( root, O_RDONLY )) != -1)
    {
        res = fstat( fd, &st );
        close( fd );
    }
    /* now try normal stat just in case */
    if (res == -1) res = stat( root, &st );
    if (res == -1) return NULL;

    RtlEnterCriticalSection( &dir_section );

    if ((f = fopen( "/etc/mnttab", "r" )))
    {
        device = parse_mount_entries( f, st.st_dev, st.st_ino);
        fclose( f );
    }
    /* look through fstab too in case it's not mounted (for instance if it's an audio CD) */
    if (!device && (f = fopen( "/etc/vfstab", "r" )))
    {
        device = parse_vfstab_entries( f, st.st_dev, st.st_ino );
        fclose( f );
    }
    if (device)
    {
        ret = RtlAllocateHeap( GetProcessHeap(), 0, strlen(device) + 1 );
        if (ret) strcpy( ret, device );
    }
    RtlLeaveCriticalSection( &dir_section );

#elif defined(__APPLE__)
    struct statfs *mntStat;
    struct stat st;
    int i;
    int mntSize;
    dev_t dev;
    ino_t ino;
    static const char path_bsd_device[] = "/dev/disk";
    int res;

    res = stat( root, &st );
    if (res == -1) return NULL;

    dev = st.st_dev;
    ino = st.st_ino;

    RtlEnterCriticalSection( &dir_section );

    mntSize = getmntinfo(&mntStat, MNT_NOWAIT);

    for (i = 0; i < mntSize && !ret; i++)
    {
        if (stat(mntStat[i].f_mntonname, &st ) == -1) continue;
        if (st.st_dev != dev || st.st_ino != ino) continue;

        /* FIXME add support for mounted network drive */
        if ( strncmp(mntStat[i].f_mntfromname, path_bsd_device, strlen(path_bsd_device)) == 0)
        {
            /* set return value to the corresponding raw BSD node */
            ret = RtlAllocateHeap( GetProcessHeap(), 0, strlen(mntStat[i].f_mntfromname) + 2 /* 2 : r and \0 */ );
            if (ret)
            {
                strcpy(ret, "/dev/r");
                strcat(ret, mntStat[i].f_mntfromname+sizeof("/dev/")-1);
            }
        }
    }
    RtlLeaveCriticalSection( &dir_section );
#else
    static int warned;
    if (!warned++) FIXME( "auto detection of DOS devices not supported on this platform\n" );
#endif
    return ret;
}


/***********************************************************************
 *           get_device_mount_point
 *
 * Return the current mount point for a device.
 */
static char *get_device_mount_point( dev_t dev )
{
    char *ret = NULL;

#ifdef linux
    FILE *f;

    RtlEnterCriticalSection( &dir_section );

#ifdef __ANDROID__
    if ((f = fopen( "/proc/mounts", "r" )))
#else
    if ((f = fopen( "/etc/mtab", "r" )))
#endif
    {
        struct mntent *entry;
        struct stat st;
        char *p, *device;

        while ((entry = getmntent( f )))
        {
            /* don't even bother stat'ing network mounts, there's no meaningful device anyway */
            if (!strcmp( entry->mnt_type, "nfs" ) ||
                !strcmp( entry->mnt_type, "cifs" ) ||
                !strcmp( entry->mnt_type, "smbfs" ) ||
                !strcmp( entry->mnt_type, "ncpfs" )) continue;

            if (!strcmp( entry->mnt_type, "supermount" ))
            {
                if ((device = strstr( entry->mnt_opts, "dev=" )))
                {
                    device += 4;
                    if ((p = strchr( device, ',' ))) *p = 0;
                }
            }
            else if (!stat( entry->mnt_fsname, &st ) && S_ISREG(st.st_mode))
            {
                /* if device is a regular file check for a loop mount */
                if ((device = strstr( entry->mnt_opts, "loop=" )))
                {
                    device += 5;
                    if ((p = strchr( device, ',' ))) *p = 0;
                }
            }
            else device = entry->mnt_fsname;

            if (device && !stat( device, &st ) && S_ISBLK(st.st_mode) && st.st_rdev == dev)
            {
                ret = RtlAllocateHeap( GetProcessHeap(), 0, strlen(entry->mnt_dir) + 1 );
                if (ret) strcpy( ret, entry->mnt_dir );
                break;
            }
        }
        fclose( f );
    }
    RtlLeaveCriticalSection( &dir_section );
#elif defined(__APPLE__)
    struct statfs *entry;
    struct stat st;
    int i, size;

    RtlEnterCriticalSection( &dir_section );

    size = getmntinfo( &entry, MNT_NOWAIT );
    for (i = 0; i < size; i++)
    {
        if (stat( entry[i].f_mntfromname, &st ) == -1) continue;
        if (S_ISBLK(st.st_mode) && st.st_rdev == dev)
        {
            ret = RtlAllocateHeap( GetProcessHeap(), 0, strlen(entry[i].f_mntonname) + 1 );
            if (ret) strcpy( ret, entry[i].f_mntonname );
            break;
        }
    }
    RtlLeaveCriticalSection( &dir_section );
#else
    static int warned;
    if (!warned++) FIXME( "unmounting devices not supported on this platform\n" );
#endif
    return ret;
}


#if defined(HAVE_GETATTRLIST) && defined(ATTR_VOL_CAPABILITIES) && \
    defined(VOL_CAPABILITIES_FORMAT) && defined(VOL_CAP_FMT_CASE_SENSITIVE)

struct get_fsid
{
    ULONG size;
    dev_t dev;
    fsid_t fsid;
};

struct fs_cache
{
    dev_t dev;
    fsid_t fsid;
    BOOLEAN case_sensitive;
} fs_cache[64];

struct vol_caps
{
    ULONG size;
    vol_capabilities_attr_t caps;
};

/***********************************************************************
 *           look_up_fs_cache
 *
 * Checks if the specified file system is in the cache.
 */
static struct fs_cache *look_up_fs_cache( dev_t dev )
{
    int i;
    for (i = 0; i < ARRAY_SIZE( fs_cache ); i++)
        if (fs_cache[i].dev == dev)
            return fs_cache+i;
    return NULL;
}

/***********************************************************************
 *           add_fs_cache
 *
 * Adds the specified file system to the cache.
 */
static void add_fs_cache( dev_t dev, fsid_t fsid, BOOLEAN case_sensitive )
{
    int i;
    struct fs_cache *entry = look_up_fs_cache( dev );
    static int once = 0;
    if (entry)
    {
        /* Update the cache */
        entry->fsid = fsid;
        entry->case_sensitive = case_sensitive;
        return;
    }

    /* Add a new entry */
    for (i = 0; i < ARRAY_SIZE( fs_cache ); i++)
        if (fs_cache[i].dev == 0)
        {
            /* This entry is empty, use it */
            fs_cache[i].dev = dev;
            fs_cache[i].fsid = fsid;
            fs_cache[i].case_sensitive = case_sensitive;
            return;
        }

    /* Cache is out of space, warn */
    if (!once++)
        WARN( "FS cache is out of space, expect performance problems\n" );
}

/***********************************************************************
 *           get_dir_case_sensitivity_attr
 *
 * Checks if the volume containing the specified directory is case
 * sensitive or not. Uses getattrlist(2).
 */
static int get_dir_case_sensitivity_attr( const char *dir )
{
    char *mntpoint;
    struct attrlist attr;
    struct vol_caps caps;
    struct get_fsid get_fsid;
    struct fs_cache *entry;

    /* First get the FS ID of the volume */
    attr.bitmapcount = ATTR_BIT_MAP_COUNT;
    attr.reserved = 0;
    attr.commonattr = ATTR_CMN_DEVID|ATTR_CMN_FSID;
    attr.volattr = attr.dirattr = attr.fileattr = attr.forkattr = 0;
    get_fsid.size = 0;
    if (getattrlist( dir, &attr, &get_fsid, sizeof(get_fsid), 0 ) != 0 ||
        get_fsid.size != sizeof(get_fsid))
        return -1;
    /* Try to look it up in the cache */
    entry = look_up_fs_cache( get_fsid.dev );
    if (entry && !memcmp( &entry->fsid, &get_fsid.fsid, sizeof(fsid_t) ))
        /* Cache lookup succeeded */
        return entry->case_sensitive;
    /* Cache is stale at this point, we have to update it */

    mntpoint = get_device_mount_point( get_fsid.dev );
    /* Now look up the case-sensitivity */
    attr.commonattr = 0;
    attr.volattr = ATTR_VOL_INFO|ATTR_VOL_CAPABILITIES;
    if (getattrlist( mntpoint, &attr, &caps, sizeof(caps), 0 ) < 0)
    {
        RtlFreeHeap( GetProcessHeap(), 0, mntpoint );
        add_fs_cache( get_fsid.dev, get_fsid.fsid, TRUE );
        return TRUE;
    }
    RtlFreeHeap( GetProcessHeap(), 0, mntpoint );
    if (caps.size == sizeof(caps) &&
        (caps.caps.valid[VOL_CAPABILITIES_FORMAT] &
         (VOL_CAP_FMT_CASE_SENSITIVE | VOL_CAP_FMT_CASE_PRESERVING)) ==
        (VOL_CAP_FMT_CASE_SENSITIVE | VOL_CAP_FMT_CASE_PRESERVING))
    {
        BOOLEAN ret;

        if ((caps.caps.capabilities[VOL_CAPABILITIES_FORMAT] &
            VOL_CAP_FMT_CASE_SENSITIVE) != VOL_CAP_FMT_CASE_SENSITIVE)
            ret = FALSE;
        else
            ret = TRUE;
        /* Update the cache */
        add_fs_cache( get_fsid.dev, get_fsid.fsid, ret );
        return ret;
    }
    return FALSE;
}
#endif

/***********************************************************************
 *           get_dir_case_sensitivity_stat
 *
 * Checks if the volume containing the specified directory is case
 * sensitive or not. Uses statfs(2) or statvfs(2).
 */
static BOOLEAN get_dir_case_sensitivity_stat( const char *dir )
{
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
    struct statfs stfs;

    if (statfs( dir, &stfs ) == -1) return FALSE;
    /* Assume these file systems are always case insensitive on Mac OS.
     * For FreeBSD, only assume CIOPFS is case insensitive (AFAIK, Mac OS
     * is the only UNIX that supports case-insensitive lookup).
     */
    if (!strcmp( stfs.f_fstypename, "fusefs" ) &&
        !strncmp( stfs.f_mntfromname, "ciopfs", 5 ))
        return FALSE;
#ifdef __APPLE__
    if (!strcmp( stfs.f_fstypename, "msdos" ) ||
        !strcmp( stfs.f_fstypename, "cd9660" ) ||
        !strcmp( stfs.f_fstypename, "udf" ) ||
        !strcmp( stfs.f_fstypename, "ntfs" ) ||
        !strcmp( stfs.f_fstypename, "smbfs" ))
        return FALSE;
#ifdef _DARWIN_FEATURE_64_BIT_INODE
     if (!strcmp( stfs.f_fstypename, "hfs" ) && (stfs.f_fssubtype == 0 ||
                                                 stfs.f_fssubtype == 1 ||
                                                 stfs.f_fssubtype == 128))
        return FALSE;
#else
     /* The field says "reserved", but a quick look at the kernel source
      * tells us that this "reserved" field is really the same as the
      * "fssubtype" field from the inode64 structure (see munge_statfs()
      * in <xnu-source>/bsd/vfs/vfs_syscalls.c).
      */
     if (!strcmp( stfs.f_fstypename, "hfs" ) && (stfs.f_reserved1 == 0 ||
                                                 stfs.f_reserved1 == 1 ||
                                                 stfs.f_reserved1 == 128))
        return FALSE;
#endif
#endif
    return TRUE;

#elif defined(__NetBSD__)
    struct statvfs stfs;

    if (statvfs( dir, &stfs ) == -1) return FALSE;
    /* Only assume CIOPFS is case insensitive. */
    if (strcmp( stfs.f_fstypename, "fusefs" ) ||
        strncmp( stfs.f_mntfromname, "ciopfs", 5 ))
        return TRUE;
    return FALSE;

#elif defined(__linux__)
    struct statfs stfs;
    struct stat st;
    char *cifile;

    /* Only assume CIOPFS is case insensitive. */
    if (statfs( dir, &stfs ) == -1) return FALSE;
    if (stfs.f_type != 0x65735546 /* FUSE_SUPER_MAGIC */)
        return TRUE;
    /* Normally, we'd have to parse the mtab to find out exactly what
     * kind of FUSE FS this is. But, someone on wine-devel suggested
     * a shortcut. We'll stat a special file in the directory. If it's
     * there, we'll assume it's a CIOPFS, else not.
     * This will break if somebody puts a file named ".ciopfs" in a non-
     * CIOPFS directory.
     */
    cifile = RtlAllocateHeap( GetProcessHeap(), 0, strlen( dir )+sizeof("/.ciopfs") );
    if (!cifile) return TRUE;
    strcpy( cifile, dir );
    strcat( cifile, "/.ciopfs" );
    if (stat( cifile, &st ) == 0)
    {
        RtlFreeHeap( GetProcessHeap(), 0, cifile );
        return FALSE;
    }
    RtlFreeHeap( GetProcessHeap(), 0, cifile );
    return TRUE;
#else
    return TRUE;
#endif
}


/***********************************************************************
 *           get_dir_case_sensitivity
 *
 * Checks if the volume containing the specified directory is case
 * sensitive or not. Uses statfs(2) or statvfs(2).
 */
static BOOLEAN get_dir_case_sensitivity( const char *dir )
{
#if defined(HAVE_GETATTRLIST) && defined(ATTR_VOL_CAPABILITIES) && \
    defined(VOL_CAPABILITIES_FORMAT) && defined(VOL_CAP_FMT_CASE_SENSITIVE)
    int case_sensitive = get_dir_case_sensitivity_attr( dir );
    if (case_sensitive != -1) return case_sensitive;
#endif
    return get_dir_case_sensitivity_stat( dir );
}


/***********************************************************************
 *           init_options
 *
 * Initialize the show_dot_files options.
 */
static DWORD WINAPI init_options( RTL_RUN_ONCE *once, void *param, void **context )
{
    static const WCHAR WineW[] = {'S','o','f','t','w','a','r','e','\\','W','i','n','e',0};
    static const WCHAR ShowDotFilesW[] = {'S','h','o','w','D','o','t','F','i','l','e','s',0};
    char tmp[80];
    HANDLE root, hkey;
    DWORD dummy;
    OBJECT_ATTRIBUTES attr;
    UNICODE_STRING nameW;

    RtlOpenCurrentUser( KEY_ALL_ACCESS, &root );
    attr.Length = sizeof(attr);
    attr.RootDirectory = root;
    attr.ObjectName = &nameW;
    attr.Attributes = 0;
    attr.SecurityDescriptor = NULL;
    attr.SecurityQualityOfService = NULL;
    RtlInitUnicodeString( &nameW, WineW );

    /* @@ Wine registry key: HKCU\Software\Wine */
    if (!NtOpenKey( &hkey, KEY_ALL_ACCESS, &attr ))
    {
        RtlInitUnicodeString( &nameW, ShowDotFilesW );
        if (!NtQueryValueKey( hkey, &nameW, KeyValuePartialInformation, tmp, sizeof(tmp), &dummy ))
        {
            WCHAR *str = (WCHAR *)((KEY_VALUE_PARTIAL_INFORMATION *)tmp)->Data;
            show_dot_files = IS_OPTION_TRUE( str[0] );
        }
        NtClose( hkey );
    }
    NtClose( root );

    /* a couple of directories that we don't want to return in directory searches */
    ignore_file( wine_get_config_dir() );
    ignore_file( "/dev" );
    ignore_file( "/proc" );
#ifdef linux
    ignore_file( "/sys" );
#endif
    return TRUE;
}


/***********************************************************************
 *           DIR_is_hidden_file
 *
 * Check if the specified file should be hidden based on its name and the show dot files option.
 */
BOOL DIR_is_hidden_file( const UNICODE_STRING *name )
{
    WCHAR *p, *end;

    RtlRunOnceExecuteOnce( &init_once, init_options, NULL, NULL );

    if (show_dot_files) return FALSE;

    end = p = name->Buffer + name->Length/sizeof(WCHAR);
    while (p > name->Buffer && IS_SEPARATOR(p[-1])) p--;
    while (p > name->Buffer && !IS_SEPARATOR(p[-1])) p--;
    if (p == end || *p != '.') return FALSE;
    /* make sure it isn't '.' or '..' */
    if (p + 1 == end) return FALSE;
    if (p[1] == '.' && p + 2 == end) return FALSE;
    return TRUE;
}


/***********************************************************************
 *           hash_short_file_name
 *
 * Transform a Unix file name into a hashed DOS name. If the name is a valid
 * DOS name, it is converted to upper-case; otherwise it is replaced by a
 * hashed version that fits in 8.3 format.
 * 'buffer' must be at least 12 characters long.
 * Returns length of short name in bytes; short name is NOT null-terminated.
 */
static ULONG hash_short_file_name( const UNICODE_STRING *name, LPWSTR buffer )
{
    static const char hash_chars[32] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";

    LPCWSTR p, ext, end = name->Buffer + name->Length / sizeof(WCHAR);
    LPWSTR dst;
    unsigned short hash;
    int i;

    /* Compute the hash code of the file name */
    /* If you know something about hash functions, feel free to */
    /* insert a better algorithm here... */
    if (!is_case_sensitive)
    {
        for (p = name->Buffer, hash = 0xbeef; p < end - 1; p++)
            hash = (hash<<3) ^ (hash>>5) ^ tolowerW(*p) ^ (tolowerW(p[1]) << 8);
        hash = (hash<<3) ^ (hash>>5) ^ tolowerW(*p); /* Last character */
    }
    else
    {
        for (p = name->Buffer, hash = 0xbeef; p < end - 1; p++)
            hash = (hash << 3) ^ (hash >> 5) ^ *p ^ (p[1] << 8);
        hash = (hash << 3) ^ (hash >> 5) ^ *p;  /* Last character */
    }

    /* Find last dot for start of the extension */
    for (p = name->Buffer + 1, ext = NULL; p < end - 1; p++) if (*p == '.') ext = p;

    /* Copy first 4 chars, replacing invalid chars with '_' */
    for (i = 4, p = name->Buffer, dst = buffer; i > 0; i--, p++)
    {
        if (p == end || p == ext) break;
        *dst++ = is_invalid_dos_char(*p) ? '_' : toupperW(*p);
    }
    /* Pad to 5 chars with '~' */
    while (i-- >= 0) *dst++ = '~';

    /* Insert hash code converted to 3 ASCII chars */
    *dst++ = hash_chars[(hash >> 10) & 0x1f];
    *dst++ = hash_chars[(hash >> 5) & 0x1f];
    *dst++ = hash_chars[hash & 0x1f];

    /* Copy the first 3 chars of the extension (if any) */
    if (ext)
    {
        *dst++ = '.';
        for (i = 3, ext++; (i > 0) && ext < end; i--, ext++)
            *dst++ = is_invalid_dos_char(*ext) ? '_' : toupperW(*ext);
    }
    return dst - buffer;
}


/***********************************************************************
 *           match_filename
 *
 * Check a long file name against a mask.
 *
 * Tests (done in W95 DOS shell - case insensitive):
 * *.txt			test1.test.txt				*
 * *st1*			test1.txt				*
 * *.t??????.t*			test1.ta.tornado.txt			*
 * *tornado*			test1.ta.tornado.txt			*
 * t*t				test1.ta.tornado.txt			*
 * ?est*			test1.txt				*
 * ?est???			test1.txt				-
 * *test1.txt*			test1.txt				*
 * h?l?o*t.dat			hellothisisatest.dat			*
 */
static BOOLEAN match_filename( const UNICODE_STRING *name_str, const UNICODE_STRING *mask_str )
{
    BOOL mismatch;
    const WCHAR *name = name_str->Buffer;
    const WCHAR *mask = mask_str->Buffer;
    const WCHAR *name_end = name + name_str->Length / sizeof(WCHAR);
    const WCHAR *mask_end = mask + mask_str->Length / sizeof(WCHAR);
    const WCHAR *lastjoker = NULL;
    const WCHAR *next_to_retry = NULL;

    while (name < name_end && mask < mask_end)
    {
        switch(*mask)
        {
        case '*':
            mask++;
            while (mask < mask_end && *mask == '*') mask++;  /* Skip consecutive '*' */
            if (mask == mask_end) return TRUE; /* end of mask is all '*', so match */
            lastjoker = mask;

            /* skip to the next match after the joker(s) */
            if (is_case_sensitive)
                while (name < name_end && (*name != *mask)) name++;
            else
                while (name < name_end && (toupperW(*name) != toupperW(*mask))) name++;
            next_to_retry = name;
            break;
        case '?':
            mask++;
            name++;
            break;
        default:
            if (is_case_sensitive) mismatch = (*mask != *name);
            else mismatch = (toupperW(*mask) != toupperW(*name));

            if (!mismatch)
            {
                mask++;
                name++;
                if (mask == mask_end)
                {
                    if (name == name_end) return TRUE;
                    if (lastjoker) mask = lastjoker;
                }
            }
            else /* mismatch ! */
            {
                if (lastjoker) /* we had an '*', so we can try unlimitedly */
                {
                    mask = lastjoker;

                    /* this scan sequence was a mismatch, so restart
                     * 1 char after the first char we checked last time */
                    next_to_retry++;
                    name = next_to_retry;
                }
                else return FALSE; /* bad luck */
            }
            break;
        }
    }
    while (mask < mask_end && ((*mask == '.') || (*mask == '*')))
        mask++;  /* Ignore trailing '.' or '*' in mask */
    return (name == name_end && mask == mask_end);
}


/***********************************************************************
 *           append_entry
 *
 * Add a file to the directory data if it matches the mask.
 */
static BOOL append_entry( struct dir_data *data, const char *long_name,
                          const char *short_name, const UNICODE_STRING *mask )
{
    int i, long_len, short_len;
    WCHAR long_nameW[MAX_DIR_ENTRY_LEN + 1];
    WCHAR short_nameW[13];
    UNICODE_STRING str;

    long_len = ntdll_umbstowcs( 0, long_name, strlen(long_name), long_nameW, MAX_DIR_ENTRY_LEN );
    if (long_len == -1) return TRUE;
    long_nameW[long_len] = 0;

    str.Buffer = long_nameW;
    str.Length = long_len * sizeof(WCHAR);
    str.MaximumLength = sizeof(long_nameW);

    if (short_name)
    {
        short_len = ntdll_umbstowcs( 0, short_name, strlen(short_name),
                                     short_nameW, ARRAY_SIZE( short_nameW ) - 1 );
        if (short_len == -1) short_len = ARRAY_SIZE( short_nameW ) - 1;
        for (i = 0; i < short_len; i++) short_nameW[i] = toupperW( short_nameW[i] );
    }
    else  /* generate a short name if necessary */
    {
        BOOLEAN spaces;

        short_len = 0;
        if (!RtlIsNameLegalDOS8Dot3( &str, NULL, &spaces ) || spaces)
            short_len = hash_short_file_name( &str, short_nameW );
    }
    short_nameW[short_len] = 0;

    TRACE( "long %s short %s mask %s\n",
           debugstr_w( long_nameW ), debugstr_w( short_nameW ), debugstr_us( mask ));

    if (mask && !match_filename( &str, mask ))
    {
        if (!short_len) return TRUE;  /* no short name to match */
        str.Buffer = short_nameW;
        str.Length = short_len * sizeof(WCHAR);
        str.MaximumLength = sizeof(short_nameW);
        if (!match_filename( &str, mask )) return TRUE;
    }

    return add_dir_data_names( data, long_nameW, short_nameW, long_name );
}


/***********************************************************************
 *           get_dir_data_entry
 *
 * Return a directory entry from the cached data.
 */
static NTSTATUS get_dir_data_entry( struct dir_data *dir_data, void *info_ptr, IO_STATUS_BLOCK *io,
                                    ULONG max_length, FILE_INFORMATION_CLASS class,
                                    union file_directory_info **last_info )
{
    const struct dir_data_names *names = &dir_data->names[dir_data->pos];
    union file_directory_info *info;
    struct stat st;
    ULONG name_len, start, dir_size, attributes;

    if (get_file_info( names->unix_name, &st, &attributes ) == -1)
    {
        TRACE( "file no longer exists %s\n", names->unix_name );
        return STATUS_SUCCESS;
    }
    if (is_ignored_file( &st ))
    {
        TRACE( "ignoring file %s\n", names->unix_name );
        return STATUS_SUCCESS;
    }
    start = dir_info_align( io->Information );
    dir_size = dir_info_size( class, 0 );
    if (start + dir_size > max_length) return STATUS_MORE_ENTRIES;

    max_length -= start + dir_size;
    name_len = strlenW( names->long_name ) * sizeof(WCHAR);
    /* if this is not the first entry, fail; the first entry is always returned (but truncated) */
    if (*last_info && name_len > max_length) return STATUS_MORE_ENTRIES;

    info = (union file_directory_info *)((char *)info_ptr + start);
    info->dir.NextEntryOffset = 0;
    info->dir.FileIndex = 0;  /* NTFS always has 0 here, so let's not bother with it */

    /* all the structures except FileNamesInformation start with a FileDirectoryInformation layout */
    if (class != FileNamesInformation)
    {
        if (st.st_dev != dir_data->id.dev) st.st_ino = 0;  /* ignore inode if on a different device */

        if (!show_dot_files && names->long_name[0] == '.' && names->long_name[1] &&
            (names->long_name[1] != '.' || names->long_name[2]))
            attributes |= FILE_ATTRIBUTE_HIDDEN;

        fill_file_info( &st, attributes, info, class );
    }

    switch (class)
    {
    case FileDirectoryInformation:
        info->dir.FileNameLength = name_len;
        break;

    case FileFullDirectoryInformation:
        info->full.EaSize = 0; /* FIXME */
        info->full.FileNameLength = name_len;
        break;

    case FileIdFullDirectoryInformation:
        info->id_full.EaSize = 0; /* FIXME */
        info->id_full.FileNameLength = name_len;
        break;

    case FileBothDirectoryInformation:
        info->both.EaSize = 0; /* FIXME */
        info->both.ShortNameLength = strlenW( names->short_name ) * sizeof(WCHAR);
        memcpy( info->both.ShortName, names->short_name, info->both.ShortNameLength );
        info->both.FileNameLength = name_len;
        break;

    case FileIdBothDirectoryInformation:
        info->id_both.EaSize = 0; /* FIXME */
        info->id_both.ShortNameLength = strlenW( names->short_name ) * sizeof(WCHAR);
        memcpy( info->id_both.ShortName, names->short_name, info->id_both.ShortNameLength );
        info->id_both.FileNameLength = name_len;
        break;

    case FileIdGlobalTxDirectoryInformation:
        info->id_tx.TxInfoFlags = 0;
        info->id_tx.FileNameLength = name_len;
        break;

    case FileNamesInformation:
        info->names.FileNameLength = name_len;
        break;

    default:
        assert(0);
        return 0;
    }

    memcpy( (char *)info + dir_size, names->long_name, min( name_len, max_length ) );
    io->Information = start + dir_size + min( name_len, max_length );
    if (*last_info) (*last_info)->next = (char *)info - (char *)*last_info;
    *last_info = info;
    return name_len > max_length ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}

#ifdef VFAT_IOCTL_READDIR_BOTH

/***********************************************************************
 *           start_vfat_ioctl
 *
 * Wrapper for the VFAT ioctl to work around various kernel bugs.
 * dir_section must be held by caller.
 */
static KERNEL_DIRENT *start_vfat_ioctl( int fd )
{
    static KERNEL_DIRENT *de;
    int res;

    if (!de)
    {
        SIZE_T size = 2 * sizeof(*de) + page_size;
        void *addr = NULL;

        if (NtAllocateVirtualMemory( GetCurrentProcess(), &addr, 1, &size, MEM_RESERVE, PAGE_READWRITE ))
            return NULL;
        /* commit only the size needed for the dir entries */
        /* this leaves an extra unaccessible page, which should make the kernel */
        /* fail with -EFAULT before it stomps all over our memory */
        de = addr;
        size = 2 * sizeof(*de);
        NtAllocateVirtualMemory( GetCurrentProcess(), &addr, 1, &size, MEM_COMMIT, PAGE_READWRITE );
    }

    /* set d_reclen to 65535 to work around an AFS kernel bug */
    de[0].d_reclen = 65535;
    res = ioctl( fd, VFAT_IOCTL_READDIR_BOTH, (long)de );
    if (res == -1)
    {
        if (errno != ENOENT) return NULL;  /* VFAT ioctl probably not supported */
        de[0].d_reclen = 0;  /* eof */
    }
    else if (!res && de[0].d_reclen == 65535) return NULL;  /* AFS bug */

    return de;
}


/***********************************************************************
 *           read_directory_vfat
 *
 * Read a directory using the VFAT ioctl; helper for NtQueryDirectoryFile.
 */
static NTSTATUS read_directory_data_vfat( struct dir_data *data, int fd, const UNICODE_STRING *mask )
{
    char *short_name, *long_name;
    size_t len;
    KERNEL_DIRENT *de;
    NTSTATUS status = STATUS_NO_MEMORY;
    off_t old_pos = lseek( fd, 0, SEEK_CUR );

    if (!(de = start_vfat_ioctl( fd ))) return STATUS_NOT_SUPPORTED;

    lseek( fd, 0, SEEK_SET );

    if (!append_entry( data, ".", NULL, mask )) goto done;
    if (!append_entry( data, "..", NULL, mask )) goto done;

    while (ioctl( fd, VFAT_IOCTL_READDIR_BOTH, (long)de ) != -1)
    {
        if (!de[0].d_reclen) break;  /* eof */

        /* make sure names are null-terminated to work around an x86-64 kernel bug */
        len = min( de[0].d_reclen, sizeof(de[0].d_name) - 1 );
        de[0].d_name[len] = 0;
        len = min( de[1].d_reclen, sizeof(de[1].d_name) - 1 );
        de[1].d_name[len] = 0;

        if (!strcmp( de[0].d_name, "." ) || !strcmp( de[0].d_name, ".." )) continue;
        if (de[1].d_name[0])
        {
            short_name = de[0].d_name;
            long_name = de[1].d_name;
        }
        else
        {
            long_name = de[0].d_name;
            short_name = NULL;
        }
        if (!append_entry( data, long_name, short_name, mask )) goto done;
    }
    status = STATUS_SUCCESS;
done:
    lseek( fd, old_pos, SEEK_SET );
    return status;
}
#endif /* VFAT_IOCTL_READDIR_BOTH */


#ifdef HAVE_GETATTRLIST
/***********************************************************************
 *           read_directory_getattrlist
 *
 * Read a single file from a directory by determining whether the file
 * identified by mask exists using getattrlist.
 */
static NTSTATUS read_directory_data_getattrlist( struct dir_data *data, const char *unix_name )
{
    struct attrlist attrlist;
#include "pshpack4.h"
    struct
    {
        u_int32_t length;
        struct attrreference name_reference;
        fsobj_type_t type;
        char name[NAME_MAX * 3 + 1];
    } buffer;
#include "poppack.h"

    memset( &attrlist, 0, sizeof(attrlist) );
    attrlist.bitmapcount = ATTR_BIT_MAP_COUNT;
    attrlist.commonattr = ATTR_CMN_NAME | ATTR_CMN_OBJTYPE;
    if (getattrlist( unix_name, &attrlist, &buffer, sizeof(buffer), FSOPT_NOFOLLOW ) == -1)
        return STATUS_NO_SUCH_FILE;
    /* If unix_name named a symlink, the above may have succeeded even if the symlink is broken.
       Check that with another call without FSOPT_NOFOLLOW.  We don't ask for any attributes. */
    if (buffer.type == VLNK)
    {
        u_int32_t dummy;
        attrlist.commonattr = 0;
        if (getattrlist( unix_name, &attrlist, &dummy, sizeof(dummy), 0 ) == -1)
            return STATUS_NO_SUCH_FILE;
    }

    TRACE( "found %s\n", buffer.name );

    if (!append_entry( data, buffer.name, NULL, NULL )) return STATUS_NO_MEMORY;

    return STATUS_SUCCESS;
}
#endif  /* HAVE_GETATTRLIST */


/***********************************************************************
 *           read_directory_stat
 *
 * Read a single file from a directory by determining whether the file
 * identified by mask exists using stat.
 */
static NTSTATUS read_directory_data_stat( struct dir_data *data, const char *unix_name )
{
    struct stat st;

    /* if the file system is not case sensitive we can't find the actual name through stat() */
    if (!get_dir_case_sensitivity(".")) return STATUS_NO_SUCH_FILE;
    if (stat( unix_name, &st ) == -1) return STATUS_NO_SUCH_FILE;

    TRACE( "found %s\n", unix_name );

    if (!append_entry( data, unix_name, NULL, NULL )) return STATUS_NO_MEMORY;

    return STATUS_SUCCESS;
}


/***********************************************************************
 *           read_directory_readdir
 *
 * Read a directory using the POSIX readdir interface; helper for NtQueryDirectoryFile.
 */
static NTSTATUS read_directory_data_readdir( struct dir_data *data, const UNICODE_STRING *mask )
{
    struct dirent *de;
    NTSTATUS status = STATUS_NO_MEMORY;
    DIR *dir = opendir( "." );

    if (!dir) return STATUS_NO_SUCH_FILE;

    if (!append_entry( data, ".", NULL, mask )) goto done;
    if (!append_entry( data, "..", NULL, mask )) goto done;
    while ((de = readdir( dir )))
    {
        if (!strcmp( de->d_name, "." ) || !strcmp( de->d_name, ".." )) continue;
        if (!append_entry( data, de->d_name, NULL, mask )) goto done;
    }
    status = STATUS_SUCCESS;

done:
    closedir( dir );
    return status;
}


/***********************************************************************
 *           read_directory_data
 *
 * Read the full contents of a directory, using one of the above helper functions.
 */
static NTSTATUS read_directory_data( struct dir_data *data, int fd, const UNICODE_STRING *mask )
{
    NTSTATUS status;

#ifdef VFAT_IOCTL_READDIR_BOTH
    if (!(status = read_directory_data_vfat( data, fd, mask ))) return status;
#endif

    if (!has_wildcard( mask ))
    {
        /* convert the mask to a Unix name and check for it */
        int ret, used_default;
        char unix_name[MAX_DIR_ENTRY_LEN * 3 + 1];

        ret = ntdll_wcstoumbs( 0, mask->Buffer, mask->Length / sizeof(WCHAR),
                               unix_name, sizeof(unix_name) - 1, NULL, &used_default );
        if (ret > 0 && !used_default)
        {
            unix_name[ret] = 0;
#ifdef HAVE_GETATTRLIST
            if (!(status = read_directory_data_getattrlist( data, unix_name ))) return status;
#endif
            if (!(status = read_directory_data_stat( data, unix_name ))) return status;
        }
    }

    return read_directory_data_readdir( data, mask );
}


/* compare file names for directory sorting */
static int name_compare( const void *a, const void *b )
{
    const struct dir_data_names *file_a = (const struct dir_data_names *)a;
    const struct dir_data_names *file_b = (const struct dir_data_names *)b;
    int ret = RtlCompareUnicodeStrings( file_a->long_name, strlenW(file_a->long_name),
                                        file_b->long_name, strlenW(file_b->long_name), TRUE );
    if (!ret) ret = strcmpW( file_a->long_name, file_b->long_name );
    return ret;
}


/***********************************************************************
 *           init_cached_dir_data
 *
 * Initialize the cached directory contents.
 */
static NTSTATUS init_cached_dir_data( struct dir_data **data_ret, int fd, const UNICODE_STRING *mask )
{
    struct dir_data *data;
    struct stat st;
    NTSTATUS status;
    unsigned int i;

    if (!(data = RtlAllocateHeap( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*data) )))
        return STATUS_NO_MEMORY;

    if ((status = read_directory_data( data, fd, mask )))
    {
        free_dir_data( data );
        return status;
    }

    /* sort filenames, but not "." and ".." */
    i = 0;
    if (i < data->count && !strcmp( data->names[i].unix_name, "." )) i++;
    if (i < data->count && !strcmp( data->names[i].unix_name, ".." )) i++;
    if (i < data->count) qsort( data->names + i, data->count - i, sizeof(*data->names), name_compare );

    if (data->count)
    {
        /* release unused space */
        if (data->buffer)
            RtlReAllocateHeap( GetProcessHeap(), HEAP_REALLOC_IN_PLACE_ONLY, data->buffer,
                               offsetof( struct dir_data_buffer, data[data->buffer->pos] ));
        if (data->count < data->size)
            RtlReAllocateHeap( GetProcessHeap(), HEAP_REALLOC_IN_PLACE_ONLY, data->names,
                               data->count * sizeof(*data->names) );
        if (!fstat( fd, &st ))
        {
            data->id.dev = st.st_dev;
            data->id.ino = st.st_ino;
        }
    }

    TRACE( "mask %s found %u files\n", debugstr_us( mask ), data->count );
    for (i = 0; i < data->count; i++)
        TRACE( "%s %s\n", debugstr_w(data->names[i].long_name), debugstr_w(data->names[i].short_name) );

    *data_ret = data;
    return data->count ? STATUS_SUCCESS : STATUS_NO_SUCH_FILE;
}


/***********************************************************************
 *           get_cached_dir_data
 *
 * Retrieve the cached directory data, or initialize it if necessary.
 */
static NTSTATUS get_cached_dir_data( HANDLE handle, struct dir_data **data_ret, int fd,
                                     const UNICODE_STRING *mask )
{
    unsigned int i;
    int entry = -1, free_entries[16];
    NTSTATUS status;

    SERVER_START_REQ( get_directory_cache_entry )
    {
        req->handle = wine_server_obj_handle( handle );
        wine_server_set_reply( req, free_entries, sizeof(free_entries) );
        if (!(status = wine_server_call( req ))) entry = reply->entry;

        for (i = 0; i < wine_server_reply_size( reply ) / sizeof(*free_entries); i++)
        {
            int free_idx = free_entries[i];
            if (free_idx < dir_data_cache_size)
            {
                free_dir_data( dir_data_cache[free_idx] );
                dir_data_cache[free_idx] = NULL;
            }
        }
    }
    SERVER_END_REQ;

    if (status)
    {
        if (status == STATUS_SHARING_VIOLATION) FIXME( "shared directory handle not supported yet\n" );
        return status;
    }

    if (entry >= dir_data_cache_size)
    {
        unsigned int size = max( dir_data_cache_initial_size, max( dir_data_cache_size * 2, entry + 1 ) );
        struct dir_data **new_cache;

        if (dir_data_cache)
            new_cache = RtlReAllocateHeap( GetProcessHeap(), HEAP_ZERO_MEMORY, dir_data_cache,
                                           size * sizeof(*new_cache) );
        else
            new_cache = RtlAllocateHeap( GetProcessHeap(), HEAP_ZERO_MEMORY, size * sizeof(*new_cache) );
        if (!new_cache) return STATUS_NO_MEMORY;
        dir_data_cache = new_cache;
        dir_data_cache_size = size;
    }

    if (!dir_data_cache[entry]) status = init_cached_dir_data( &dir_data_cache[entry], fd, mask );

    *data_ret = dir_data_cache[entry];
    return status;
}


/******************************************************************************
 *  NtQueryDirectoryFile	[NTDLL.@]
 *  ZwQueryDirectoryFile	[NTDLL.@]
 */
NTSTATUS WINAPI NtQueryDirectoryFile( HANDLE handle, HANDLE event,
                                      PIO_APC_ROUTINE apc_routine, PVOID apc_context,
                                      PIO_STATUS_BLOCK io,
                                      PVOID buffer, ULONG length,
                                      FILE_INFORMATION_CLASS info_class,
                                      BOOLEAN single_entry,
                                      PUNICODE_STRING mask,
                                      BOOLEAN restart_scan )
{
    int cwd, fd, needs_close;
    struct dir_data *data;
    NTSTATUS status;

    TRACE("(%p %p %p %p %p %p 0x%08x 0x%08x 0x%08x %s 0x%08x\n",
          handle, event, apc_routine, apc_context, io, buffer,
          length, info_class, single_entry, debugstr_us(mask),
          restart_scan);

    if (event || apc_routine)
    {
        FIXME( "Unsupported yet option\n" );
        return STATUS_NOT_IMPLEMENTED;
    }
    switch (info_class)
    {
    case FileDirectoryInformation:
    case FileBothDirectoryInformation:
    case FileFullDirectoryInformation:
    case FileIdBothDirectoryInformation:
    case FileIdFullDirectoryInformation:
    case FileIdGlobalTxDirectoryInformation:
    case FileNamesInformation:
        if (length < dir_info_align( dir_info_size( info_class, 1 ))) return STATUS_INFO_LENGTH_MISMATCH;
        break;
    case FileObjectIdInformation:
        if (length != sizeof(FILE_OBJECTID_INFORMATION)) return STATUS_INFO_LENGTH_MISMATCH;
        return STATUS_INVALID_INFO_CLASS;
    case FileQuotaInformation:
        if (length != sizeof(FILE_QUOTA_INFORMATION)) return STATUS_INFO_LENGTH_MISMATCH;
        return STATUS_INVALID_INFO_CLASS;
    case FileReparsePointInformation:
        if (length != sizeof(FILE_REPARSE_POINT_INFORMATION)) return STATUS_INFO_LENGTH_MISMATCH;
        return STATUS_INVALID_INFO_CLASS;
    default:
        return STATUS_INVALID_INFO_CLASS;
    }
    if (!buffer) return STATUS_ACCESS_VIOLATION;

    if ((status = server_get_unix_fd( handle, FILE_LIST_DIRECTORY, &fd, &needs_close, NULL, NULL )) != STATUS_SUCCESS)
        return status;

    io->Information = 0;

    RtlRunOnceExecuteOnce( &init_once, init_options, NULL, NULL );

    RtlEnterCriticalSection( &dir_section );

    cwd = open( ".", O_RDONLY );
    if (fchdir( fd ) != -1)
    {
        if (!(status = get_cached_dir_data( handle, &data, fd, mask )))
        {
            union file_directory_info *last_info = NULL;

            if (restart_scan) data->pos = 0;

            while (!status && data->pos < data->count)
            {
                status = get_dir_data_entry( data, buffer, io, length, info_class, &last_info );
                if (!status || status == STATUS_BUFFER_OVERFLOW) data->pos++;
                if (single_entry) break;
            }

            if (!last_info) status = STATUS_NO_MORE_FILES;
            else if (status == STATUS_MORE_ENTRIES) status = STATUS_SUCCESS;

            io->u.Status = status;
        }
        if (cwd == -1 || fchdir( cwd ) == -1) chdir( "/" );
    }
    else status = FILE_GetNtStatus();

    RtlLeaveCriticalSection( &dir_section );

    if (needs_close) close( fd );
    if (cwd != -1) close( cwd );
    TRACE( "=> %x (%ld)\n", status, io->Information );
    return status;
}


/***********************************************************************
 *           find_file_in_dir
 *
 * Find a file in a directory the hard way, by doing a case-insensitive search.
 * The file found is appended to unix_name at pos.
 * There must be at least MAX_DIR_ENTRY_LEN+2 chars available at pos.
 */
static NTSTATUS find_file_in_dir( char *unix_name, int pos, const WCHAR *name, int length,
                                  BOOLEAN check_case, BOOLEAN *is_win_dir )
{
    WCHAR buffer[MAX_DIR_ENTRY_LEN];
    UNICODE_STRING str;
    BOOLEAN spaces, is_name_8_dot_3;
    DIR *dir;
    struct dirent *de;
    struct stat st;
    int ret, used_default;

    /* try a shortcut for this directory */

    unix_name[pos++] = '/';
    ret = ntdll_wcstoumbs( 0, name, length, unix_name + pos, MAX_DIR_ENTRY_LEN,
                           NULL, &used_default );
    /* if we used the default char, the Unix name won't round trip properly back to Unicode */
    /* so it cannot match the file we are looking for */
    if (ret >= 0 && !used_default)
    {
        unix_name[pos + ret] = 0;
        if (!stat( unix_name, &st ))
        {
            if (is_win_dir) *is_win_dir = is_same_file( &windir, &st );
            return STATUS_SUCCESS;
        }
    }
    if (check_case) goto not_found;  /* we want an exact match */

    if (pos > 1) unix_name[pos - 1] = 0;
    else unix_name[1] = 0;  /* keep the initial slash */

    /* check if it fits in 8.3 so that we don't look for short names if we won't need them */

    str.Buffer = (WCHAR *)name;
    str.Length = length * sizeof(WCHAR);
    str.MaximumLength = str.Length;
    is_name_8_dot_3 = RtlIsNameLegalDOS8Dot3( &str, NULL, &spaces ) && !spaces;
#ifndef VFAT_IOCTL_READDIR_BOTH
    is_name_8_dot_3 = is_name_8_dot_3 && length >= 8 && name[4] == '~';
#endif

    if (!is_name_8_dot_3 && !get_dir_case_sensitivity( unix_name )) goto not_found;

    if (dcache_enabled && ret >= 0 && !used_default)
    {
        NTSTATUS status;
        if (!dc_find_file_in_dir(unix_name, pos, name, length, &status))
            return status;
        /* not cache or cache failure */
    }

    /* now look for it through the directory */

#ifdef VFAT_IOCTL_READDIR_BOTH
    if (is_name_8_dot_3)
    {
        int fd = open( unix_name, O_RDONLY | O_DIRECTORY );
        if (fd != -1)
        {
            KERNEL_DIRENT *kde;

            RtlEnterCriticalSection( &dir_section );
            if ((kde = start_vfat_ioctl( fd )))
            {
                unix_name[pos - 1] = '/';
                while (kde[0].d_reclen)
                {
                    /* make sure names are null-terminated to work around an x86-64 kernel bug */
                    size_t len = min(kde[0].d_reclen, sizeof(kde[0].d_name) - 1 );
                    kde[0].d_name[len] = 0;
                    len = min(kde[1].d_reclen, sizeof(kde[1].d_name) - 1 );
                    kde[1].d_name[len] = 0;

                    if (kde[1].d_name[0])
                    {
                        ret = ntdll_umbstowcs( 0, kde[1].d_name, strlen(kde[1].d_name),
                                               buffer, MAX_DIR_ENTRY_LEN );
                        if (ret == length && !memicmpW( buffer, name, length))
                        {
                            strcpy( unix_name + pos, kde[1].d_name );
                            RtlLeaveCriticalSection( &dir_section );
                            close( fd );
                            goto success;
                        }
                    }
                    ret = ntdll_umbstowcs( 0, kde[0].d_name, strlen(kde[0].d_name),
                                           buffer, MAX_DIR_ENTRY_LEN );
                    if (ret == length && !memicmpW( buffer, name, length))
                    {
                        strcpy( unix_name + pos,
                                kde[1].d_name[0] ? kde[1].d_name : kde[0].d_name );
                        RtlLeaveCriticalSection( &dir_section );
                        close( fd );
                        goto success;
                    }
                    if (ioctl( fd, VFAT_IOCTL_READDIR_BOTH, (long)kde ) == -1)
                    {
                        RtlLeaveCriticalSection( &dir_section );
                        close( fd );
                        goto not_found;
                    }
                }
            }
            RtlLeaveCriticalSection( &dir_section );
            close( fd );
        }
        /* fall through to normal handling */
    }
#endif /* VFAT_IOCTL_READDIR_BOTH */

    if (!(dir = opendir( unix_name )))
    {
        if (errno == ENOENT) return STATUS_OBJECT_PATH_NOT_FOUND;
        else return FILE_GetNtStatus();
    }
    unix_name[pos - 1] = '/';
    str.Buffer = buffer;
    str.MaximumLength = sizeof(buffer);
    while ((de = readdir( dir )))
    {
        ret = ntdll_umbstowcs( 0, de->d_name, strlen(de->d_name), buffer, MAX_DIR_ENTRY_LEN );
        if (ret == length && !memicmpW( buffer, name, length ))
        {
            strcpy( unix_name + pos, de->d_name );
            closedir( dir );
            goto success;
        }

        if (!is_name_8_dot_3) continue;

        str.Length = ret * sizeof(WCHAR);
        if (!RtlIsNameLegalDOS8Dot3( &str, NULL, &spaces ) || spaces)
        {
            WCHAR short_nameW[12];
            ret = hash_short_file_name( &str, short_nameW );
            if (ret == length && !memicmpW( short_nameW, name, length ))
            {
                strcpy( unix_name + pos, de->d_name );
                closedir( dir );
                goto success;
            }
        }
    }
    closedir( dir );

not_found:
    unix_name[pos - 1] = 0;
    return STATUS_OBJECT_PATH_NOT_FOUND;

success:
    if (is_win_dir && !stat( unix_name, &st )) *is_win_dir = is_same_file( &windir, &st );
    return STATUS_SUCCESS;
}


#ifndef _WIN64

static const WCHAR catrootW[] = {'s','y','s','t','e','m','3','2','\\','c','a','t','r','o','o','t',0};
static const WCHAR catroot2W[] = {'s','y','s','t','e','m','3','2','\\','c','a','t','r','o','o','t','2',0};
static const WCHAR driversstoreW[] = {'s','y','s','t','e','m','3','2','\\','d','r','i','v','e','r','s','s','t','o','r','e',0};
static const WCHAR driversetcW[] = {'s','y','s','t','e','m','3','2','\\','d','r','i','v','e','r','s','\\','e','t','c',0};
static const WCHAR logfilesW[] = {'s','y','s','t','e','m','3','2','\\','l','o','g','f','i','l','e','s',0};
static const WCHAR spoolW[] = {'s','y','s','t','e','m','3','2','\\','s','p','o','o','l',0};
static const WCHAR system32W[] = {'s','y','s','t','e','m','3','2',0};
static const WCHAR syswow64W[] = {'s','y','s','w','o','w','6','4',0};
static const WCHAR sysnativeW[] = {'s','y','s','n','a','t','i','v','e',0};
static const WCHAR regeditW[] = {'r','e','g','e','d','i','t','.','e','x','e',0};
static const WCHAR wow_regeditW[] = {'s','y','s','w','o','w','6','4','\\','r','e','g','e','d','i','t','.','e','x','e',0};

static struct
{
    const WCHAR *source;
    const WCHAR *dos_target;
    const char *unix_target;
} redirects[] =
{
    { catrootW, NULL, NULL },
    { catroot2W, NULL, NULL },
    { driversstoreW, NULL, NULL },
    { driversetcW, NULL, NULL },
    { logfilesW, NULL, NULL },
    { spoolW, NULL, NULL },
    { system32W, syswow64W, NULL },
    { sysnativeW, system32W, NULL },
    { regeditW, wow_regeditW, NULL }
};

static unsigned int nb_redirects;


/***********************************************************************
 *           get_redirect_target
 *
 * Find the target unix name for a redirected dir.
 */
static const char *get_redirect_target( const char *windows_dir, const WCHAR *name )
{
    int used_default, len, pos, win_len = strlen( windows_dir );
    char *unix_name, *unix_target = NULL;
    NTSTATUS status;

    if (!(unix_name = RtlAllocateHeap( GetProcessHeap(), 0, win_len + MAX_DIR_ENTRY_LEN + 2 )))
        return NULL;
    memcpy( unix_name, windows_dir, win_len );
    pos = win_len;

    while (*name)
    {
        const WCHAR *end, *next;

        for (end = name; *end; end++) if (IS_SEPARATOR(*end)) break;
        for (next = end; *next; next++) if (!IS_SEPARATOR(*next)) break;

        status = find_file_in_dir( unix_name, pos, name, end - name, FALSE, NULL );
        if (status == STATUS_OBJECT_PATH_NOT_FOUND && !*next)  /* not finding last element is ok */
        {
            len = ntdll_wcstoumbs( 0, name, end - name, unix_name + pos + 1,
                                   MAX_DIR_ENTRY_LEN - (pos - win_len), NULL, &used_default );
            if (len > 0 && !used_default)
            {
                unix_name[pos] = '/';
                pos += len + 1;
                unix_name[pos] = 0;
                break;
            }
        }
        if (status) goto done;
        pos += strlen( unix_name + pos );
        name = next;
    }

    if ((unix_target = RtlAllocateHeap( GetProcessHeap(), 0, pos - win_len )))
        memcpy( unix_target, unix_name + win_len + 1, pos - win_len );

done:
    RtlFreeHeap( GetProcessHeap(), 0, unix_name );
    return unix_target;
}


/***********************************************************************
 *           init_redirects
 */
static void init_redirects(void)
{
    UNICODE_STRING nt_name;
    ANSI_STRING unix_name;
    NTSTATUS status;
    struct stat st;
    unsigned int i;

    if (!RtlDosPathNameToNtPathName_U( user_shared_data->NtSystemRoot, &nt_name, NULL, NULL ))
    {
        ERR( "can't convert %s\n", debugstr_w(user_shared_data->NtSystemRoot) );
        return;
    }
    status = wine_nt_to_unix_file_name( &nt_name, &unix_name, FILE_OPEN_IF, FALSE );
    RtlFreeUnicodeString( &nt_name );
    if (status)
    {
        ERR( "cannot open %s (%x)\n", debugstr_w(user_shared_data->NtSystemRoot), status );
        return;
    }
    if (!stat( unix_name.Buffer, &st ))
    {
        windir.dev = st.st_dev;
        windir.ino = st.st_ino;
        nb_redirects = ARRAY_SIZE( redirects );
        for (i = 0; i < nb_redirects; i++)
        {
            if (!redirects[i].dos_target) continue;
            redirects[i].unix_target = get_redirect_target( unix_name.Buffer, redirects[i].dos_target );
            TRACE( "%s -> %s\n", debugstr_w(redirects[i].source), redirects[i].unix_target );
        }
    }
    RtlFreeAnsiString( &unix_name );

}


/***********************************************************************
 *           match_redirect
 *
 * Check if path matches a redirect name. If yes, return matched length.
 */
static int match_redirect( const WCHAR *path, int len, const WCHAR *redir, BOOLEAN check_case )
{
    int i = 0;

    while (i < len && *redir)
    {
        if (IS_SEPARATOR(path[i]))
        {
            if (*redir++ != '\\') return 0;
            while (i < len && IS_SEPARATOR(path[i])) i++;
            continue;  /* move on to next path component */
        }
        else if (check_case)
        {
            if (path[i] != *redir) return 0;
        }
        else
        {
            if (tolowerW(path[i]) != tolowerW(*redir)) return 0;
        }
        i++;
        redir++;
    }
    if (*redir) return 0;
    if (i < len && !IS_SEPARATOR(path[i])) return 0;
    while (i < len && IS_SEPARATOR(path[i])) i++;
    return i;
}


/***********************************************************************
 *           get_redirect_path
 *
 * Retrieve the Unix path corresponding to a redirected path if any.
 */
static int get_redirect_path( char *unix_name, int pos, const WCHAR *name, int length, BOOLEAN check_case )
{
    unsigned int i;
    int len;

    for (i = 0; i < nb_redirects; i++)
    {
        if ((len = match_redirect( name, length, redirects[i].source, check_case )))
        {
            if (!redirects[i].unix_target) break;
            unix_name[pos++] = '/';
            strcpy( unix_name + pos, redirects[i].unix_target );
            return len;
        }
    }
    return 0;
}

#else  /* _WIN64 */

/* there are no redirects on 64-bit */

static const unsigned int nb_redirects = 0;

static int get_redirect_path( char *unix_name, int pos, const WCHAR *name, int length, BOOLEAN check_case )
{
    return 0;
}

#endif

/***********************************************************************
 *           init_directories
 */
void init_directories(void)
{
#ifndef _WIN64
    if (is_wow64) init_redirects();
#endif
}


/******************************************************************************
 *           get_dos_device
 *
 * Get the Unix path of a DOS device.
 */
static NTSTATUS get_dos_device( const WCHAR *name, UINT name_len, ANSI_STRING *unix_name_ret )
{
    const char *config_dir = wine_get_config_dir();
    struct stat st;
    char *unix_name, *new_name, *dev;
    unsigned int i;
    int unix_len;

    /* make sure the device name is ASCII */
    for (i = 0; i < name_len; i++)
        if (name[i] <= 32 || name[i] >= 127) return STATUS_BAD_DEVICE_TYPE;

    unix_len = strlen(config_dir) + sizeof("/dosdevices/") + name_len + 1;

    if (!(unix_name = RtlAllocateHeap( GetProcessHeap(), 0, unix_len )))
        return STATUS_NO_MEMORY;

    strcpy( unix_name, config_dir );
    strcat( unix_name, "/dosdevices/" );
    dev = unix_name + strlen(unix_name);

    for (i = 0; i < name_len; i++) dev[i] = (char)tolowerW(name[i]);
    dev[i] = 0;

    /* special case for drive devices */
    if (name_len == 2 && dev[1] == ':')
    {
        dev[i++] = ':';
        dev[i] = 0;
    }

    for (;;)
    {
        if (!stat( unix_name, &st ))
        {
            TRACE( "%s -> %s\n", debugstr_wn(name,name_len), debugstr_a(unix_name) );
            unix_name_ret->Buffer = unix_name;
            unix_name_ret->Length = strlen(unix_name);
            unix_name_ret->MaximumLength = unix_len;
            return STATUS_SUCCESS;
        }
        if (!dev) break;

        /* now try some defaults for it */
        if (!strcmp( dev, "aux" ))
        {
            strcpy( dev, "com1" );
            continue;
        }
        if (!strcmp( dev, "prn" ))
        {
            strcpy( dev, "lpt1" );
            continue;
        }

        new_name = NULL;
        if (dev[1] == ':' && dev[2] == ':')  /* drive device */
        {
            dev[2] = 0;  /* remove last ':' to get the drive mount point symlink */
            new_name = get_default_drive_device( unix_name );
        }

        if (!new_name) break;

        RtlFreeHeap( GetProcessHeap(), 0, unix_name );
        unix_name = new_name;
        unix_len = strlen(unix_name) + 1;
        dev = NULL; /* last try */
    }
    RtlFreeHeap( GetProcessHeap(), 0, unix_name );
    return STATUS_BAD_DEVICE_TYPE;
}


/* return the length of the DOS namespace prefix if any */
static inline int get_dos_prefix_len( const UNICODE_STRING *name )
{
    static const WCHAR nt_prefixW[] = {'\\','?','?','\\'};
    static const WCHAR dosdev_prefixW[] = {'\\','D','o','s','D','e','v','i','c','e','s','\\'};

    if (name->Length >= sizeof(nt_prefixW) &&
        !memcmp( name->Buffer, nt_prefixW, sizeof(nt_prefixW) ))
        return ARRAY_SIZE( nt_prefixW );

    if (name->Length >= sizeof(dosdev_prefixW) &&
        !memicmpW( name->Buffer, dosdev_prefixW, ARRAY_SIZE( dosdev_prefixW )))
        return ARRAY_SIZE( dosdev_prefixW );

    return 0;
}


/******************************************************************************
 *           find_file_id
 *
 * Recursively search directories from the dir queue for a given inode.
 */
static NTSTATUS find_file_id( ANSI_STRING *unix_name, ULONGLONG file_id, dev_t dev )
{
    unsigned int pos;
    DIR *dir;
    struct dirent *de;
    NTSTATUS status;
    struct stat st;

    while (!(status = next_dir_in_queue( unix_name->Buffer )))
    {
        if (!(dir = opendir( unix_name->Buffer ))) continue;
        TRACE( "searching %s for %s\n", unix_name->Buffer, wine_dbgstr_longlong(file_id) );
        pos = strlen( unix_name->Buffer );
        if (pos + MAX_DIR_ENTRY_LEN >= unix_name->MaximumLength/sizeof(WCHAR))
        {
            char *new = RtlReAllocateHeap( GetProcessHeap(), 0, unix_name->Buffer,
                                           unix_name->MaximumLength * 2 );
            if (!new)
            {
                closedir( dir );
                return STATUS_NO_MEMORY;
            }
            unix_name->MaximumLength *= 2;
            unix_name->Buffer = new;
        }
        unix_name->Buffer[pos++] = '/';
        while ((de = readdir( dir )))
        {
            if (!strcmp( de->d_name, "." ) || !strcmp( de->d_name, ".." )) continue;
            strcpy( unix_name->Buffer + pos, de->d_name );
            if (lstat( unix_name->Buffer, &st ) == -1) continue;
            if (st.st_dev != dev) continue;
            if (st.st_ino == file_id)
            {
                closedir( dir );
                return STATUS_SUCCESS;
            }
            if (!S_ISDIR( st.st_mode )) continue;
            if ((status = add_dir_to_queue( unix_name->Buffer )) != STATUS_SUCCESS)
            {
                closedir( dir );
                return status;
            }
        }
        closedir( dir );
    }
    return status;
}


/******************************************************************************
 *           file_id_to_unix_file_name
 *
 * Lookup a file from its file id instead of its name.
 */
NTSTATUS file_id_to_unix_file_name( const OBJECT_ATTRIBUTES *attr, ANSI_STRING *unix_name )
{
    enum server_fd_type type;
    int old_cwd, root_fd, needs_close;
    NTSTATUS status;
    ULONGLONG file_id;
    struct stat st, root_st;

    if (attr->ObjectName->Length != sizeof(ULONGLONG)) return STATUS_OBJECT_PATH_SYNTAX_BAD;
    if (!attr->RootDirectory) return STATUS_INVALID_PARAMETER;
    memcpy( &file_id, attr->ObjectName->Buffer, sizeof(file_id) );

    unix_name->MaximumLength = 2 * MAX_DIR_ENTRY_LEN + 4;
    if (!(unix_name->Buffer = RtlAllocateHeap( GetProcessHeap(), 0, unix_name->MaximumLength )))
        return STATUS_NO_MEMORY;
    strcpy( unix_name->Buffer, "." );

    if ((status = server_get_unix_fd( attr->RootDirectory, 0, &root_fd, &needs_close, &type, NULL )))
        goto done;

    if (type != FD_TYPE_DIR)
    {
        status = STATUS_OBJECT_TYPE_MISMATCH;
        goto done;
    }

    fstat( root_fd, &root_st );
    if (root_st.st_ino == file_id)  /* shortcut for "." */
    {
        status = STATUS_SUCCESS;
        goto done;
    }

    RtlEnterCriticalSection( &dir_section );
    if ((old_cwd = open( ".", O_RDONLY )) != -1 && fchdir( root_fd ) != -1)
    {
        /* shortcut for ".." */
        if (!stat( "..", &st ) && st.st_dev == root_st.st_dev && st.st_ino == file_id)
        {
            strcpy( unix_name->Buffer, ".." );
            status = STATUS_SUCCESS;
        }
        else
        {
            status = add_dir_to_queue( "." );
            if (!status)
                status = find_file_id( unix_name, file_id, root_st.st_dev );
            if (!status)  /* get rid of "./" prefix */
                memmove( unix_name->Buffer, unix_name->Buffer + 2, strlen(unix_name->Buffer) - 1 );
            flush_dir_queue();
        }
        if (fchdir( old_cwd ) == -1) chdir( "/" );
    }
    else status = FILE_GetNtStatus();
    RtlLeaveCriticalSection( &dir_section );
    if (old_cwd != -1) close( old_cwd );

done:
    if (status == STATUS_SUCCESS)
    {
        TRACE( "%s -> %s\n", wine_dbgstr_longlong(file_id), debugstr_a(unix_name->Buffer) );
        unix_name->Length = strlen( unix_name->Buffer );
    }
    else
    {
        TRACE( "%s not found in dir %p\n", wine_dbgstr_longlong(file_id), attr->RootDirectory );
        RtlFreeHeap( GetProcessHeap(), 0, unix_name->Buffer );
    }
    if (needs_close) close( root_fd );
    return status;
}


/******************************************************************************
 *           lookup_unix_name
 *
 * Helper for nt_to_unix_file_name
 */
static NTSTATUS lookup_unix_name( const WCHAR *with_drive, const WCHAR *name, int name_len,
                                  char **buffer, int unix_len, int pos, UINT disposition, BOOLEAN check_case )
{
    NTSTATUS status;
    int ret, used_default, len;
    struct stat st;
    char *unix_name = *buffer;
    const BOOL redirect = nb_redirects && ntdll_get_thread_data()->wow64_redir;

    /* try a shortcut first */

    ret = ntdll_wcstoumbs( 0, name, name_len, unix_name + pos, unix_len - pos - 1,
                           NULL, &used_default );

    while (name_len && IS_SEPARATOR(*name))
    {
        name++;
        name_len--;
    }

    if (ret >= 0 && !used_default)  /* if we used the default char the name didn't convert properly */
    {
        char *p;
        unix_name[pos + ret] = 0;
        for (p = unix_name + pos ; *p; p++) if (*p == '\\') *p = '/';
        if (!redirect || (!strstr( unix_name, "/windows/") && strncmp( unix_name, "windows/", 8 )))
        {
            if (!stat( unix_name, &st ))
            {
                /* creation fails with STATUS_ACCESS_DENIED for the root of the drive */
                if (disposition == FILE_CREATE)
                    return name_len ? STATUS_OBJECT_NAME_COLLISION : STATUS_ACCESS_DENIED;
                return STATUS_SUCCESS;
            }
        }
    }

    if (!name_len)  /* empty name -> drive root doesn't exist */
        return STATUS_OBJECT_PATH_NOT_FOUND;
    if (check_case && !redirect && (disposition == FILE_OPEN || disposition == FILE_OVERWRITE))
        return STATUS_OBJECT_NAME_NOT_FOUND;

    /* we only use cache where roundtrip to unicode is good and path has a drive letter */
    if (dcache_enabled && with_drive && ret >= 0 && !used_default)
    {
        NTSTATUS status;
        int with_drive_len = name_len + (name - with_drive);
        if (!dc_lookup_unix_name(with_drive, with_drive_len, buffer, unix_len, disposition, &status))
            return status;
        /* not cache or cache failure */
    }

    /* now do it component by component */

    while (name_len)
    {
        const WCHAR *end, *next;
        BOOLEAN is_win_dir = FALSE;

        end = name;
        while (end < name + name_len && !IS_SEPARATOR(*end)) end++;
        next = end;
        while (next < name + name_len && IS_SEPARATOR(*next)) next++;
        name_len -= next - name;

        /* grow the buffer if needed */

        if (unix_len - pos < MAX_DIR_ENTRY_LEN + 2)
        {
            char *new_name;
            unix_len += 2 * MAX_DIR_ENTRY_LEN;
            if (!(new_name = RtlReAllocateHeap( GetProcessHeap(), 0, unix_name, unix_len )))
                return STATUS_NO_MEMORY;
            unix_name = *buffer = new_name;
        }

        status = find_file_in_dir( unix_name, pos, name, end - name,
                                   check_case, redirect ? &is_win_dir : NULL );

        /* if this is the last element, not finding it is not necessarily fatal */
        if (!name_len)
        {
            if (status == STATUS_OBJECT_PATH_NOT_FOUND)
            {
                status = STATUS_OBJECT_NAME_NOT_FOUND;
                if (disposition != FILE_OPEN && disposition != FILE_OVERWRITE)
                {
                    ret = ntdll_wcstoumbs( 0, name, end - name, unix_name + pos + 1,
                                           MAX_DIR_ENTRY_LEN, NULL, &used_default );
                    if (ret > 0 && !used_default)
                    {
                        unix_name[pos] = '/';
                        unix_name[pos + 1 + ret] = 0;
                        status = STATUS_NO_SUCH_FILE;
                        break;
                    }
                }
            }
            else if (status == STATUS_SUCCESS && disposition == FILE_CREATE)
            {
                status = STATUS_OBJECT_NAME_COLLISION;
            }
        }

        if (status != STATUS_SUCCESS) break;

        pos += strlen( unix_name + pos );
        name = next;

        if (is_win_dir && (len = get_redirect_path( unix_name, pos, name, name_len, check_case )))
        {
            name += len;
            name_len -= len;
            pos += strlen( unix_name + pos );
            TRACE( "redirecting -> %s + %s\n", debugstr_a(unix_name), debugstr_w(name) );
        }
    }

    return status;
}


/******************************************************************************
 *           nt_to_unix_file_name_attr
 */
NTSTATUS nt_to_unix_file_name_attr( const OBJECT_ATTRIBUTES *attr, ANSI_STRING *unix_name_ret,
                                    UINT disposition )
{
    static const WCHAR invalid_charsW[] = { INVALID_NT_CHARS, 0 };
    enum server_fd_type type;
    int old_cwd, root_fd, needs_close;
    const WCHAR *name, *p;
    char *unix_name;
    int name_len, unix_len;
    NTSTATUS status;
    BOOLEAN check_case = !(attr->Attributes & OBJ_CASE_INSENSITIVE);

    if (!attr->RootDirectory)  /* without root dir fall back to normal lookup */
        return wine_nt_to_unix_file_name( attr->ObjectName, unix_name_ret, disposition, check_case );

    name     = attr->ObjectName->Buffer;
    name_len = attr->ObjectName->Length / sizeof(WCHAR);

    if (name_len && IS_SEPARATOR(name[0])) return STATUS_INVALID_PARAMETER;

    /* check for invalid characters */
    for (p = name; p < name + name_len; p++)
        if (*p < 32 || strchrW( invalid_charsW, *p )) return STATUS_OBJECT_NAME_INVALID;

    unix_len = ntdll_wcstoumbs( 0, name, name_len, NULL, 0, NULL, NULL );
    unix_len += MAX_DIR_ENTRY_LEN + 3;
    if (!(unix_name = RtlAllocateHeap( GetProcessHeap(), 0, unix_len )))
        return STATUS_NO_MEMORY;
    unix_name[0] = '.';

    if (!(status = server_get_unix_fd( attr->RootDirectory, 0, &root_fd, &needs_close, &type, NULL )))
    {
        if (type != FD_TYPE_DIR)
        {
            if (needs_close) close( root_fd );
            status = STATUS_BAD_DEVICE_TYPE;
        }
        else
        {
            RtlEnterCriticalSection( &dir_section );
            if ((old_cwd = open( ".", O_RDONLY )) != -1 && fchdir( root_fd ) != -1)
            {
                status = lookup_unix_name( NULL, name, name_len, &unix_name, unix_len, 1,
                                           disposition, check_case );
                if (fchdir( old_cwd ) == -1) chdir( "/" );
            }
            else status = FILE_GetNtStatus();
            RtlLeaveCriticalSection( &dir_section );
            if (old_cwd != -1) close( old_cwd );
            if (needs_close) close( root_fd );
        }
    }
    else if (status == STATUS_OBJECT_TYPE_MISMATCH) status = STATUS_BAD_DEVICE_TYPE;

    if (status == STATUS_SUCCESS || status == STATUS_NO_SUCH_FILE)
    {
        TRACE( "%s -> %s\n", debugstr_us(attr->ObjectName), debugstr_a(unix_name) );
        unix_name_ret->Buffer = unix_name;
        unix_name_ret->Length = strlen(unix_name);
        unix_name_ret->MaximumLength = unix_len;
    }
    else
    {
        TRACE( "%s not found in %s\n", debugstr_w(name), unix_name );
        RtlFreeHeap( GetProcessHeap(), 0, unix_name );
    }
    return status;
}


/******************************************************************************
 *           wine_nt_to_unix_file_name  (NTDLL.@) Not a Windows API
 *
 * Convert a file name from NT namespace to Unix namespace.
 *
 * If disposition is not FILE_OPEN or FILE_OVERWRITE, the last path
 * element doesn't have to exist; in that case STATUS_NO_SUCH_FILE is
 * returned, but the unix name is still filled in properly.
 */
NTSTATUS CDECL wine_nt_to_unix_file_name( const UNICODE_STRING *nameW, ANSI_STRING *unix_name_ret,
                                          UINT disposition, BOOLEAN check_case )
{
    static const WCHAR unixW[] = {'u','n','i','x'};
    static const WCHAR invalid_charsW[] = { INVALID_NT_CHARS, 0 };

    NTSTATUS status = STATUS_SUCCESS;
    const char *config_dir = wine_get_config_dir();
    const WCHAR *name, *p;
    struct stat st;
    char *unix_name;
    int pos, ret, name_len, unix_len, prefix_len, used_default;
    WCHAR prefix[MAX_DIR_ENTRY_LEN];
    BOOLEAN is_unix = FALSE;
    const WCHAR *with_drive;

    name     = nameW->Buffer;
    name_len = nameW->Length / sizeof(WCHAR);

    if (!name_len || !IS_SEPARATOR(name[0])) return STATUS_OBJECT_PATH_SYNTAX_BAD;

    if (!(pos = get_dos_prefix_len( nameW )))
        return STATUS_BAD_DEVICE_TYPE;  /* no DOS prefix, assume NT native name */

    name += pos;
    name_len -= pos;

    if (!name_len) return STATUS_OBJECT_NAME_INVALID;

    with_drive = name;

    /* check for sub-directory */
    for (pos = 0; pos < name_len; pos++)
    {
        if (IS_SEPARATOR(name[pos])) break;
        if (name[pos] < 32 || strchrW( invalid_charsW, name[pos] ))
            return STATUS_OBJECT_NAME_INVALID;
    }
    if (pos > MAX_DIR_ENTRY_LEN)
        return STATUS_OBJECT_NAME_INVALID;

    if (pos == name_len)  /* no subdir, plain DOS device */
        return get_dos_device( name, name_len, unix_name_ret );

    for (prefix_len = 0; prefix_len < pos; prefix_len++)
        prefix[prefix_len] = tolowerW(name[prefix_len]);

    name += prefix_len;
    name_len -= prefix_len;

    /* check for invalid characters (all chars except 0 are valid for unix) */
    is_unix = (prefix_len == 4 && !memcmp( prefix, unixW, sizeof(unixW) ));
    if (is_unix)
    {
        for (p = name; p < name + name_len; p++)
            if (!*p) return STATUS_OBJECT_NAME_INVALID;
        check_case = TRUE;
        with_drive = NULL;
    }
    else
    {
        for (p = name; p < name + name_len; p++)
            if (*p < 32 || strchrW( invalid_charsW, *p )) return STATUS_OBJECT_NAME_INVALID;
    }

    unix_len = ntdll_wcstoumbs( 0, prefix, prefix_len, NULL, 0, NULL, NULL );
    unix_len += ntdll_wcstoumbs( 0, name, name_len, NULL, 0, NULL, NULL );
    unix_len += MAX_DIR_ENTRY_LEN + 3;
    unix_len += strlen(config_dir) + sizeof("/dosdevices/");
    if (!(unix_name = RtlAllocateHeap( GetProcessHeap(), 0, unix_len )))
        return STATUS_NO_MEMORY;
    strcpy( unix_name, config_dir );
    strcat( unix_name, "/dosdevices/" );
    pos = strlen(unix_name);

    ret = ntdll_wcstoumbs( 0, prefix, prefix_len, unix_name + pos, unix_len - pos - 1,
                           NULL, &used_default );
    if (!ret || used_default)
    {
        RtlFreeHeap( GetProcessHeap(), 0, unix_name );
        return STATUS_OBJECT_NAME_INVALID;
    }
    pos += ret;

    /* check if prefix exists (except for DOS drives to avoid extra stat calls) */

    if (prefix_len != 2 || prefix[1] != ':')
    {
        unix_name[pos] = 0;
        if (lstat( unix_name, &st ) == -1 && errno == ENOENT)
        {
            if (!is_unix)
            {
                RtlFreeHeap( GetProcessHeap(), 0, unix_name );
                return STATUS_BAD_DEVICE_TYPE;
            }
            pos = 0;  /* fall back to unix root */
        }
    }

    status = lookup_unix_name( with_drive, name, name_len, &unix_name, unix_len,
                               pos, disposition, check_case );
    if (status == STATUS_SUCCESS || status == STATUS_NO_SUCH_FILE)
    {
        TRACE( "%s -> %s\n", debugstr_us(nameW), debugstr_a(unix_name) );
        unix_name_ret->Buffer = unix_name;
        unix_name_ret->Length = strlen(unix_name);
        unix_name_ret->MaximumLength = unix_len;
    }
    else
    {
        TRACE( "%s not found in %s\n", debugstr_w(name), unix_name );
        RtlFreeHeap( GetProcessHeap(), 0, unix_name );
    }
    return status;
}


/******************************************************************
 *		RtlWow64EnableFsRedirection   (NTDLL.@)
 */
NTSTATUS WINAPI RtlWow64EnableFsRedirection( BOOLEAN enable )
{
    if (!is_wow64) return STATUS_NOT_IMPLEMENTED;
    ntdll_get_thread_data()->wow64_redir = enable;
    return STATUS_SUCCESS;
}


/******************************************************************
 *		RtlWow64EnableFsRedirectionEx   (NTDLL.@)
 */
NTSTATUS WINAPI RtlWow64EnableFsRedirectionEx( ULONG disable, ULONG *old_value )
{
    if (!is_wow64) return STATUS_NOT_IMPLEMENTED;

    __TRY
    {
        *old_value = !ntdll_get_thread_data()->wow64_redir;
    }
    __EXCEPT_PAGE_FAULT
    {
        return STATUS_ACCESS_VIOLATION;
    }
    __ENDTRY

    ntdll_get_thread_data()->wow64_redir = !disable;
    return STATUS_SUCCESS;
}


/******************************************************************
 *		RtlDoesFileExists_U   (NTDLL.@)
 */
BOOLEAN WINAPI RtlDoesFileExists_U(LPCWSTR file_name)
{
    UNICODE_STRING nt_name;
    FILE_BASIC_INFORMATION basic_info;
    OBJECT_ATTRIBUTES attr;
    BOOLEAN ret;

    if (!RtlDosPathNameToNtPathName_U( file_name, &nt_name, NULL, NULL )) return FALSE;

    attr.Length = sizeof(attr);
    attr.RootDirectory = 0;
    attr.ObjectName = &nt_name;
    attr.Attributes = OBJ_CASE_INSENSITIVE;
    attr.SecurityDescriptor = NULL;
    attr.SecurityQualityOfService = NULL;

    ret = NtQueryAttributesFile(&attr, &basic_info) == STATUS_SUCCESS;

    RtlFreeUnicodeString( &nt_name );
    return ret;
}


/***********************************************************************
 *           DIR_unmount_device
 *
 * Unmount the specified device.
 */
NTSTATUS DIR_unmount_device( HANDLE handle )
{
    NTSTATUS status;
    int unix_fd, needs_close;

    if (!(status = server_get_unix_fd( handle, 0, &unix_fd, &needs_close, NULL, NULL )))
    {
        struct stat st;
        char *mount_point = NULL;

        if (fstat( unix_fd, &st ) == -1 || !is_valid_mounted_device( &st ))
            status = STATUS_INVALID_PARAMETER;
        else
        {
            if ((mount_point = get_device_mount_point( st.st_rdev )))
            {
#ifdef __APPLE__
                static const char umount[] = "diskutil unmount >/dev/null 2>&1 ";
#else
                static const char umount[] = "umount >/dev/null 2>&1 ";
#endif
                char *cmd = RtlAllocateHeap( GetProcessHeap(), 0, strlen(mount_point)+sizeof(umount));
                if (cmd)
                {
                    strcpy( cmd, umount );
                    strcat( cmd, mount_point );
                    system( cmd );
                    RtlFreeHeap( GetProcessHeap(), 0, cmd );
#ifdef linux
                    /* umount will fail to release the loop device since we still have
                       a handle to it, so we release it here */
                    if (major(st.st_rdev) == LOOP_MAJOR) ioctl( unix_fd, 0x4c01 /*LOOP_CLR_FD*/, 0 );
#endif
                }
                RtlFreeHeap( GetProcessHeap(), 0, mount_point );
            }
        }
        if (needs_close) close( unix_fd );
    }
    return status;
}


/******************************************************************************
 *           DIR_get_unix_cwd
 *
 * Retrieve the Unix name of the current directory; helper for wine_unix_to_nt_file_name.
 * Returned value must be freed by caller.
 */
NTSTATUS DIR_get_unix_cwd( char **cwd )
{
    int old_cwd, unix_fd, needs_close;
    CURDIR *curdir;
    HANDLE handle;
    NTSTATUS status;

    RtlAcquirePebLock();

    if (NtCurrentTeb()->Tib.SubSystemTib)  /* FIXME: hack */
        curdir = &((WIN16_SUBSYSTEM_TIB *)NtCurrentTeb()->Tib.SubSystemTib)->curdir;
    else
        curdir = &NtCurrentTeb()->Peb->ProcessParameters->CurrentDirectory;

    if (!(handle = curdir->Handle))
    {
        UNICODE_STRING dirW;
        OBJECT_ATTRIBUTES attr;
        IO_STATUS_BLOCK io;

        if (!RtlDosPathNameToNtPathName_U( curdir->DosPath.Buffer, &dirW, NULL, NULL ))
        {
            status = STATUS_OBJECT_NAME_INVALID;
            goto done;
        }
        attr.Length = sizeof(attr);
        attr.RootDirectory = 0;
        attr.Attributes = OBJ_CASE_INSENSITIVE;
        attr.ObjectName = &dirW;
        attr.SecurityDescriptor = NULL;
        attr.SecurityQualityOfService = NULL;

        status = NtOpenFile( &handle, SYNCHRONIZE, &attr, &io, 0,
                             FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT );
        RtlFreeUnicodeString( &dirW );
        if (status != STATUS_SUCCESS) goto done;
    }

    if ((status = server_get_unix_fd( handle, 0, &unix_fd, &needs_close, NULL, NULL )) == STATUS_SUCCESS)
    {
        RtlEnterCriticalSection( &dir_section );

        if ((old_cwd = open(".", O_RDONLY)) != -1 && fchdir( unix_fd ) != -1)
        {
            unsigned int size = 512;

            for (;;)
            {
                if (!(*cwd = RtlAllocateHeap( GetProcessHeap(), 0, size )))
                {
                    status = STATUS_NO_MEMORY;
                    break;
                }
                if (getcwd( *cwd, size )) break;
                RtlFreeHeap( GetProcessHeap(), 0, *cwd );
                if (errno != ERANGE)
                {
                    status = STATUS_OBJECT_PATH_INVALID;
                    break;
                }
                size *= 2;
            }
            if (fchdir( old_cwd ) == -1) chdir( "/" );
        }
        else status = FILE_GetNtStatus();

        RtlLeaveCriticalSection( &dir_section );
        if (old_cwd != -1) close( old_cwd );
        if (needs_close) close( unix_fd );
    }
    if (!curdir->Handle) NtClose( handle );

done:
    RtlReleasePebLock();
    return status;
}

/************************************************************************
 *                    directory caching
 *
 * functions to reduce i/o from exhaustive search by caching known-good directories
 * a single cache uses a tree of directories which, in turn, are themselves trees
 * of files.  the tree of directories maps an NT path to unix.  the tree of files
 * maps either a long or short 8.3 NT filename to unix
 *
 * includes 6 semi-public functions:
 * - dc_init - one-time initialization on process start, if enabled
 *
 * - dc_lookup_unix_name - to be called by lookup_unix_name, after stat shortcut
 *      to avoid updating the cache for paths that can be found directly.
 *      normally, if the stat shortcut fails, the exhaustive search goes from the
 *      left most path component to the right (/foo, /foo/bar, /foo/bar/dir, ...)
 *      dc_lookup_unix_name avoids exhaustive search by checking existing fullest
 *      known-good cached directory first (starts with /foo/bar/dir).  an existing
 *      listing is updated if needed, but no new ones are created
 *
 * - dc_find_file_in_dir - to be called by find_file_in_dir, after stat shortcut
 *      to avoid unnecessarily caching paths that can be found directly.
 *      a normal exhaustive search iterates over directory until file is found
 *      with a case-insensitive comparison.  dc_find_file_in_dir creates a new
 *      listing and reads directory contents, stopping at desired file, if found
 *      if file is not found and entire directory is read, it is stamped with the
 *      directory's last modified time.  knowing this speeds future lookups, since
 *      we can know whether a file exists without reading the directory.  otherwise,
 *      if a file is not found in the cache, the disk needs to be checked
 *
 * - dc_add_file - to be called after successfully creating a file to update the cache
 *      only adds a file to the cache for a directory that has already been cached
 *      it is a no-op otherwise.  new files can be added faster than we can detect
 *      using file timestamps. a process should be aware of its own new files, so this
 *      updates the cache to keep it in sync.  files added by external files can be
 *      missed, but that is true anyway; the exhaustive search loop and nfs caching
 *      can both miss files that are added too fast
 *
 * - dc_rm_dir - to be called after successfully removing a directory
 *      removes both a listing for this directory and
 *      removes an entry in the parent, if cached.  comments above for dc_add_file
 *      about timestamps apply here, too
 *
 * - dc_rm_file - to be called after successfully removing a file
 *      Windows deletes files in 2 phases:
 *      - calls NtOpenFile with DELETE_ON_CLOSE flag
 *      - calls NtClose on resulting handle
 *      dc_rm_file queue a path to be delete with the associated handle
 *
 * - dc_rm_final - to be called after successfully closing handle
 *      removes queued path for the given handle.  no-op otherwise
 *      a file is only removed from disk once the last handle is closed
 *      so dc_rm_final only removes from cache once file is removed from disk
 *
 * - dc_mv - to be called after successfully calling glibc rename()
 *      handles both directories and files.  if old path is directory, the
 *      its listing is removed and no new listing is created; we still want
 *      to depend on dc_find_file_in_dir to cache new directories.  if old
 *      path is a file, it is removed from the old listing (if available) and
 *      added to the new listing (if available). both operations are independent
 *      of each other since we may not have both listings cached
 *
 * it's possible for the duplicate entries with different names because of either
 * symlinks or short names:
 *  C:\foobar -> $WINEPREFIX/dosdevices/c:/foobar (symlink to somewhere under z:)
 *  C:\FOOB~1 -> $WINEPREFIX/dosdevices/c:/foobar
 *  Z:/.../foobar -> $WINEPREFIX/dosdevices/z:/foobar
 * this is expected to be rare.  if it does happen, the worst consequence is
 * that extra memory is used
 */

/* these are ONLY for dfcache internal use while holding dfcache lock
   this is to avoid a deadlock between the dfcache and the virtual memory manaager
   one thread calls UnmapViewOfFile -> delete_view -> close_handle -> dc_rm_final while holding csVirtual
   another thread allocates memory inside dfcache, which calls into the vmm
   the first can't lock dfcache because second holds dfcache lock
   the second thread can't lock the vmm because the first thread holds csVirtual
   win32 heap calls should still be used for managing memory from callers */
static void *dc_alloc(SIZE_T len) __WINE_ALLOC_SIZE(1);
static inline void *dc_alloc(SIZE_T len)
{
    return malloc(len);
}

static void *dc_realloc(void *mem, SIZE_T len) __WINE_ALLOC_SIZE(2);
static inline void *dc_realloc(void *mem, SIZE_T len)
{
    return realloc(mem, len);
}

static inline void dc_free(void *mem)
{
    free(mem);
}

#include "pshpack4.h"
typedef struct
{
    WCHAR   *name;
    WCHAR   *short_name;
    int      len;
    int      size;
    struct wine_rb_entry entry;
    /* only used if short_name != NULL */
    struct wine_rb_entry entry83;
    /* name data starts here */
} dfile_t;

typedef struct dfile_del_s
{
    WCHAR   *dir;
    WCHAR   *file;
    int      is83;
    size_t   size;
    HANDLE   handle;
    struct wine_rb_entry entry;
    /* dir and file data starts here */
} dfile_del_t;

typedef struct
{
    LONG64               lastmod;   /* non-zero if entire directory is cached (faster file lookups) */
    size_t               size;      /* amount of memory entire listing with files consumes, including overhead */
    int                  selfsize;  /* size of listing itself with name data, but no files */
    struct list          mru_entry;
    struct wine_rb_entry rb_entry;
    struct wine_rb_tree  files;
    struct wine_rb_tree  files83;
    WCHAR               *nt_name;
    char                *unix_name;
    /* name data here */
} dlisting_t;

typedef struct
{
    RTL_CRITICAL_SECTION    lock;
    size_t                  used;       /* current memory used, including overhead */
    size_t                  allowed;    /* maximum amount of memory allowed, including overhead */
    struct list             mru;        /* head is MRU.  listings in tail are purged first to reclaim mem */
    struct wine_rb_tree     pending;    /* files to be deleted on close */
    struct wine_rb_tree     listings;   /* rb tree of listings, maps NT directories to unix names */
} dcache_t;
#include "poppack.h"

static inline LONG64 dc_mtime_from_stat(const struct stat *st)
{
#ifdef HAVE_STRUCT_STAT_ST_MTIM
    return (LONG64)st->st_mtim.tv_sec * 1000000000 + st->st_mtim.tv_nsec;
#else
    return (LONG64)st->st_mtime;
#endif
}

static int dfile_del_compare(const void *key, const struct wine_rb_entry *entry)
{
    const dfile_del_t *file = WINE_RB_ENTRY_VALUE(entry, dfile_del_t, entry);
    return HandleToLong(key) - HandleToLong(file->handle);
}

static int dlisting_compare(const void *_key, const struct wine_rb_entry *entry)
{
    const dlisting_t *listing = WINE_RB_ENTRY_VALUE(entry, dlisting_t, rb_entry);
    const WCHAR *key = _key;

    return strcmpiW(key, listing->nt_name);
}

/* maximum size of cache, including internal structures */
#define DC_DEF_ALLOWED        4 /* in MB */

static char dc_dosdev[PATH_MAX];
static size_t dc_dosdev_size;

static dcache_t g_dcache;

static inline void dcache_lock(void)
{
    RtlEnterCriticalSection(&g_dcache.lock);
}

static inline void dcache_unlock(void)
{
    RtlLeaveCriticalSection(&g_dcache.lock);
}

static inline void dfile_free(dfile_t *file)
{
    dc_free(file);
}

static void dfile_free_entry_rb(struct wine_rb_entry *entry, void *context)
{
    dfile_t *file;

    file = WINE_RB_ENTRY_VALUE(entry, dfile_t, entry);
    dfile_free(file);
}

static inline int dc_is_8dot3(const WCHAR *name, int len)
{
    BOOLEAN spaces;
    UNICODE_STRING str;
    int is_name_8_dot_3;

    str.Buffer = (WCHAR*)name;
    str.Length = len * sizeof(WCHAR);
    str.MaximumLength = str.Length;

    is_name_8_dot_3 = RtlIsNameLegalDOS8Dot3(&str, NULL, &spaces) && !spaces;
#ifndef VFAT_IOCTL_READDIR_BOTH
    is_name_8_dot_3 = is_name_8_dot_3 && len >= 8 && name[4] == '~';
#endif
    return is_name_8_dot_3;
}

static inline int dc_umbstowcs(const char *unix_path, int unix_len, WCHAR *nt_path, int *nt_len)
{
    int len;

    if ((len = ntdll_umbstowcs(0, unix_path, unix_len, nt_path, *nt_len)) < 0)
        return ENAMETOOLONG;
    nt_path[len] = 0;
    *nt_len = len;
    return 0;
}

/* converts a path to wide-char, allocating a buffer if needed that caller must free */
static inline int dc_convert_path(const char *unix_path, int unix_len, WCHAR **pnt_path, int *pnt_len)
{
    int nt_len;
    WCHAR *nt_path;

    /* in the common case, the converted path will fit in the given buffer */
    nt_path = *pnt_path;
    if (dc_umbstowcs(unix_path, unix_len, nt_path, pnt_len))
    {
        /* must be a long path - get correct size */
        if ((nt_len = ntdll_umbstowcs(0, unix_path, unix_len, NULL, 0)) < 0)
            return ENAMETOOLONG;

        if (!(nt_path = dc_alloc((nt_len + 1) * sizeof(WCHAR))))
            return ENOMEM;

        dc_umbstowcs(unix_path, unix_len, nt_path, &nt_len);
        *pnt_path = nt_path;
        *pnt_len = nt_len;
    }

    return 0;
}

/* we only accept unix paths from dosdevices
   if the path is valid, skip to the drive letter */
static inline char *dc_drive_letter_from_unix(const char *full_unix)
{
    char *unix_path;

    if (!(unix_path = strstr(full_unix, dc_dosdev)))
        return NULL;

    if (unix_path != full_unix)
        return NULL;

    /* skip path to dosdevices */
    unix_path += dc_dosdev_size;
    return unix_path;
}

/* copy path and return size */
static inline int dc_strncpyW(WCHAR *dst, const WCHAR *src, int src_len)
{
    WCHAR *orig;

    orig = dst;
    while (src_len--) { *dst++ = *src++; }
    *dst = 0;

    return dst - orig;
}

/* assumes normalized path.  splits given path in-place into directory and file */
static inline int dc_split_path(WCHAR *path, int len, int *dir_len, WCHAR **file, int *file_len)
{
    WCHAR *slash;

    /* skip to last separator.  the unix path already has normalized slashes */
    if (!(slash = memrchrW(path, '/', len)))
        return EINVAL;

    /* NULL terminate dir and get file */
    *dir_len = slash - path;
    *slash++ = 0;
    *file = slash;
    *file_len = len - *dir_len - 1;
    return 0;
}

/* allocate file structure to add to listing.  creates short path if needed
   paths are stored at the end of the structure */
static int dfile_new(const WCHAR *name, int len, dfile_t **pfile)
{
    size_t size;
    int short_len;
    dfile_t *file;
    UNICODE_STRING str;
    WCHAR short_name[13];
    const size_t size_no_short_name = sizeof(dfile_t) - sizeof(struct wine_rb_entry); /* subtract entry83 */

    *pfile = NULL;

    if (dc_is_8dot3(name, len))
    {
        /* name is already short - don't need to create one */
        short_len = 0;
        size = size_no_short_name;
    }
    else
    {
        str.Buffer = (WCHAR*)name;
        str.Length = len * sizeof(WCHAR);
        str.MaximumLength = str.Length;
        short_len = hash_short_file_name(&str, short_name);
        size = sizeof(dfile_t) + (short_len + 1) * sizeof(WCHAR);
    }
    size += (len + 1) * sizeof(WCHAR);

    if (!(file = dc_alloc(size)))
        return ENOMEM;

    if (!short_len)
    {
        file->name       = (WCHAR*)((char*)file + size_no_short_name);
        file->short_name = NULL;
    }
    else
    {
        file->name       = (WCHAR*)((char*)file + sizeof(dfile_t));
        file->short_name = file->name + len + 1;

        memcpy(file->short_name, short_name, short_len * sizeof(WCHAR));
        file->short_name[short_len] = 0;
    }

    memcpy(file->name, name, len * sizeof(WCHAR));
    file->name[len] = 0;

    file->len = len;
    file->size = size;
    *pfile = file;

    return 0;
}

/* mark a listing as out of date and free current tree of files */
static inline void dlisting_invalidate(dlisting_t *listing)
{
    listing->lastmod = 0;
    listing->size = listing->selfsize;
    wine_rb_destroy(&listing->files83, NULL, NULL); /* memory deleted below */
    wine_rb_destroy(&listing->files, dfile_free_entry_rb, NULL);
}

static inline void dc_remove_listing(dlisting_t *listing)
{
    g_dcache.used -= listing->size;
    wine_rb_remove(&g_dcache.listings, &listing->rb_entry);
    list_remove(&listing->mru_entry);
    dlisting_invalidate(listing);
    dc_free(listing);
}

static void dlisting_remove_file(dlisting_t *listing, dfile_t *file)
{
    listing->size -= file->size;
    g_dcache.used -= file->size;
    wine_rb_remove(&listing->files, &file->entry);
    if (file->short_name)
        wine_rb_remove(&listing->files83, &file->entry83);
    dfile_free(file);
}

static void dc_remove_dfile_del(dfile_del_t *del)
{
    wine_rb_remove(&g_dcache.pending, &del->entry);
    g_dcache.used -= del->size;
    dc_free(del);
}

/* ensure a path is still accessible and actually a directory */
static inline int dc_valid_dir(const char *unix_name, struct stat *st)
{
    if (stat(unix_name, st))
        return errno;

    if (!S_ISDIR(st->st_mode))
        return ENOTDIR;

    return 0;
}

/* copy at most dstlen chars, stopping earlier if needed (strncpy zero-fills the dst) */
static size_t dc_strlcpy(char *dst, const char *src, size_t dstlen)
{
    size_t len;

    len = 0;
    while (dstlen && *src)
    {
        *dst++ = *src++;
        dstlen--;
        len++;
    }
    while (*src) { src++; len++; }
    if (dstlen) *dst = 0;

    return len;
}

/* safely build a unix path */
static inline int dc_build_path(const dlisting_t *listing, const dfile_t *file, char *unix_path, int unix_len)
{
    int len;
    int used_default;

    if ((len = dc_strlcpy(unix_path, listing->unix_name, unix_len)) >= unix_len)
        return ENAMETOOLONG;
    unix_len -= len;
    unix_path += len;
    if ((len = dc_strlcpy(unix_path, "/", unix_len)) >= unix_len)
        return ENAMETOOLONG;
    unix_len -= len;
    unix_path += len;
    if ((len = ntdll_wcstoumbs(0, file->name, file->len, unix_path, unix_len, NULL, &used_default)) <= 0)
        return ENAMETOOLONG;
    unix_path[len] = 0;

    return 0;
}

/* make sure timestamp for listing matches disk */
static inline int dlisting_consistency_check(dlisting_t *listing)
{
    struct stat st;
    int rc;

    if (!listing->lastmod)
        return 0; /* already considered out of date */

    if ((rc = dc_valid_dir(listing->unix_name, &st)))
        return rc;

    if (listing->lastmod != dc_mtime_from_stat(&st))
        listing->lastmod = 0; /* cache out of date */

    return 0;
}

/* make sure timestamps for listing and file match dist */
static int dlisting_consistency_check_file(dlisting_t *listing, dfile_t *file)
{
    char unix_path[PATH_MAX];
    struct stat st;
    LONG64 tdir;
    int rc;

    if (!listing->lastmod)
        return 0;

    if ((rc = dc_valid_dir(listing->unix_name, &st)))
    {
        /* listing suddenly doesn't exist on disk */
        return rc;
    }

    tdir = dc_mtime_from_stat(&st);
    if (listing->lastmod != tdir)
    {
        /* cache out of date */
        listing->lastmod = 0;
        return 0;
    }

    if ((rc = dc_build_path(listing, file, unix_path, sizeof(unix_path))))
        return rc;

    if (stat(unix_path, &st))
        return errno; /* file suddenly doesn't exist on disk */

    if (tdir != dc_mtime_from_stat(&st))
        listing->lastmod = 0; /* file time doesn't match listing */

    return 0;
}

static inline dfile_t *dlisting_find_file(dlisting_t *listing, const WCHAR *want, int is83)
{
    struct wine_rb_entry *entry;

    if ((entry = wine_rb_get(&listing->files, want)))
        return WINE_RB_ENTRY_VALUE(entry, dfile_t, entry);

    if (is83)
        if ((entry = wine_rb_get(&listing->files83, want)))
            return WINE_RB_ENTRY_VALUE(entry, dfile_t, entry83);

    return NULL;
}

static int dlisting_put_file(dlisting_t *listing, const WCHAR *name, int len, int is83, dfile_t **pfile)
{
    dfile_t *file;
    int rc;

    /* see if current file is already added */
    if ((file = dlisting_find_file(listing, name, is83)))
    {
        /* it's extremely rare, but the unix name could just change case
           and it would have the same key in the tree.  since we are
           case-preserving, update the name just in case */
        memcpy(file->name, name, len * sizeof(WCHAR));
    }
    else
    {
        if ((rc = dfile_new(name, len, &file)))
            return rc;

        if (file->short_name)
        {
            /* it's extremely rare, but we could collide on the short name
               in this case, just keep the first one we added.  this is
               the same as an exhaustive search which returns first found */
            if (wine_rb_put(&listing->files83, file->short_name, &file->entry83))
            {
                WARN("collision %s %s\n", debugstr_w(file->name), debugstr_w(file->short_name));

                /* NULL the short_name pointer so we don't try to remove it from the tree later
                   we waste a small amount of memory but shouldn't matter much since it's so rare */
                file->short_name = NULL;
            }
        }

        wine_rb_put(&listing->files, file->name, &file->entry);
        listing->size += file->size;
    }

    *pfile = file;
    return 0;
}

static inline dlisting_t *dc_find_listing(const WCHAR *dir)
{
    struct wine_rb_entry *entry;
    dlisting_t *listing;

    if (!(entry = wine_rb_get(&g_dcache.listings, dir)))
        return NULL;

    listing = WINE_RB_ENTRY_VALUE(entry, dlisting_t, rb_entry);

    /* move the listing up the MRU */
    list_remove(&listing->mru_entry);
    list_add_head(&g_dcache.mru, &listing->mru_entry);

    return listing;
}

/* free up memory from cache by first removing any stale entries
   keep freeing until we hit the threshold */
static void dc_purge(void)
{
    const size_t threshold = g_dcache.allowed / 2;
    struct wine_rb_entry *entry;
    dlisting_t *listing;
    dfile_del_t *del;
    struct list *next;
    struct list *cur;
    struct stat st;

    TRACE("purging %zu / %zu\n", g_dcache.used, g_dcache.allowed);

    /* free up all invalid listings first */
    LIST_FOR_EACH_SAFE(cur, next, &g_dcache.mru)
    {
        listing = LIST_ENTRY(cur, dlisting_t, mru_entry);
        if ((dc_valid_dir(listing->unix_name, &st)))
            dc_remove_listing(listing);
    }

    /* remove any pending files for listings that no longer exist */
    entry = wine_rb_head(g_dcache.pending.root);
    while (entry)
    {
        del = WINE_RB_ENTRY_VALUE(entry, dfile_del_t, entry);
        entry = wine_rb_next(entry);

        if (!(listing = dc_find_listing(del->dir)))
            dc_remove_dfile_del(del);
    }

    TRACE("updated cache size %zu / %zu\n", g_dcache.used, g_dcache.allowed);

    if (g_dcache.used < threshold)
        return;

    /* free listings from tail until enough is free */
    LIST_FOR_EACH_SAFE_REV(cur, next, &g_dcache.mru)
    {
        if (g_dcache.used < threshold)
            break;

        listing = LIST_ENTRY(cur, dlisting_t, mru_entry);
        dc_remove_listing(listing);
    }

    /* remove any pending files for listings that no longer exist */
    entry = wine_rb_head(g_dcache.pending.root);
    while (entry)
    {
        del = WINE_RB_ENTRY_VALUE(entry, dfile_del_t, entry);
        entry = wine_rb_next(entry);

        if (!(listing = dc_find_listing(del->dir)))
            dc_remove_dfile_del(del);
    }

    TRACE("updated cache size %zu / %zu\n", g_dcache.used, g_dcache.allowed);
}

void dc_init(void)
{
    char *var;
    size_t allowed;
    const char *config_dir = wine_get_config_dir();

    if (((var = getenv("DFCACHE_DISABLE")) && (!strcasecmp(var, "true") || atoi(var))))
    {
        TRACE("dfcache disabled\n");
        return;
    }

    allowed = DC_DEF_ALLOWED;
    if ((var = getenv("DFCACHE_MAX_SIZE"))) /* in MB */
    {
        long val;

        if ((val = atol(var)) < 1)
        {
            TRACE("dfcache disabled (requested size too small %ld)\n", val);
            return;
        }

        allowed = val;
    }
    allowed *= 1024 * 1024;

    TRACE("dfcache enabled with %zu MB\n", allowed / 1024 / 1024);
    RtlInitializeCriticalSection(&g_dcache.lock);
    g_dcache.used = sizeof(g_dcache);
    g_dcache.allowed = allowed;
    list_init(&g_dcache.mru);
    wine_rb_init(&g_dcache.pending, dfile_del_compare);
    wine_rb_init(&g_dcache.listings, dlisting_compare);

    strcpy(dc_dosdev, config_dir);
    strcat(dc_dosdev, "/dosdevices/");
    dc_dosdev_size = strlen(dc_dosdev);

    dcache_enabled = TRUE;
}

static int dfile_short_compare(const void *_key, const struct wine_rb_entry *entry)
{
    const dfile_t *node = WINE_RB_ENTRY_VALUE(entry, dfile_t, entry83);
    const WCHAR *key = _key;
    return strcmpiW(key, node->short_name);
}

static int dfile_long_compare(const void *_key, const struct wine_rb_entry *entry)
{
    const dfile_t *node = WINE_RB_ENTRY_VALUE(entry, dfile_t, entry);
    const WCHAR *key = _key;
    return strcmpiW(key, node->name);
}

/* dfcache can't properly handle short names from a VFAT directory
   if this returns true, we should fall back so exhaustive search for the given path */
static BOOL dc_is_vfat(const char *unix_name)
{
    KERNEL_DIRENT *kde;
    BOOL is_vfat;
    int fd;

    if ((fd = open(unix_name, O_RDONLY | O_DIRECTORY)) == -1)
        return FALSE;

    RtlEnterCriticalSection(&dir_section);
    is_vfat = (kde = start_vfat_ioctl(fd)) ? TRUE : FALSE;
    RtlLeaveCriticalSection(&dir_section);

    close(fd);
    return is_vfat;
}

/* refreshes the files in a listing by re-reading the directory
   any files previously cached are freed, so they don't accumulate
   stops reading early if the file we want is found */
static int dc_read_listing(dlisting_t *listing, const WCHAR *want, dfile_t **pfile)
{
    int rc;
    int is83;
    DIR *dir;
    int unix_len;
    int long_len;
    dfile_t *file;
    struct stat st;
    char *unix_name;
    struct dirent *de;
    WCHAR long_name[MAX_DIR_ENTRY_LEN+1];

    *pfile = NULL;
    dlisting_invalidate(listing);
    if (!(dir = opendir(listing->unix_name)))
        return errno;

    /* opendir will refresh cached attributes */
    if (stat(listing->unix_name, &st))
    {
        rc = errno;
        closedir(dir);
        return rc;
    }

    rc = 0;
    is83 = dc_is_8dot3(want, strlenW(want));
    while ((de = readdir(dir)))
    {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;

        unix_name = de->d_name;
        unix_len = strlen(unix_name);
        long_len = sizeof(long_name)/sizeof(long_name[0]);
        if ((rc = dc_umbstowcs(unix_name, unix_len, long_name, &long_len)))
            break;

        file = NULL;
        if ((rc = dlisting_put_file(listing, long_name, long_len, FALSE, &file)))
            break;

        if ((file = dlisting_find_file(listing, want, is83)))
        {
            *pfile = file;
            break;
        }
    }
    closedir(dir);

    /* if we read the whole directory without error, mark it as complete */
    if (!de && !rc) listing->lastmod = dc_mtime_from_stat(&st);

    return rc;
}

/* caller must add to cache */
static int dlisting_new(const WCHAR *dir, const char *unix_path, dlisting_t **plisting)
{
    int rc;
    int dir_len;
    int selfsize;
    struct stat st;
    size_t unix_len;
    dlisting_t *listing;
    const BOOL redirect = nb_redirects && ntdll_get_thread_data()->wow64_redir;

    if ((rc = dc_valid_dir(unix_path, &st)))
        return rc;

    /* don't cache if Windows path (redirected on 32-bit WoW64) */
    if (redirect && is_same_file(&windir, &st))
        return EINVAL;

    /* don't cache vfat paths because we don't handle its short names */
    if (dc_is_vfat(unix_path))
        return EINVAL;

    unix_len = strlen(unix_path);
    dir_len = strlenW(dir);
    selfsize = sizeof(*listing) + unix_len + 1 + (dir_len + 1) * sizeof(WCHAR);
    if (!(listing = dc_alloc(selfsize)))
        return ENOMEM;

    listing->lastmod = 0;
    listing->size = selfsize;
    listing->selfsize = selfsize;

    wine_rb_init(&listing->files, dfile_long_compare);
    wine_rb_init(&listing->files83, dfile_short_compare);

    listing->nt_name = (WCHAR*)((char*)listing + sizeof(*listing));
    memcpy(listing->nt_name, dir, dir_len * sizeof(WCHAR));
    listing->nt_name[dir_len] = 0;

    listing->unix_name = (char*)(listing->nt_name + dir_len + 1);
    memcpy(listing->unix_name, unix_path, unix_len);
    listing->unix_name[unix_len] = 0;

    *plisting = listing;
    return 0;
}

/* convert any slashes to '/' and remove duplicate and trailing slashes
   assumes src starts at drive letter */
static int dc_normalize_slashes(WCHAR *dst, const WCHAR *src, int srclen)
{
    WCHAR *orig_dst;

    for (orig_dst = dst; srclen > 0; srclen--, src++)
    {
        if (IS_SEPARATOR(*src))
        {
            if (dst == orig_dst || dst[-1] != '/')
                *dst++ = '/';
        }
        else
            *dst++ = *src;
    }

    /* remove trailing / */
    if (dst[-1] == '/')
        --dst;

    *dst = '\0';
    return dst - orig_dst;
}

static inline BOOL dc_is_drive(const WCHAR *path)
{
    return path[1] == ':' && IS_SEPARATOR(path[2]) &&
           ((path[0] >= 'a' && path[0] <= 'z') || (path[0] >= 'A' && path[0] <= 'Z'));
}

/* searches the listing for a filename.  checks for consistency and updates cache
   by re-reading directory if needed */
static int dc_find_file_in_cache(dlisting_t *listing, const WCHAR *want, dfile_t **pfile)
{
    size_t old_size;
    struct stat st;
    dfile_t *file;
    LONG64 disk;
    int is83;
    int rc;

    *pfile = NULL;
    is83 = dc_is_8dot3(want, strlenW(want));
    if ((file = dlisting_find_file(listing, want, is83)))
    {
        *pfile = file;
        return 0;
    }

    /* file not in cache - check if dir still exists */
    if ((rc = dc_valid_dir(listing->unix_name, &st)))
        return rc;

    /* listing path is valid */
    disk = dc_mtime_from_stat(&st);
    if (listing->lastmod == disk)
    {
        /* file not in cache that is current and complete */
        return 0;
    }

    /* listing path is valid but cache is out of date and/or incomplete */
    file = NULL;
    old_size = listing->size; /* updating listing might change size */
    if ((rc = dc_read_listing(listing, want, &file)))
    {
        /* when caller removes it, it should subtract the original size */
        listing->size = old_size;
        return rc;
    }

    /* listing path valid and now up to date.  update cache size */
    g_dcache.used += listing->size - old_size;
    *pfile = file; /* can be NULL if file not found */

    return 0;
}

/* assumes dcache lock is held */
static int dc_lookup_unix_name_nl(WCHAR *nt, int nt_len, char *unix_path, int unix_len,
                                  UINT disposition, NTSTATUS *status)
{
    dlisting_t *listing;
    dfile_t *dfile;
    struct stat st;
    WCHAR *file;
    WCHAR *dir;
    size_t len;
    int used_default;
    int file_len;
    int dir_len;
    int ret;
    int rc;

    /* try full path as-is */
    if ((listing = dc_find_listing(nt)))
    {
        /* make sure path still exists */
        if (!(rc = dc_valid_dir(listing->unix_name, &st)))
        {
            /* only build a unix path if we're not creating the file
               so we don't have a name collision */
            if (disposition == FILE_CREATE)
                *status = STATUS_OBJECT_NAME_COLLISION;
            else
            {
                if ((len = dc_strlcpy(unix_path, listing->unix_name, unix_len)) >= unix_len)
                {
                    dc_remove_listing(listing);
                    return ENAMETOOLONG;
                }
                *status = STATUS_SUCCESS;
            }
            return 0;
        }

        /* remove listing for path that no longer exists and
           fall-through to see if we can build path using parent dir */
        dc_remove_listing(listing);
    }

    if ((rc = dc_split_path(nt, nt_len, &dir_len, &file, &file_len)))
        return rc;
    dir = nt;

    /* try looking for file part in parent dir */
    if (!(listing = dc_find_listing(dir)))
        return ENOENT;

    dfile = NULL;
    if ((rc = dc_find_file_in_cache(listing, file, &dfile)))
    {
        /* failure to update listing.  remove for now and
           fall back to exhaustive search */
        dc_remove_listing(listing);
        return rc;
    }

    /* dc_find_file_in_cache refreshes the listing from disk if the file
       is not found in the cache.  so if we have a NULL dfile here, we can
       be sure the file really doesn't exist.  if we do have a dfile, then
       it came from the in-memory cache and may no longer exist on disk */

    if (dfile)
    {
        if ((rc = dc_build_path(listing, dfile, unix_path, unix_len)))
        {
            dc_remove_listing(listing);
            return rc;
        }

        if (stat(unix_path, &st))
        {
            /* file is in cache but no longer on disk */
            rc = errno;

            /* remove entire listing if no longer valid
               if still valid, mark it incomplete */
            if (!dc_valid_dir(listing->unix_name, &st))
                listing->lastmod = 0;
            else
                dc_remove_listing(listing);

            return rc;
        }

        /* unix path was valid and on-disk */
        if (disposition == FILE_CREATE)
            *status = STATUS_OBJECT_NAME_COLLISION;
        else
            *status = STATUS_SUCCESS;
        return 0;
    }

    /* we didn't find a file, so it doesn't exist in cache or on disk */
    if (disposition == FILE_OPEN || disposition == FILE_OVERWRITE)
    {
        *status = STATUS_OBJECT_NAME_NOT_FOUND; /* file MUST exist */
        return 0;
    }

    /* not finding a file isn't fatal if we're going to create it */
    if ((len = dc_strlcpy(unix_path, listing->unix_name, unix_len)) >= unix_len)
        return ENAMETOOLONG;

    ret = ntdll_wcstoumbs(0, file, file_len, &unix_path[len+1],
                          MAX_DIR_ENTRY_LEN, NULL, &used_default);
    if (ret <= 0 || used_default)
        return ENAMETOOLONG;

    unix_path[len] = '/';
    unix_path[len + 1 + ret] = 0;
    *status = STATUS_NO_SUCH_FILE;
    return 0;
}

/* uses cache to find directory or file
   returning an error means caller should fall back to exchaustive search
   returning 0 mean *status will be set, whether given path is found or not:
   - SUCCESS                - file/dir is found
   - OBJECT_NAME_COLLISION  - file/dir exists, but shouldn't
   - OBJECT_PATH_NOT_FOUND  - directory doesn't exist
   - NO_SUCH_FILE           - file doesn't exist, but not fatal
   - OBJECT_NAME_NOT_FOUND  - file exists, but fatal
   a unix path is returned if status is SUCCESS or NO_SUCH_FILE
   unix name buffer will be reallocated if needed
*/
int dc_lookup_unix_name(const WCHAR *nt_name, int nt_len, char **punix_name, int unix_len,
                        UINT disposition, NTSTATUS *status)
{
    char unix_path[PATH_MAX];
    WCHAR norm_stack[MAX_PATH+1]; /* original case, normalized slashes */
    WCHAR *norm = norm_stack;
    int norm_len;
    size_t len;
    char *tmp;
    int rc;

    /* omit any trailing slashes */
    while ((nt_len > 0) && IS_SEPARATOR(nt_name[nt_len-1])) nt_len--;

    /* only accept paths that start with drive letters */
    if (nt_len < 3 || !dc_is_drive(nt_name))
        return EINVAL;

    if ((nt_len > MAX_PATH) &&
        !(norm = dc_alloc((nt_len + 1) * sizeof(WCHAR))))
    {
        return ENOMEM;
    }

    /* assume path not found */
    unix_path[0] = 0;

    /* normalize separators before doing lookup */
    if ((norm_len = dc_normalize_slashes(norm, nt_name, nt_len)))
    {
        dcache_lock();
        rc = dc_lookup_unix_name_nl(norm, norm_len, unix_path, unix_len, disposition, status);
        dcache_unlock();
    }
    else rc = EINVAL;

    if (norm != norm_stack)
        dc_free(norm);

    if (rc)
    {
        TRACE("%d %s\n", rc, debugstr_wn(nt_name, nt_len));
        return rc;
    }

    /* we have a unix path to a file that either exists and we want to open
       or doesn't exist but we want to create it */
    if (unix_path[0] || (*status == STATUS_SUCCESS || *status == STATUS_NO_SUCH_FILE))
    {
        /* for most paths, lookup_unix_name will allocate enough memory
           for the resolved unix path.  but redirects may need more room */
        len = strlen(unix_path);
        if (len > unix_len)
        {
            /* NOTE: use Wine heap here and outside of dfcache lock */
            if (!(tmp = RtlReAllocateHeap(GetProcessHeap(), 0, *punix_name, len+1)))
            {
                TRACE("%d %s\n", ENOMEM, debugstr_wn(nt_name, nt_len));
                return ENOMEM;
            }
            *punix_name = tmp;
        }
        memcpy(*punix_name, unix_path, len);
        (*punix_name)[len] = 0;
    }

    TRACE("%x %s -> %s\n", *status, debugstr_wn(nt_name, nt_len), debugstr_a(*punix_name));

    return 0;
}

/* assumes dcache lock is held */
static int dc_find_file_in_dir_nl(dlisting_t *listing, char *unix_name, int pos,
                                  const WCHAR *file, int file_len, NTSTATUS *status)
{
    WCHAR want[MAX_DIR_ENTRY_LEN+1];
    int used_default;
    struct stat st;
    dfile_t *dfile;
    int ret;
    int rc;

    dfile = NULL;
    dc_strncpyW(want, file, file_len);
    if ((rc = dc_find_file_in_cache(listing, want, &dfile)))
        return rc;

    /* not finding a file isn't a fatal error */
    if (!dfile)
        return 0;

    /* we may have fetched the filename from in-memory cache
       make sure file still exists on disk
       find_file_in_dir will set unix_name[pos-1] = \0 */
    unix_name[pos - 1] = '/';
    ret = ntdll_wcstoumbs(0, dfile->name, dfile->len, unix_name + pos,
                          MAX_DIR_ENTRY_LEN, NULL, &used_default);
    unix_name[pos + ret] = 0;

    if (!stat(unix_name, &st))
    {
        *status = STATUS_SUCCESS;
        return 0;
    }

    /* file no longer exists.  restore path for caller
       and mark listing out of date */
    unix_name[pos - 1] = 0;
    listing->lastmod = 0;
    return errno;
}

/* uses cache to find file in given unix directory
   returning an error means caller should fall back to exhaustive search
   returning 0 means status will be set:
   SUCCESS               - file found and filename appended
   OBJECT_PATH_NOT_FOUND - file not found and filename not appended
   like find_file_in_dir, assumes MAX_DIR_ENTRY_LEN+2 bytes available at pos */
int dc_find_file_in_dir(char *unix_name, int pos, const WCHAR *file, int file_len, NTSTATUS *status)
{
    WCHAR dir_stack[MAX_PATH+1];
    WCHAR *dir = dir_stack;
    dlisting_t *listing;
    int dir_len;
    char *cur;
    int rc;

    /* only accept paths from $WINEPREFIX where a filename is specified */
    if (pos < 1 || !file_len || file_len >= MAX_DIR_ENTRY_LEN)
        return EINVAL;

    if (!(cur = dc_drive_letter_from_unix(unix_name)))
        return EINVAL;

    dir_len = sizeof(dir_stack)/sizeof(dir_stack[0]);
    if ((rc = dc_convert_path(cur, strlen(cur), &dir, &dir_len)))
        return rc;

    /* converted unix path already has normalized slashes */

    *status = STATUS_OBJECT_PATH_NOT_FOUND;
    dcache_lock();
    if (!(listing = dc_find_listing(dir)))
    {
        /* NT paths are stored starting from the drive letter
           but the unix paths associated with the listings include $WINEPREFIX/dosdevices/ */
        if (!(rc = dlisting_new(dir, unix_name, &listing)))
        {
            wine_rb_put(&g_dcache.listings, dir, &listing->rb_entry);
            g_dcache.used += listing->size;
            list_add_head(&g_dcache.mru, &listing->mru_entry);
        }

        /* fall-through, populate and search listing */
    }

    if (listing)
    {
        /* if the file is found in the listing, it is copied to unix_name at pos */
        if ((rc = dc_find_file_in_dir_nl(listing, unix_name, pos, file, file_len, status)))
            dc_remove_listing(listing);

        if (g_dcache.used > g_dcache.allowed)
            dc_purge();
    }
    else
        rc = ENOENT;
    dcache_unlock();

    if (dir != dir_stack)
        dc_free(dir);

    TRACE("%x %s -> %s\n", *status, debugstr_wn(file, file_len),
          *status == STATUS_SUCCESS ? unix_name : "");

    return rc;
}

/* adds file to a listing.  assumes dcache lock is held */
static int dc_add_file_w_nl(dlisting_t *listing, const WCHAR *file, int file_len, int is83)
{
    size_t old_size;
    dfile_t *dfile;
    int rc;

    /* even if we support long paths, the components must be the right size */
    if (file_len > MAX_DIR_ENTRY_LEN)
        return ENAMETOOLONG;

    dfile = NULL;
    old_size = listing->size;
    if ((rc = dlisting_put_file(listing, file, file_len, is83, &dfile)))
        return rc;

    /* update the size, but only if we added it */
    g_dcache.used += listing->size - old_size;
    if (listing->lastmod)
    {
        /* if we added or updated a file, the directory mtime will be newer
           if the listing is already up to date and complete, we should update the mtime
           so that future search benefit from it being complete.  mtime should be the
           same as the file just added.  otherwise, something else modified it */
        rc = dlisting_consistency_check_file(listing, dfile);
    }

    return rc;
}

/* adds wide-char path to cache */
static int dc_add_file_w(WCHAR *nt, int nt_len)
{
    dlisting_t *listing;
    WCHAR *file;
    WCHAR *dir;
    int file_len;
    int dir_len;
    int is83;
    int rc;

    dir = nt;
    file = NULL;
    dir_len = 0;
    file_len = 0;
    if ((rc = dc_split_path(dir, nt_len, &dir_len, &file, &file_len)))
        return rc;

    is83 = dc_is_8dot3(file, file_len);

    /* we only want to update an existing listing */
    dcache_lock();
    if ((listing = dc_find_listing(dir)))
    {
        if ((rc = dc_add_file_w_nl(listing, file, file_len, is83)))
        {
            /* on any error, remove the entire listing this would be added to */
            dc_remove_listing(listing);
        }

        if (g_dcache.used > g_dcache.allowed)
            dc_purge();
    }
    else rc = ENOENT;
    dcache_unlock();

    return rc;
}

/* to be called by FILE_Create after a new file has been created
   paths have already been checked by wine_nt_to_unix_name (possibly dfcache)
   NT names should be \??\C:\foo\bar
   unix names should be $WINEPREFIX/dosdevices/c:/foo/bar
   the case of the unix path is known to be correct */
int dc_add_file(const char *full_unix)
{
    WCHAR nt_stack[MAX_PATH+1];
    WCHAR *nt = nt_stack;
    char *unix_path;
    size_t unix_len;
    int nt_len;
    int rc;

    if (!(unix_path = dc_drive_letter_from_unix(full_unix)))
        return EINVAL;

    /* make sure there's a filename */
    if (!strrchr(unix_path, '/'))
        return EINVAL;

    unix_len = strlen(unix_path);
    nt_len = sizeof(nt_stack)/sizeof(nt_stack[0]);
    if ((rc = dc_convert_path(unix_path, unix_len, &nt, &nt_len)))
        return rc;

    /* call the wide char version */
    rc = dc_add_file_w(nt, nt_len);

    /* free the long name path if needed */
    if (nt != nt_stack)
        dc_free(nt);

    TRACE("%d %s\n", rc, debugstr_an(unix_path, unix_len));

    return rc;
}

/* assumes caller has dcache lock
   removes listing for the given directory name, if cached
   also removes the entry from the listing in the parent directory, if cached */
static inline void dc_del_dir(const WCHAR *dir)
{
    WCHAR child[MAX_DIR_ENTRY_LEN+1];
    WCHAR par_stack[MAX_PATH+1];
    WCHAR *parent = par_stack;
    dlisting_t *listing;
    dfile_t *dfile;
    int parent_len;
    int child_len;
    int dir_len;
    int is83;
    WCHAR *slash;

    if ((listing = dc_find_listing(dir)))
        dc_remove_listing(listing);

    /* skip to last separator.  the unix path already has normalized slashes */
    dir_len = strlenW(dir);
    if (!(slash = memrchrW(dir, '/', dir_len)))
        return;

    parent_len = slash - dir;
    if ((parent_len > MAX_PATH) &&
        !(parent = dc_alloc((parent_len + 1) * sizeof(WCHAR))))
    {
        return;
    }

    dc_strncpyW(parent, dir, parent_len);
    if ((listing = dc_find_listing(parent)))
    {
        child_len = dir_len - parent_len - 1;
        if (child_len > MAX_DIR_ENTRY_LEN)
        {
            dc_remove_listing(listing);
        }
        else
        {
            dc_strncpyW(child, slash + 1, child_len);
            is83 = dc_is_8dot3(child, child_len);

            if ((dfile = dlisting_find_file(listing, child, is83)))
                dlisting_remove_file(listing, dfile);

            if (dlisting_consistency_check(listing))
                dc_remove_listing(listing);
        }
    }

    if (parent != par_stack)
        dc_free(parent);
}

/* for removing whole directories from cache
   unix name is assumed to be valid normalized path returned from a previous call to
   wine_nt_to_unix_name and  after a successful call to create the file */
int dc_rm_dir(const char *full_unix)
{
    WCHAR nt_stack[MAX_PATH+1];
    WCHAR *nt = nt_stack;
    char *unix_path;
    size_t unix_len;
    int nt_len;
    int rc;

    if (!(unix_path = dc_drive_letter_from_unix(full_unix)))
        return EINVAL;

    unix_len = strlen(unix_path);
    nt_len = sizeof(nt_stack)/sizeof(nt_stack[0]);
    if ((rc = dc_convert_path(unix_path, unix_len, &nt, &nt_len)))
        return rc;

    dcache_lock();
    dc_del_dir(nt);
    dcache_unlock();

    if (nt != nt_stack)
        dc_free(nt);

    TRACE("%d %s\n", rc, debugstr_a(full_unix));

    return rc;
}

/* add dir and file to pending.  don't store direct pointers since a listing could
   be invalidated, making the pointers invalid */
static int dc_add_pending(HANDLE handle, const WCHAR *dir, int dir_len, const WCHAR *file, int file_len, int is83)
{
    struct wine_rb_entry *entry;
    dfile_del_t *cur;
    dfile_del_t *del;
    size_t size;

    size = sizeof(*del) + (dir_len + 1 + file_len + 1) * sizeof(WCHAR);
    if (!(del = dc_alloc(size)))
        return ENOMEM;

    if (wine_rb_put(&g_dcache.pending, handle, &del->entry))
    {
        /* same handle is being added to pending multiple times
           this shouldn't happen since there should have been an NtClose that
           releases the handle followed by a dc_rm_final to remove from pending
           in this case, remove the listings for both from cache and we fall
           back to exhaustive search on next lookup */
        entry = wine_rb_get(&g_dcache.pending, handle);
        cur = WINE_RB_ENTRY_VALUE(entry, dfile_del_t, entry);
        if (strcmpiW(dir, cur->dir))
            dc_del_dir(cur->dir);

        /* remove current pending file */
        dc_remove_dfile_del(cur);
        dc_free(del);

        return EEXIST;
    }

    /* we want to avoid storing direct references to files
       inside of listings, because invalidate may reallocate memory */
    del->dir = (WCHAR *)((char*)del + sizeof(*del));
    del->file = del->dir + dir_len + 1;

    memcpy(del->dir, dir, dir_len * sizeof(WCHAR));
    del->dir[dir_len] = 0;
    memcpy(del->file, file, file_len * sizeof(WCHAR));
    del->file[file_len] = 0;

    del->is83 = is83;
    del->size = size;
    del->handle = handle;

    g_dcache.used += size;
    return 0;
}

/* add to pending using wide-char path */
static int dc_rm_file_w(WCHAR *nt, int nt_len, HANDLE handle)
{
    dlisting_t *listing;
    dfile_t *dfile;
    WCHAR *file;
    WCHAR *dir;
    int file_len;
    int dir_len;
    int is83;
    int rc;

    dir = nt;
    file = NULL;
    dir_len = 0;
    file_len = 0;
    if ((rc = dc_split_path(dir, nt_len, &dir_len, &file, &file_len)))
        return rc;

    is83 = dc_is_8dot3(file, file_len);

    dcache_lock();
    /* only add to pending if a listing and file are cached
       it's possible for the listing to not currently be cached, so the path
       is not added to pending, and the followup call to dc_rm_final doesn't
       remove the file from the cache.  this is ok, since once the file is
       removed, stat() in dc_lookup_unix_name will realize it's gone, invalidate
       the listing and re-read the directory.  the purpose of the pending is
       to reduce the I/O from this re-read.  the common case for pending comes
       from DeleteFile, where the time between dc_rm_file and dc_rm_final is short
       so the issue above should be rare */
    if ((listing = dc_find_listing(dir)) &&
        (dfile = dlisting_find_file(listing, file, is83)))
    {
        if ((rc = dc_add_pending(handle, dir, dir_len, file, file_len, is83)))
        {
            /* if we can't add pending file to delete, remove the entire listing from cache */
            dc_del_dir(dir);
        }

        if (g_dcache.used > g_dcache.allowed)
            dc_purge();
    }
    else rc = ENOENT;
    dcache_unlock();

    return rc;
}

/* called when a file is being deleted using FILE_DELETE_ON_CLOSE (not used for directories)
   unix name is assumed to be valid normalized path returned from a previous call to
   wine_nt_to_unix_name and  after a successful call to create the file */
int dc_rm_file(const char *full_unix, HANDLE handle)
{
    WCHAR nt_stack[MAX_PATH+1];
    WCHAR *nt = nt_stack;
    size_t unix_len;
    char *unix_path;
    struct stat st;
    int nt_len;
    int rc;

    if (!handle || (handle == INVALID_HANDLE_VALUE))
        return EINVAL;

    /* if it already doesn't exist, don't add to pending */
    if (stat(full_unix, &st))
        return errno;

    if (!(unix_path = dc_drive_letter_from_unix(full_unix)))
        return EINVAL;

    unix_len = strlen(unix_path);
    nt_len = sizeof(nt_stack)/sizeof(nt_stack[0]);
    if ((rc = dc_convert_path(unix_path, unix_len, &nt, &nt_len)))
        return rc;

    rc = dc_rm_file_w(nt, nt_len, handle);

    if (nt != nt_stack)
        dc_free(nt);

    TRACE("%d %p %s\n", rc, handle, debugstr_a(full_unix));

    return rc;
}

/* remove a pending file from cache.  assumes dcache lock is held */
static int dc_rm_final_nl(const WCHAR *dir, const WCHAR *file, int is83)
{
    char unix_path[PATH_MAX];
    dlisting_t *listing;
    dfile_t *dfile;
    struct stat st;
    int rc;

    if (!(listing = dc_find_listing(dir)))
        return ENOENT;

    /* there may be multiple handles for a given path
       the refcount is kept on wineserver and we don't (or want, for performance)
       access to the count.  just use the opportunity to see if we're up to date
       and only remove the file from the cache once it's gone from the disk */
    if ((dfile = dlisting_find_file(listing, file, is83)))
    {
        if ((rc = dc_build_path(listing, dfile, unix_path, sizeof(unix_path))))
        {
            /* remove the entire listing if the path is too long */
            dc_remove_listing(listing);
            return rc;
        }

        /* listing is still valid.  remove only the file from cache if no longer on disk */
        if (stat(unix_path, &st))
            dlisting_remove_file(listing, dfile);
    }

    /* whether or not we removed a file, take the opportunity
       to check for cache consistency */
    if ((rc = dlisting_consistency_check(listing)))
        dc_remove_listing(listing);

    return rc;
}

/* called after NtClose for files opened with FILE_DELETE_ON_CLOSE
   should be called only if handle was successfully closed */
int dc_rm_final(HANDLE handle)
{
    struct wine_rb_entry *entry;
    dfile_del_t *del;
    int rc;

    if (!handle || (handle == INVALID_HANDLE_VALUE))
        return EINVAL;

    dcache_lock();
    if ((entry = wine_rb_get(&g_dcache.pending, handle)))
    {
        del = WINE_RB_ENTRY_VALUE(entry, dfile_del_t, entry);
        rc = dc_rm_final_nl(del->dir, del->file, del->is83);

        /* always free this instance of pending file on finalize, even it
           did not result in the file being removed from cache.  if there
           are multiple references to the file holding it open, there will
           be another call to dc_rm_final to remove it later */
        dc_remove_dfile_del(del);
    }
    else rc = ENOENT;
    dcache_unlock();

    TRACE("%d %p\n", rc, handle);

    return rc;
}

/* moves file in cache.  assumes dcache lock is held */
static int dc_mv_w(WCHAR *new_nt, int new_len, WCHAR *old_nt, int old_len)
{
    dlisting_t *old_listing;
    dlisting_t *new_listing;
    int new_file_len;
    int old_file_len;
    int new_dir_len;
    int old_dir_len;
    WCHAR *new_file;
    WCHAR *old_file;
    WCHAR *new_dir;
    WCHAR *old_dir;
    int is83;
    int rc;

    /* check if the full old path was a cached listing and remove it */
    if ((old_listing = dc_find_listing(old_nt)))
    {
        dc_remove_listing(old_listing);

        /* fall-through and remove from parent listing, if needed */
    }

    /* we only want to update the listings that exist in cache
       but if both exist and an update fails on either one, remove both from cache */
    old_dir = old_nt;
    old_file = NULL;
    old_dir_len = 0;
    old_file_len = 0;
    if ((rc = dc_split_path(old_nt, old_len, &old_dir_len, &old_file, &old_file_len)))
        return rc;

    new_dir = new_nt;
    new_file = NULL;
    new_dir_len = 0;
    new_file_len = 0;
    if ((rc = dc_split_path(new_nt, new_len, &new_dir_len, &new_file, &new_file_len)))
        return rc;

    old_listing = dc_find_listing(old_dir);
    new_listing = dc_find_listing(new_dir);

    if (!old_listing && !new_listing)
        return ENOENT;

    /* at least one listing is cached */
    if (new_listing)
    {
        is83 = dc_is_8dot3(new_file, new_file_len);
        if ((rc = dc_add_file_w_nl(new_listing, new_file, new_file_len, is83)))
        {
            /* on any error, remove both the old and new listings from cache */
            dc_remove_listing(new_listing);
            if (old_listing && (new_listing != old_listing))
                dc_remove_listing(old_listing);
            return rc;
        }
    }

    if (old_listing)
    {
        is83 = dc_is_8dot3(old_file, old_file_len);
        if ((rc = dc_rm_final_nl(old_dir, old_file, is83))) /* removes old_listing on error */
        {
            if (new_listing && (new_listing != old_listing))
                dc_remove_listing(new_listing);
        }
    }

    return rc;
}

/* called after successful rename()
   paths should be normalized absolute paths returned by wine_nt_to_unix_name */
int dc_mv(const char *old, const char *new)
{
    WCHAR old_stack[MAX_PATH+1];
    WCHAR new_stack[MAX_PATH+1];
    WCHAR *old_nt = old_stack;
    WCHAR *new_nt = new_stack;
    char *old_unix;
    char *new_unix;
    int old_nt_len;
    int new_nt_len;
    int old_len;
    int new_len;
    int rc;

    if (!(old_unix = dc_drive_letter_from_unix(old)) ||
        !(new_unix = dc_drive_letter_from_unix(new)))
    {
        return EINVAL;
    }

    old_len = strlen(old_unix);
    old_nt_len = sizeof(old_stack)/sizeof(old_stack[0]);
    if ((rc = dc_convert_path(old_unix, old_len, &old_nt, &old_nt_len)))
        return rc;

    new_len = strlen(new_unix);
    new_nt_len = sizeof(new_stack)/sizeof(new_stack[0]);
    if ((rc = dc_convert_path(new_unix, new_len, &new_nt, &new_nt_len)))
    {
        if (old_nt != old_stack)
            dc_free(old_nt);
        return rc;
    }

    dcache_lock();
    rc = dc_mv_w(new_nt, new_nt_len, old_nt, old_nt_len);
    dcache_unlock();

    if (old_nt != old_stack)
        dc_free(old_nt);
    if (new_nt != new_stack)
        dc_free(new_nt);

    TRACE("%d %s -> %s\n", rc, debugstr_a(old), debugstr_a(new));

    return rc;
}

NTSYSAPI int CDECL wine_file_rename( const char *oldpath, const char *newpath )
{
    /* only update cache on successful rename */
    if (rename(oldpath, newpath) == -1)
        return -1;
    if (dcache_enabled)
        dc_mv(oldpath, newpath);
    return 0;
}
