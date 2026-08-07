/* tcl_compat.c - Tcl 8.0 API (dc/include/tcl.h) implemented over Jim Tcl.
 *
 * The kos-port builds Jim 0.84 as libjim.a with `--without-ext="aio,zlib"`
 * (/opt/toolchains/dc/kos-ports/libjimtcl/files/KOSMakefile.mk:2). Everything
 * awkward in here follows from three facts about that build, all verified
 * rather than assumed — see kb/design-tcl.md:
 *
 *   1. Jim commands are objv-based; src/'s are argc/argv-based, and src/ walks
 *      argv until it hits a NULL (src/tcl_util.h:44). The bridge below
 *      materialises a NUL-terminated char *argv[] per call.
 *   2. No aio means no `open`, `gets`, `puts`, `close` at all — tclcompat.tcl:12
 *      gates all of them on `[exists -command stdout]`. The stock data script
 *      courses/course_idx.tcl uses all four, so this file supplies them.
 *   3. newlib declares access() but nothing in the toolchain defines it, and
 *      jim-file.c:492 implements `file exists` with it. Without the shim at the
 *      bottom of this file the game does not link.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <jimtcl/jim.h>

#include "tcl.h"

/* Tcl_Interp is a Jim_Interp wearing a different name. Keeping the cast in one
 * place means command callbacks get back byte-for-byte the pointer that
 * src/main.c:151 stored in g_game.tcl_interp. */
static Jim_Interp *J(Tcl_Interp *ip)
{
    return (Jim_Interp *) ip;
}

/*===========================================================================
 * 1. Commands: objv -> argc/argv bridge
 *=========================================================================*/

typedef struct {
    Tcl_CmdProc *proc;
    ClientData   clientData;
} cmd_bridge_t;

/* argc for the game's own commands tops out around 20 (src/course_load.c's
 * tux_item_spec); the malloc path exists only so a pathological script cannot
 * smash the stack. */
#define BRIDGE_STACK_ARGS 32

static int bridge_invoke(Jim_Interp *interp, int objc, Jim_Obj *const *objv)
{
    cmd_bridge_t *bridge = Jim_CmdPrivData(interp);
    char  *stackv[BRIDGE_STACK_ARGS + 1];
    char **argv;
    int    i, rc;

    argv = (objc < BRIDGE_STACK_ARGS)
         ? stackv
         : (char **) malloc((objc + 1) * sizeof(char *));
    if (argv == NULL) {
        Jim_SetResultString(interp, "out of memory building argv", -1);
        return JIM_ERR;
    }

    /* Casting away const is safe here: no callback in src/ writes through
     * argv[i]. Checked by grepping every src/ .c file for strtok and for
     * writes through argv: the only strtok() calls (src/debug.c:59,
     * src/keyboard_util.c:103) operate on private copies. The strings belong
     * to objv[i], which the caller keeps referenced for this call. */
    for (i = 0; i < objc; i++) {
        argv[i] = (char *) Jim_String(objv[i]);
    }
    argv[objc] = NULL;  /* src/tcl_util.h:44 CHECK_ARG dereferences argv[argc] */

    rc = bridge->proc(bridge->clientData, (Tcl_Interp *) interp, objc, argv);

    if (argv != stackv) {
        free(argv);
    }

    /* TCL_OK/TCL_ERROR and JIM_OK/JIM_ERR are the same integers (jim.h:132). */
    return (rc == TCL_OK) ? JIM_OK : JIM_ERR;
}

static void bridge_delete(Jim_Interp *interp, void *privData)
{
    (void) interp;
    free(privData);
}

Tcl_Command Tcl_CreateCommand(Tcl_Interp *interp, const char *cmdName,
                              Tcl_CmdProc *proc, ClientData clientData,
                              Tcl_CmdDeleteProc *deleteProc)
{
    cmd_bridge_t *bridge;

    /* Every call site in src/ passes 0 for deleteProc (e.g.
     * src/hier_cb.c:361-368), so there is nothing to forward. */
    (void) deleteProc;

    bridge = (cmd_bridge_t *) malloc(sizeof(*bridge));
    if (bridge == NULL) {
        return NULL;
    }
    bridge->proc = proc;
    bridge->clientData = clientData;

    if (Jim_CreateCommand(J(interp), cmdName, bridge_invoke, bridge,
                          bridge_delete) != JIM_OK) {
        free(bridge);
        return NULL;
    }
    /* src/ ignores the return value; a non-NULL token is enough. */
    return (Tcl_Command) bridge;
}

