/*
 *  sysdeps.h - Try to include the right system headers and get other
 *              system-specific stuff right
 *
 *  Frodo (C) 1994-1997,2002-2005 Christian Bauer
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef _SYSDEPS_H
#define _SYSDEPS_H

#include <stdlib.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <boolean.h>

#include "types.h"

/* Define if you have the <dirent.h> header file, and it defines `DIR'. */
#define HAVE_DIRENT_H 1

/* Define if you have the <fcntl.h> header file. */
#define HAVE_FCNTL_H 1

/* Define if you have the `gettimeofday' function. */
#define HAVE_GETTIMEOFDAY 1

/* Define if you have the <inttypes.h> header file. */
#define HAVE_INTTYPES_H 1

/* Define if you have the <memory.h> header file. */
#define HAVE_MEMORY_H 1

/* Define if you have the `mkdir' function. */
#define HAVE_MKDIR 1

/* Define if you have the `rmdir' function. */
#define HAVE_RMDIR 1

/* Define if you have the `sigaction' function. */
#if !defined (__vita__) && !defined(__psp__) && !defined(_3DS) && !defined(_WIN32) && !defined(__SWITCH__)
#define HAVE_SIGACTION 1
#endif

/* Define if you have the `statfs' function. */
#define HAVE_STATFS 1

/* Define if you have the <stdint.h> header file. */
#define HAVE_STDINT_H 1

/* Define if you have the <stdlib.h> header file. */
#define HAVE_STDLIB_H 1

/* Define if you have the `strerror' function. */
#define HAVE_STRERROR 1

/* Define if you have the <strings.h> header file. */
#define HAVE_STRINGS_H 1

/* Define if you have the <string.h> header file. */
#define HAVE_STRING_H 1

/* Define if you have the `strstr' function. */
#define HAVE_STRSTR 1

/* Define if `st_blocks' is member of `struct stat'. */
#define HAVE_STRUCT_STAT_ST_BLOCKS 1

/* Define if your `struct stat' has `st_blocks'. Deprecated, use
   `HAVE_STRUCT_STAT_ST_BLOCKS' instead. */
#define HAVE_ST_BLOCKS 1

/* Define if you have the <sys/param.h> header file. */
#define HAVE_SYS_PARAM_H 1

/* Define if you have the <sys/stat.h> header file. */
#define HAVE_SYS_STAT_H 1

/* Define if you have the <sys/time.h> header file. */
#define HAVE_SYS_TIME_H 1

/* Define if you have the <sys/types.h> header file. */
#define HAVE_SYS_TYPES_H 1

/* Define if you have the <unistd.h> header file. */
#define HAVE_UNISTD_H 1

/* Define if you have the `usleep' function. */
#define HAVE_USLEEP 1

/* Define if you have the <utime.h> header file. */
#define HAVE_UTIME_H 1

/* Define if `utime(file, NULL)' sets file's timestamp to the present. */
#define HAVE_UTIME_NULL 1

/* Define if you have the <values.h> header file. */
//#define HAVE_VALUES_H 1

/* Define as the return type of signal handlers (`int' or `void'). */
#define RETSIGTYPE void

/* The size of a `char', as computed by sizeof. */
#define SIZEOF_CHAR 1

/* The size of a `int', as computed by sizeof. */
#define SIZEOF_INT 4

/* The size of a `long', as computed by sizeof. */
#define SIZEOF_LONG 8

/* The size of a `long long', as computed by sizeof. */
#define SIZEOF_LONG_LONG 8

/* The size of a `short', as computed by sizeof. */
#define SIZEOF_SHORT 2

/* Define if you have the ANSI C header files. */
#define STDC_HEADERS 1

/* Define if you can safely include both <sys/time.h> and <time.h>. */
#define TIME_WITH_SYS_TIME 1

/* Define if your <sys/time.h> declares `struct tm'. */
/* #undef TM_IN_SYS_TIME */

/* Define if the X Window System is missing or not being used. */
/* #undef X_DISPLAY_MISSING */

/* Define if on AIX 3.
   System headers sometimes define this.
   We just want to avoid a redefinition error message.  */
#ifndef _ALL_SOURCE
/* # undef _ALL_SOURCE */
#endif

/* Define if you need to in order for stat and other things to work. */
/* #undef _POSIX_SOURCE */

/* Define to empty if `const' does not conform to ANSI C. */
/* #undef const */

/* Define as `__inline' if that's what the C compiler calls it, or to nothing
   if it is not supported. */
/* #undef inline */

/* Define to `int' if <sys/types.h> does not define. */
/* #undef mode_t */

/* Define to `long' if <sys/types.h> does not define. */
/* #undef off_t */

/* Define to `int' if <sys/types.h> does not define. */
/* #undef pid_t */


#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#ifdef HAVE_VALUES_H
#include <values.h>
#endif

#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#ifdef HAVE_UTIME_H
#include <utime.h>
#endif

#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif

#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#ifdef HAVE_SYS_VFS_H
#include <sys/vfs.h>
#endif

#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif

#ifdef HAVE_SYS_MOUNT_H
#include <sys/mount.h>
#endif

#ifdef HAVE_SYS_STATFS_H
#include <sys/statfs.h>
#endif

#ifdef HAVE_SYS_STATVFS_H
#include <sys/statvfs.h>
#endif

#if TIME_WITH_SYS_TIME
# include <sys/time.h>
# include <time.h>
#else
# if HAVE_SYS_TIME_H
#  include <sys/time.h>
# else
#  include <time.h>
# endif
#endif

#if HAVE_DIRENT_H
# include <dirent.h>
#else
# define dirent direct
# if HAVE_SYS_NDIR_H
#  include <sys/ndir.h>
# endif
# if HAVE_SYS_DIR_H
#  include <sys/dir.h>
# endif
# if HAVE_NDIR_H
#  include <ndir.h>
# endif
#endif

#include <assert.h>

#if EEXIST == ENOTEMPTY
#define BROKEN_OS_PROBABLY_AIX
#endif

#ifdef __NeXT__
#define S_IRUSR S_IREAD
#define S_IWUSR S_IWRITE
#define S_IXUSR S_IEXEC
#define S_ISDIR(val) (S_IFDIR & val)
struct utimbuf
{
    time_t actime;
    time_t modtime;
};
#endif

#ifdef __DOS__
#include <pc.h>
#include <io.h>
#else
#undef O_BINARY
#define O_BINARY 0
#endif

#ifdef _WIN32
#include <direct.h>
#if !defined(M_PI)
#define M_PI 3.14159265358979323846
#endif
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define LITTLE_ENDIAN_UNALIGNED 1
#endif

#endif
