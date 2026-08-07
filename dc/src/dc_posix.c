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
#include <pwd.h>
#include <unistd.h>

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

/*
 * getuid(2) / getpwuid(3). newlib declares both and defines neither; the
 * single consumer is src/game_config.c:843, which builds the config directory
 * path as "<pw_dir>/<CONFIG_DIR>" and then writes the options file into it.
 *
 * pw_dir is "/ram" — KOS's in-memory filesystem — and NOT "/cd", which is a
 * read-only ISO9660 mount and would fail every write, and not "/vmu/a1"
 * either: game_config.c mkdir()s the directory and then opens a file inside
 * it, and the VMU filesystem is flat, with no subdirectories at all.
 *
 * CONSEQUENCE, and it is a real one: settings do not survive a power cycle.
 * Persisting them means a VMU save, which is a different shape of code — one
 * fixed-name save file written whole, not a directory — so it belongs in a
 * save layer, not in a fake passwd entry. Tracked in kb/STATE.md.
 */
uid_t getuid(void) {
    return 0;
}

struct passwd *getpwuid(uid_t uid) {
    /* Static storage is correct here: getpwuid() is specified to return a
     * pointer to a static buffer that the next call may overwrite, and there
     * is exactly one caller, once, at startup. */
    static char name[]  = "dreamcast";
    static char dir[]   = "/ram";
    static char shell[] = "";
    static char empty[] = "";
    static struct passwd pw;

    (void)uid;

    pw.pw_name    = name;
    pw.pw_passwd  = empty;
    pw.pw_uid     = 0;
    pw.pw_gid     = 0;
    pw.pw_comment = empty;
    pw.pw_gecos   = empty;
    pw.pw_dir     = dir;
    pw.pw_shell   = shell;

    return &pw;
}