/*===========================================================================
 * 2. Result
 *=========================================================================*/

/* Tcl_AppendResult APPENDS to whatever is already in the result, and src/
 * depends on that: src/hier_cb.c:56-59 lets Tcl_GetDouble() install "expected
 * floating-point number but got ..." and then tacks ": invalid rotation angle"
 * onto it. Jim empties the result before every command invocation
 * (jim.c JimInvokeCommand, `Jim_SetEmptyResult(interp)` before the dispatch),
 * so seeding from the current result reproduces Tcl 8's behaviour exactly and
 * cannot pick up a previous command's leftovers. */
void Tcl_AppendResult(Tcl_Interp *interp, ...)
{
    Jim_Interp *j = J(interp);
    Jim_Obj    *acc;
    va_list     ap;
    const char *s;

    acc = Jim_NewStringObj(j, Jim_String(Jim_GetResult(j)), -1);
    Jim_IncrRefCount(acc);

    va_start(ap, interp);
    while ((s = va_arg(ap, const char *)) != NULL) {
        Jim_AppendString(j, acc, s, -1);
    }
    va_end(ap);

    Jim_SetResult(j, acc);
    Jim_DecrRefCount(j, acc);
}

const char *Tcl_GetStringResult(Tcl_Interp *interp)
{
    return Jim_String(Jim_GetResult(J(interp)));
}

/*===========================================================================
 * 3. Result objects
 *
 * Tcl 8.0's Tcl_NewStringObj/NewIntObj/NewBooleanObj take no interpreter, but
 * every Jim_Obj constructor needs one. Rather than stash a global "the one
 * interp", a Tcl_Obj here is a tiny heap record that Tcl_SetObjResult converts
 * and frees. That is sound because all nine construction sites in src/ hand
 * the object straight to Tcl_SetObjResult (src/audio_data.c:624, :652, :764,
 * :774; src/course_mgr.c:1339; src/game_config.c:1051, :1056, :1061, :1066) —
 * an object built and dropped would leak, and there are none.
 *=========================================================================*/

enum { OBJ_STRING, OBJ_INT, OBJ_BOOL };

struct Tcl_Obj {
    int   kind;
    long  ival;
    char *str;
    int   len;
};

static Tcl_Obj *obj_alloc(int kind)
{
    Tcl_Obj *o = (Tcl_Obj *) calloc(1, sizeof(*o));
    if (o != NULL) {
        o->kind = kind;
    }
    return o;
}

Tcl_Obj *Tcl_NewStringObj(const char *bytes, int length)
{
    Tcl_Obj *o = obj_alloc(OBJ_STRING);
    if (o == NULL) {
        return NULL;
    }
    if (bytes == NULL) {
        length = 0;
    } else if (length < 0) {
        length = (int) strlen(bytes);
    }
    /* src/game_config.c:1056 passes (&char_val, 1) — a single char that is not
     * NUL-terminated — so the length must be honoured, never strlen'd. */
    o->str = (char *) malloc((size_t) length + 1);
    if (o->str == NULL) {
        free(o);
        return NULL;
    }
    if (length > 0) {
        memcpy(o->str, bytes, (size_t) length);
    }
    o->str[length] = '\0';
    o->len = length;
    return o;
}

Tcl_Obj *Tcl_NewIntObj(int intValue)
{
    Tcl_Obj *o = obj_alloc(OBJ_INT);
    if (o != NULL) {
        o->ival = intValue;
    }
    return o;
}

Tcl_Obj *Tcl_NewBooleanObj(int boolValue)
{
    Tcl_Obj *o = obj_alloc(OBJ_BOOL);
    if (o != NULL) {
        o->ival = (boolValue != 0);
    }
    return o;
}

