/*
 * dc_posix.c — the small POSIX gaps between what upstream Tux Racer assumes
 * and what KallistiOS's newlib provides.
 *
 * Deliberately tiny. Every entry here is a function upstream calls that KOS
 * does not have; anything KOS *does* have is used directly rather than
 * wrapped, so this file does not become a shadow libc.
 */

#include <string.h>
#include <stdio.h>

#include <sys/utsname.h>

#include <kos/version.h>

/*
 * uname(2). Single consumer: src/os_util.c:205, which builds the "what am I
 * running on" string for the version display. The values are shown, never
 * parsed, so constants are the correct implementation — but they are honest
 * constants, not "unknown": a screenshot of the version screen should say what
 * it actually ran on.
 *
 * KOS_VERSION_STRING comes from <kos/version.h>, so the release field tracks
 * whatever KOS the SDK image was built with instead of a number baked in here
 * that would rot.
 */
int uname(struct utsname *buf) {
    if(buf == NULL)
        return -1;

    memset(buf, 0, sizeof(*buf));

    strncpy(buf->sysname,  "KallistiOS",        sizeof(buf->sysname) - 1);
    strncpy(buf->nodename, "dreamcast",         sizeof(buf->nodename) - 1);
    strncpy(buf->release,  KOS_VERSION_STRING,  sizeof(buf->release) - 1);
    strncpy(buf->version,  __DATE__,            sizeof(buf->version) - 1);
    strncpy(buf->machine,  "sh4",               sizeof(buf->machine) - 1);

    return 0;
}
