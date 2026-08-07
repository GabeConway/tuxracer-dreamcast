/*
 * sys/utsname.h — the one POSIX header KallistiOS's newlib does not ship.
 *
 * MEASURED: of the seven system headers src/tuxracer.h pulls in under
 * COMPILER_IS_UNIX_COMPATIBLE (pwd.h, dirent.h, unistd.h, sys/time.h,
 * sys/types.h, sys/stat.h, ieeefp.h) every one exists in the sh-elf sysroot
 * except this. Probe:
 *
 *   for h in pwd.h dirent.h unistd.h sys/utsname.h sys/time.h sys/stat.h; do
 *       echo "#include <$h>" > /tmp/p.c; sh-elf-gcc -c /tmp/p.c -o /tmp/p.o
 *   done
 *   -> only sys/utsname.h fails
 *
 * Its single consumer is src/os_util.c:205, which formats a human-readable
 * "what am I running on" string for the debug/version display. Nothing depends
 * on the values, so a truthful constant answer is the whole requirement.
 * Implementation: dc/src/dc_posix.c.
 */

#ifndef TR_DC_SYS_UTSNAME_H
#define TR_DC_SYS_UTSNAME_H

#ifdef __cplusplus
extern "C" {
#endif

/* Field width follows the Linux convention. src/os_util.c:208 sums the
 * lengths of sysname/release/version against its own buffer, so nothing here
 * may be unbounded. */
#define _UTSNAME_LENGTH 65

struct utsname {
    char sysname[_UTSNAME_LENGTH];   /* "KallistiOS" */
    char nodename[_UTSNAME_LENGTH];  /* "dreamcast"  */
    char release[_UTSNAME_LENGTH];   /* KOS version  */
    char version[_UTSNAME_LENGTH];   /* build date   */
    char machine[_UTSNAME_LENGTH];   /* "sh4"        */
};

int uname(struct utsname *buf);

#ifdef __cplusplus
}
#endif

#endif /* TR_DC_SYS_UTSNAME_H */