void Tcl_SetObjResult(Tcl_Interp *interp, Tcl_Obj *objPtr)
{
    Jim_Interp *j = J(interp);
    Jim_Obj    *r;

    if (objPtr == NULL) {
        Jim_SetEmptyResult(j);
        return;
    }

    switch (objPtr->kind) {
    case OBJ_STRING:
        r = Jim_NewStringObj(j, objPtr->str, objPtr->len);
        break;
    default:
        /* Jim has no distinct boolean type; `if {[tux_load_music ...]}`
         * (tuxracer_init.tcl:31) is happy with 0/1. */
        r = Jim_NewIntObj(j, (jim_wide) objPtr->ival);
        break;
    }
    Jim_SetResult(j, r);

    free(objPtr->str);
    free(objPtr);
}

/*===========================================================================
 * 4. Value conversion
 *=========================================================================*/

/* src/lights.c:104 can reach Tcl_GetInt with *argv == NULL when the script
 * called tux_course_light with too few arguments: NEXT_ARG walks off the end
 * and lands on the NULL terminator. Real Tcl would fault; on the Dreamcast
 * that is a reboot, so it is worth four lines to turn it into an error. */
static int conv_prologue(Jim_Interp *j, const char *src, Jim_Obj **out)
{
    if (src == NULL) {
        Jim_SetResultString(j, "missing argument", -1);
        return 0;
    }
    *out = Jim_NewStringObj(j, src, -1);
    Jim_IncrRefCount(*out);
    return 1;
}

int Tcl_GetInt(Tcl_Interp *interp, const char *src, int *intPtr)
{
    Jim_Interp *j = J(interp);
    Jim_Obj    *o;
    long        v;
    int         rc;

    if (!conv_prologue(j, src, &o)) {
        return TCL_ERROR;
    }
    rc = Jim_GetLong(j, o, &v);
    Jim_DecrRefCount(j, o);
    if (rc != JIM_OK) {
        return TCL_ERROR;
    }
    *intPtr = (int) v;      /* sh-elf: sizeof(long) == sizeof(int) == 4 */
    return TCL_OK;
}

int Tcl_GetDouble(Tcl_Interp *interp, const char *src, double *doublePtr)
{
    Jim_Interp *j = J(interp);
    Jim_Obj    *o;
    int         rc;

    if (!conv_prologue(j, src, &o)) {
        return TCL_ERROR;
    }
    rc = Jim_GetDouble(j, o, doublePtr);
    Jim_DecrRefCount(j, o);
    return (rc == JIM_OK) ? TCL_OK : TCL_ERROR;
}

int Tcl_GetBoolean(Tcl_Interp *interp, const char *src, int *boolPtr)
{
    Jim_Interp *j = J(interp);
    Jim_Obj    *o;
    int         rc;

    if (!conv_prologue(j, src, &o)) {
        return TCL_ERROR;
    }
    rc = Jim_GetBoolean(j, o, boolPtr);
    Jim_DecrRefCount(j, o);
    return (rc == JIM_OK) ? TCL_OK : TCL_ERROR;
}

/*===========================================================================
 * 5. Lists
 *=========================================================================*/

/* One allocation holding the pointer array followed by the strings, exactly
 * like Tcl's, because callers keep the block alive while walking it and free
 * it with a single Tcl_Free (src/course_mgr.c:519).
 *
 * This never returns TCL_ERROR. Jim's list conversion cannot fail — jim.c
 * SetListFromAny is unconditionally `return JIM_OK` and its own comment says
 * "The string->list conversion can't fail" — and returning an error would be
 * actively dangerous: src/tcl_util.c:43 calls Tcl_Free on the (uninitialised)
 * argv in its error path. Malformed input yields a best-effort split, and the
 * element-count checks the callers already do catch it. */
int Tcl_SplitList(Tcl_Interp *interp, const char *list,
                  int *argcPtr, char ***argvPtr)
{
    Jim_Interp *j = J(interp);
    Jim_Obj    *lo;
    char      **argv;
    char       *p;
    size_t      bytes;
    int         n, i;

    lo = Jim_NewStringObj(j, (list != NULL) ? list : "", -1);
    Jim_IncrRefCount(lo);

    n = Jim_ListLength(j, lo);
    bytes = (size_t) (n + 1) * sizeof(char *);
    for (i = 0; i < n; i++) {
        bytes += strlen(Jim_String(Jim_ListGetIndex(j, lo, i))) + 1;
    }

    argv = (char **) malloc(bytes);
    if (argv == NULL) {
        Jim_DecrRefCount(j, lo);
        Jim_SetResultString(j, "out of memory splitting list", -1);
        return TCL_ERROR;
    }

    p = (char *) (argv + n + 1);
    for (i = 0; i < n; i++) {
        const char *e = Jim_String(Jim_ListGetIndex(j, lo, i));
        size_t      l = strlen(e) + 1;
        memcpy(p, e, l);
        argv[i] = p;
        p += l;
    }
    argv[n] = NULL;     /* src/course_mgr.c:447 walks until *argv == NULL */

    Jim_DecrRefCount(j, lo);

    *argcPtr = n;
    *argvPtr = argv;
    return TCL_OK;
}

void Tcl_Free(char *ptr)
{
    free(ptr);
}

/*===========================================================================
 * 6. Variables
 *=========================================================================*/

char *Tcl_GetVar(Tcl_Interp *interp, const char *varName, int flags)
{
    Jim_Interp *j = J(interp);
    Jim_Obj    *v;

    (void) flags;   /* every call site passes TCL_GLOBAL_ONLY */

    /* JIM_NONE, not JIM_ERRMSG: src/game_config.c:151 treats NULL as "not
     * configured, use the default" and must not leave an error behind. */
    v = Jim_GetGlobalVariableStr(j, varName, JIM_NONE);
    if (v == NULL) {
        return NULL;
    }
    /* Valid until the variable is reassigned. Every caller copies immediately
     * (src/game_config.c:154 string_copy). */
    return (char *) Jim_String(v);
}

char *Tcl_SetVar(Tcl_Interp *interp, const char *varName, const char *newValue,
                 int flags)
{
    Jim_Interp *j = J(interp);
    Jim_Obj    *v;

    (void) flags;

    v = Jim_NewStringObj(j, (newValue != NULL) ? newValue : "", -1);
    if (Jim_SetGlobalVariableStr(j, varName, v) != JIM_OK) {
        /* Jim frees a zero-refcount value object when the set fails. */
        return NULL;
    }
    return (char *) Jim_String(v);
}

/*===========================================================================
 * 7. Hash table
 *
 * Jim has Jim_HashTable, but its entry struct is private-ish and its iterator
 * is heap-allocated, while src/hash.c:90 embeds a Tcl_HashSearch by value and
 * src/hash.c:119 reads searchPtr->tablePtr. Open hashing here is both smaller
 * and immune to a Jim point release moving those fields. String keys only:
 * the only two callers ask for TCL_STRING_KEYS (src/hier.c:417, src/hash.c:28).
 *=========================================================================*/

#define HASH_STATIC_BUCKETS  16
#define HASH_REBUILD_FACTOR  3      /* same load factor Tcl 8.0 uses */

static unsigned int hash_string(const char *s)
{
    unsigned int r = 0;
    while (*s != '\0') {
        r += (r << 3) + (unsigned char) *s++;
    }
    return r;
}

void Tcl_InitHashTable(Tcl_HashTable *tablePtr, int keyType)
{
    int i;

    for (i = 0; i < HASH_STATIC_BUCKETS; i++) {
        tablePtr->staticBuckets[i] = NULL;
    }
    tablePtr->buckets     = tablePtr->staticBuckets;
    tablePtr->numBuckets  = HASH_STATIC_BUCKETS;
    tablePtr->numEntries  = 0;
    tablePtr->rebuildSize = HASH_STATIC_BUCKETS * HASH_REBUILD_FACTOR;
    tablePtr->mask        = HASH_STATIC_BUCKETS - 1;
    tablePtr->keyType     = keyType;    /* recorded; only STRING is honoured */
}

static void hash_grow(Tcl_HashTable *tablePtr)
{
    Tcl_HashEntry **old      = tablePtr->buckets;
    int             oldCount = tablePtr->numBuckets;
    int             newCount = oldCount * 4;
    Tcl_HashEntry **fresh;
    int             i;

    fresh = (Tcl_HashEntry **) calloc((size_t) newCount, sizeof(*fresh));
    if (fresh == NULL) {
        return;     /* stay correct, just slower */
    }

    tablePtr->buckets     = fresh;
    tablePtr->numBuckets  = newCount;
    tablePtr->mask        = (unsigned int) newCount - 1;
    tablePtr->rebuildSize = newCount * HASH_REBUILD_FACTOR;

    for (i = 0; i < oldCount; i++) {
        Tcl_HashEntry *e = old[i];
        while (e != NULL) {
            Tcl_HashEntry *next = e->nextPtr;
            unsigned int   b    = e->hash & tablePtr->mask;
            e->nextPtr = fresh[b];
            fresh[b] = e;
            e = next;
        }
    }
    if (old != tablePtr->staticBuckets) {
        free(old);
    }
}

Tcl_HashEntry *Tcl_FindHashEntry(Tcl_HashTable *tablePtr, const char *key)
{
    unsigned int   h = hash_string(key);
    Tcl_HashEntry *e;

    for (e = tablePtr->buckets[h & tablePtr->mask]; e != NULL; e = e->nextPtr) {
        if (e->hash == h && strcmp(e->key, key) == 0) {
            return e;
        }
    }
    return NULL;
}

Tcl_HashEntry *Tcl_CreateHashEntry(Tcl_HashTable *tablePtr, const char *key,
                                   int *newPtr)
{
    unsigned int   h = hash_string(key);
    Tcl_HashEntry *e;
    size_t         klen;

    for (e = tablePtr->buckets[h & tablePtr->mask]; e != NULL; e = e->nextPtr) {
        if (e->hash == h && strcmp(e->key, key) == 0) {
            *newPtr = 0;
            return e;
        }
    }

    klen = strlen(key);
    /* key[1] in the struct already accounts for the NUL. */
    e = (Tcl_HashEntry *) malloc(sizeof(Tcl_HashEntry) + klen);
    if (e == NULL) {
        *newPtr = 0;
        return NULL;
    }
    memcpy(e->key, key, klen + 1);
    e->hash       = h;
    e->tablePtr   = tablePtr;
    e->clientData = NULL;
    e->nextPtr    = tablePtr->buckets[h & tablePtr->mask];
    tablePtr->buckets[h & tablePtr->mask] = e;
    tablePtr->numEntries++;

    if (tablePtr->numEntries >= tablePtr->rebuildSize) {
        hash_grow(tablePtr);
    }

    *newPtr = 1;
    return e;
}

void Tcl_DeleteHashEntry(Tcl_HashEntry *entryPtr)
{
    Tcl_HashTable  *t = entryPtr->tablePtr;
    Tcl_HashEntry **pp = &t->buckets[entryPtr->hash & t->mask];

    while (*pp != NULL) {
        if (*pp == entryPtr) {
            *pp = entryPtr->nextPtr;
            t->numEntries--;
            free(entryPtr);
            return;
        }
        pp = &(*pp)->nextPtr;
    }
}

void Tcl_DeleteHashTable(Tcl_HashTable *tablePtr)
{
    int i;

    for (i = 0; i < tablePtr->numBuckets; i++) {
        Tcl_HashEntry *e = tablePtr->buckets[i];
        while (e != NULL) {
            Tcl_HashEntry *next = e->nextPtr;
            free(e);
            e = next;
        }
        tablePtr->buckets[i] = NULL;
    }
    if (tablePtr->buckets != tablePtr->staticBuckets) {
        free(tablePtr->buckets);
    }
    tablePtr->buckets    = tablePtr->staticBuckets;
    tablePtr->numBuckets = HASH_STATIC_BUCKETS;
    tablePtr->numEntries = 0;
    tablePtr->mask       = HASH_STATIC_BUCKETS - 1;
}

/* The "next" entry is fetched before the current one is returned, so
 * src/hash.c's callers may delete the entry they were just handed. */
static Tcl_HashEntry *hash_advance(Tcl_HashSearch *searchPtr)
{
    Tcl_HashTable *t = searchPtr->tablePtr;

    while (searchPtr->nextEntryPtr == NULL) {
        if (searchPtr->nextIndex >= t->numBuckets) {
            return NULL;
        }
        searchPtr->nextEntryPtr = t->buckets[searchPtr->nextIndex++];
    }
    {
        Tcl_HashEntry *e = searchPtr->nextEntryPtr;
        searchPtr->nextEntryPtr = e->nextPtr;
        return e;
    }
}

Tcl_HashEntry *Tcl_FirstHashEntry(Tcl_HashTable *tablePtr,
                                  Tcl_HashSearch *searchPtr)
{
    searchPtr->tablePtr     = tablePtr;
    searchPtr->nextIndex    = 0;
    searchPtr->nextEntryPtr = NULL;
    return hash_advance(searchPtr);
}

Tcl_HashEntry *Tcl_NextHashEntry(Tcl_HashSearch *searchPtr)
{
    return hash_advance(searchPtr);
}

/*===========================================================================
 * 8. Minimal file I/O commands
 *
 * The kos-port drops the aio extension, and tclcompat.tcl:12 gates puts/gets/
 * close/read/eof on `[exists -command stdout]`, which aio is what provides.
 * So a stock Jim on this target has NO puts and NO open.
 *
 * courses/course_idx.tcl:6-15 does `open course.tcl r`, `gets $fileId line`
 * and `puts stderr ...`, and tuxracer_init.tcl:23 sources it unconditionally.
 * The `open` failure would be caught by the surrounding `catch`, but the
 * `puts stderr` on the next line would not be, and the error propagates out of
 * Tcl_EvalFile into src/main.c:105 handle_error() — a hard exit at boot.
 *
 * Read-only, line-oriented, and no channel objects: that is the whole of what
 * the stock data needs. Not provided: read, seek, tell, flush, fconfigure,
 * write modes.
 *=========================================================================*/

#define IO_MAX_FILES 32     /* stock data opens one per contrib course (11) and
                             * never closes them - get_course_info leaks by
                             * design (courses/course_idx.tcl:5-43) */

static FILE *io_files[IO_MAX_FILES];

static int io_slot(const char *name)
{
    int n;
    if (name == NULL || strncmp(name, "file", 4) != 0) {
        return -1;
    }
    n = atoi(name + 4);
    if (n < 0 || n >= IO_MAX_FILES || io_files[n] == NULL) {
        return -1;
    }
    return n;
}

static int io_open(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    const char *path = Jim_String(argv[1]);
    const char *mode = (argc > 2) ? Jim_String(argv[2]) : "r";
    char        handle[16];
    FILE       *fp;
    int         i;

    if (mode[0] != 'r') {
        Jim_SetResultFormatted(interp,
            "open: only read modes are supported on this target (got \"%s\")",
            mode);
        return JIM_ERR;
    }

    for (i = 0; i < IO_MAX_FILES && io_files[i] != NULL; i++) {
        /* find a free slot */
    }
    if (i == IO_MAX_FILES) {
        Jim_SetResultString(interp, "open: too many open files", -1);
        return JIM_ERR;
    }

    fp = fopen(path, "rb");
    if (fp == NULL) {
        Jim_SetResultFormatted(interp, "couldn't open \"%s\"", argv[1]);
        return JIM_ERR;
    }

    io_files[i] = fp;
    sprintf(handle, "file%d", i);
    Jim_SetResultString(interp, handle, -1);
    return JIM_OK;
}

/* `gets chan ?varName?` - with varName, sets it and returns the character
 * count, or -1 at end of file. That is the form course_idx.tcl:15 tests. */
static int io_gets(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    int   slot = io_slot(Jim_String(argv[1]));
    char *buf  = NULL;
    size_t len = 0, cap = 0;
    int    c, sawAny = 0;

    if (slot < 0) {
        Jim_SetResultFormatted(interp, "can not find channel named \"%#s\"",
                               argv[1]);
        return JIM_ERR;
    }

    while ((c = fgetc(io_files[slot])) != EOF) {
        sawAny = 1;
        if (c == '\n') {
            break;
        }
        if (len + 2 > cap) {
            char *nb;
            cap = (cap == 0) ? 128 : cap * 2;
            nb = (char *) realloc(buf, cap);
            if (nb == NULL) {
                free(buf);
                Jim_SetResultString(interp, "out of memory in gets", -1);
                return JIM_ERR;
            }
            buf = nb;
        }
        buf[len++] = (char) c;
    }
    if (len > 0 && buf[len - 1] == '\r') {  /* tolerate CRLF data files */
        len--;
    }

    if (argc > 2) {
        Jim_Obj *line = Jim_NewStringObj(interp, (buf != NULL) ? buf : "",
                                         (int) len);
        if (Jim_SetVariable(interp, argv[2], line) != JIM_OK) {
            free(buf);
            return JIM_ERR;
        }
        Jim_SetResultInt(interp, sawAny ? (jim_wide) len : -1);
    } else {
        Jim_SetResultString(interp, (buf != NULL) ? buf : "", (int) len);
    }
    free(buf);
    return JIM_OK;
}

static int io_close(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    int slot = io_slot(Jim_String(argv[1]));

    (void) argc;
    if (slot < 0) {
        Jim_SetResultFormatted(interp, "can not find channel named \"%#s\"",
                               argv[1]);
        return JIM_ERR;
    }
    fclose(io_files[slot]);
    io_files[slot] = NULL;
    Jim_SetEmptyResult(interp);
    return JIM_OK;
}

static int io_eof(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    int slot = io_slot(Jim_String(argv[1]));

    (void) argc;
    if (slot < 0) {
        Jim_SetResultFormatted(interp, "can not find channel named \"%#s\"",
                               argv[1]);
        return JIM_ERR;
    }
    Jim_SetResultInt(interp, feof(io_files[slot]) ? 1 : 0);
    return JIM_OK;
}

/* `puts ?-nonewline? ?channelId? string`. Only stdout and stderr are writable;
 * course_idx.tcl:7 and :90 are the only uses in the stock data and both go to
 * stderr. */
static int io_puts(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    int         i = 1;
    int         newline = 1;
    const char *chan = "stdout";
    const char *msg;
    int         len;
    FILE       *out;

    if (i < argc && strcmp(Jim_String(argv[i]), "-nonewline") == 0) {
        newline = 0;
        i++;
    }
    if (argc - i == 2) {
        chan = Jim_String(argv[i]);
        i++;
    } else if (argc - i != 1) {
        Jim_WrongNumArgs(interp, 1, argv, "?-nonewline? ?channelId? string");
        return JIM_ERR;
    }

    if (strcmp(chan, "stderr") == 0) {
        out = stderr;
    } else if (strcmp(chan, "stdout") == 0) {
        out = stdout;
    } else {
        Jim_SetResultFormatted(interp, "channel \"%s\" is not open for writing",
                               chan);
        return JIM_ERR;
    }

    msg = Jim_GetString(argv[i], &len);
    fwrite(msg, 1, (size_t) len, out);
    if (newline) {
        fputc('\n', out);
    }
    Jim_SetEmptyResult(interp);
    return JIM_OK;
}

static void io_register(Jim_Interp *interp)
{
    Jim_RegisterCmd(interp, "open",  "fileName ?access?", 1, 2, io_open,
                    NULL, NULL, 0);
    Jim_RegisterCmd(interp, "gets",  "channelId ?varName?", 1, 2, io_gets,
                    NULL, NULL, 0);
    Jim_RegisterCmd(interp, "close", "channelId", 1, 1, io_close,
                    NULL, NULL, 0);
    Jim_RegisterCmd(interp, "eof",   "channelId", 1, 1, io_eof,
                    NULL, NULL, 0);
    Jim_RegisterCmd(interp, "puts",  "?-nonewline? ?channelId? string", 1, 3,
                    io_puts, NULL, NULL, 0);
}

/*===========================================================================
 * 9. Interpreter
 *=========================================================================*/

Tcl_Interp *Tcl_CreateInterp(void)
{
    Jim_Interp *j = Jim_CreateInterp();

    if (j == NULL) {
        return NULL;
    }

    /* Jim_CreateInterp registers nothing; jimsh.c:103-107 is the reference
     * sequence and this mirrors it. */
    Jim_RegisterCoreCommands(j);
    if (Jim_InitStaticExtensions(j) != JIM_OK) {
        /* Non-fatal on purpose: the Tcl-level extensions (stdlib, tclcompat,
         * glob, oo) are independent, and losing one should not cost the boot.
         * Losing glob would, but that failure will surface at course_idx.tcl
         * with a better message than anything we could print here. */
        fprintf(stderr, "tcl_compat: Jim_InitStaticExtensions failed: %s\n",
                Jim_String(Jim_GetResult(j)));
    }
    io_register(j);

    return (Tcl_Interp *) j;
}

/* src/main.c:113 and two siblings assert on this. The shim never deletes an
 * interpreter, so it is always false. */
int Tcl_InterpDeleted(Tcl_Interp *interp)
{
    (void) interp;
    return 0;
}

int Tcl_EvalFile(Tcl_Interp *interp, const char *fileName)
{
    Jim_Interp *j = J(interp);
    int         rc;

    rc = Jim_EvalFile(j, fileName);

    if (rc != JIM_OK) {
        /* Jim leaves only the bare message in the result. errorInfo (from the
         * statically linked stdlib.tcl:73) rewrites it as "file:line: Error: ..."
         * plus a stack dump - over a serial line that is the difference
         * between a usable and a useless report. Guarded because errorInfo is
         * absent if Jim_InitStaticExtensions failed above. */
        Jim_Obj *name = Jim_NewStringObj(j, "errorInfo", -1);
        Jim_IncrRefCount(name);
        if (Jim_GetCommand(j, name, JIM_NONE) != NULL) {
            Jim_MakeErrorMessage(j);
        }
        Jim_DecrRefCount(j, name);
        return TCL_ERROR;
    }
    return TCL_OK;
}

/*===========================================================================
 * 10. Channels - no-ops
 *
 * src/tcl_util.c:140-170 is the only caller and its whole body sits inside
 * `#if defined( NATIVE_WIN32_COMPILER )`, so on this target nothing calls
 * either function. They exist so the header is honest, and because the Tcl
 * result already goes to C stdout/stderr through io_puts above.
 *=========================================================================*/

void Tcl_SetStdChannel(Tcl_Channel channel, int type)
{
    (void) channel;
    (void) type;
}

Tcl_Channel Tcl_MakeFileChannel(ClientData handle, int mode)
{
    (void) handle;
    (void) mode;
    return (Tcl_Channel) (void *) &io_files[0];  /* non-NULL: the Win32 path
                                                  * check_assertion()s on it */
}

/*===========================================================================
 * 11. access() - a libc hole, not a Tcl one
 *
 * newlib declares access() at sh-elf/include/sys/unistd.h:20 but NOTHING in
 * the toolchain defines it. Verified: nm --defined-only over every .a under
 * /opt/toolchains/dc finds no _access, and nm --undefined-only on libjim.a
 * leaves _access as the single unresolved symbol.
 *
 * jim-file.c:492 implements `file exists` as access(path, F_OK), and
 * courses/course_idx.tcl:62 uses `file exists`. jim-file.o is linked in
 * unconditionally by Jim_InitStaticExtensions, so this is a link-time blocker,
 * not a runtime one.
 *
 * stat() is enough: KOS's iso9660 mount is read-only and has no permission
 * bits, so any successful stat answers R_OK, W_OK, X_OK and F_OK alike.
 *=========================================================================*/

int access(const char *path, int mode)
{
    struct stat st;

    (void) mode;
    return (stat(path, &st) == 0) ? 0 : -1;
}
