/* tcl.h - the slice of the Tcl 8.0 API that Tux Racer 0.61 actually uses,
 * declared over the Jim Tcl shim in dc/src/tcl_compat.c.
 *
 * src/tuxracer.h:110 does `#include TCL_HEADER`, and dc/include/config.h:52
 * points that at <tcl.h>. dc/include is first on the include path, so this
 * file shadows any real Tcl header. Nothing here includes <jimtcl/jim.h>:
 * src/ must not see Jim's names, and the two headers disagree about enough
 * (Jim_HashTable vs Tcl_HashTable layouts, result-object ownership) that
 * keeping them apart is what makes the shim reviewable.
 *
 * The API surface is exactly what `grep -rn 'Tcl_' src/` reports and no more.
 * Signatures are Tcl 8.0's, not 8.4's: src/ declares its command callbacks as
 * `int (ClientData, Tcl_Interp *, int, char *argv[])` (src/hier_cb.c:29) and
 * assigns Tcl_GetVar()'s result to a plain `char *` (src/game_config.c:150),
 * both of which the 8.4 CONST84 signatures would reject.
 *
 * Evidence for the mapping, and for what is stubbed: kb/design-tcl.md.
 */

#ifndef TR_DC_TCL_H
#define TR_DC_TCL_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Return codes. These are identical to Jim's (jim.h:132-136 defines JIM_OK 0,
 * JIM_ERR 1, JIM_RETURN 2), which is why the command bridge can pass most of
 * them through untranslated.
 */
#define TCL_OK          0
#define TCL_ERROR       1
#define TCL_RETURN      2
#define TCL_BREAK       3
#define TCL_CONTINUE    4

/* Variable flags. Only TCL_GLOBAL_ONLY is used (src/game_config.c:150 and
 * seven siblings); the shim always operates on globals regardless. */
#define TCL_GLOBAL_ONLY     1
#define TCL_NAMESPACE_ONLY  2
#define TCL_APPEND_VALUE    4
#define TCL_LIST_ELEMENT    8
#define TCL_LEAVE_ERR_MSG   0x200

/* Hash key types. src/ only ever asks for TCL_STRING_KEYS
 * (src/hier.c:417-418, src/hash.c:28) and the shim implements only that. */
#define TCL_STRING_KEYS     0
#define TCL_ONE_WORD_KEYS   1

/* Channel ids and modes. Referenced only inside the NATIVE_WIN32_COMPILER
 * block of src/tcl_util.c:140-170, which is dead code on this target. */
#define TCL_STDIN       0
#define TCL_STDOUT      1
#define TCL_STDERR      2
#define TCL_READABLE    (1<<1)
#define TCL_WRITABLE    (1<<2)

typedef void *ClientData;

/* Opaque to src/. Tcl_Interp * is really a Jim_Interp *; the cast lives in
 * dc/src/tcl_compat.c so that command callbacks receive back exactly the
 * pointer main.c:151 stored in g_game.tcl_interp. */
typedef struct Tcl_Interp    Tcl_Interp;
typedef struct Tcl_Obj       Tcl_Obj;
typedef struct Tcl_Command_ *Tcl_Command;
typedef struct Tcl_Channel_ *Tcl_Channel;

typedef int  (Tcl_CmdProc)       (ClientData clientData, Tcl_Interp *interp,
                                  int argc, char *argv[]);
typedef void (Tcl_CmdDeleteProc) (ClientData clientData);
typedef void (Tcl_FreeProc)      (char *blockPtr);

#define TCL_STATIC      ((Tcl_FreeProc *) 0)
#define TCL_VOLATILE    ((Tcl_FreeProc *) 1)
#define TCL_DYNAMIC     ((Tcl_FreeProc *) 3)

/*
 * Hash table.
 *
 * These structs must be COMPLETE, not opaque: src/hier.c:28-29 declares two
 * Tcl_HashTable by value, src/hash.c:27 does malloc(sizeof(Tcl_HashTable)),
 * src/hash.c:90 declares a Tcl_HashSearch by value, and src/hash.c:119 reaches
 * into it for `.tablePtr`. The layout below is ours, not Tcl's — nothing in
 * src/ touches any other field.
 */
typedef struct Tcl_HashEntry {
    struct Tcl_HashEntry *nextPtr;      /* next entry in this bucket */
    struct Tcl_HashTable *tablePtr;     /* owner; Tcl_DeleteHashEntry needs it */
    unsigned int          hash;         /* cached, so growth need not rehash */
    ClientData            clientData;
    char                  key[1];       /* over-allocated to strlen(key)+1 */
} Tcl_HashEntry;

typedef struct Tcl_HashTable {
    Tcl_HashEntry **buckets;            /* == staticBuckets until first growth */
    Tcl_HashEntry  *staticBuckets[16];
    int             numBuckets;
    int             numEntries;
    int             rebuildSize;
    unsigned int    mask;
    int             keyType;
} Tcl_HashTable;

typedef struct Tcl_HashSearch {
    Tcl_HashTable *tablePtr;            /* read by src/hash.c:119 */
    int            nextIndex;
    Tcl_HashEntry *nextEntryPtr;        /* fetched ahead, so the caller may
                                           delete the entry it was just given */
} Tcl_HashSearch;

/* Macros in real Tcl, so they stay macros here: src/hash.c:44 and
 * src/hier.c:72 use Tcl_SetHashValue as a statement with a typed pointer. */
#define Tcl_GetHashValue(h)         ((h)->clientData)
#define Tcl_SetHashValue(h, value)  ((h)->clientData = (ClientData)(value))
#define Tcl_GetHashKey(tablePtr, h) ((void)(tablePtr), (h)->key)

/* interpreter */
Tcl_Interp *Tcl_CreateInterp    (void);
int         Tcl_InterpDeleted   (Tcl_Interp *interp);
int         Tcl_EvalFile        (Tcl_Interp *interp, const char *fileName);

/* commands */
Tcl_Command Tcl_CreateCommand   (Tcl_Interp *interp, const char *cmdName,
                                 Tcl_CmdProc *proc, ClientData clientData,
                                 Tcl_CmdDeleteProc *deleteProc);

/* result */
void        Tcl_AppendResult    (Tcl_Interp *interp, ...);
const char *Tcl_GetStringResult (Tcl_Interp *interp);
void        Tcl_SetObjResult    (Tcl_Interp *interp, Tcl_Obj *objPtr);

/* result objects. No interp argument, matching Tcl 8.0 — see kb/design-tcl.md
 * §5 for how these are made to work without one. */
Tcl_Obj    *Tcl_NewStringObj    (const char *bytes, int length);
Tcl_Obj    *Tcl_NewIntObj       (int intValue);
Tcl_Obj    *Tcl_NewBooleanObj   (int boolValue);

/* value conversion */
int         Tcl_GetInt          (Tcl_Interp *interp, const char *src, int *intPtr);
int         Tcl_GetDouble       (Tcl_Interp *interp, const char *src, double *doublePtr);
int         Tcl_GetBoolean      (Tcl_Interp *interp, const char *src, int *boolPtr);

/* lists. Tcl_SplitList's argv is NUL-terminated (argv[*argcPtr] == NULL) and
 * is one allocation; free it with Tcl_Free and nothing else. */
int         Tcl_SplitList       (Tcl_Interp *interp, const char *list,
                                 int *argcPtr, char ***argvPtr);
void        Tcl_Free            (char *ptr);

/* variables */
char       *Tcl_GetVar          (Tcl_Interp *interp, const char *varName,
                                 int flags);
char       *Tcl_SetVar          (Tcl_Interp *interp, const char *varName,
                                 const char *newValue, int flags);

/* hash table */
void            Tcl_InitHashTable   (Tcl_HashTable *tablePtr, int keyType);
void            Tcl_DeleteHashTable (Tcl_HashTable *tablePtr);
Tcl_HashEntry  *Tcl_CreateHashEntry (Tcl_HashTable *tablePtr, const char *key,
                                     int *newPtr);
Tcl_HashEntry  *Tcl_FindHashEntry   (Tcl_HashTable *tablePtr, const char *key);
void            Tcl_DeleteHashEntry (Tcl_HashEntry *entryPtr);
Tcl_HashEntry  *Tcl_FirstHashEntry  (Tcl_HashTable *tablePtr,
                                     Tcl_HashSearch *searchPtr);
Tcl_HashEntry  *Tcl_NextHashEntry   (Tcl_HashSearch *searchPtr);

/* channels - no-ops, see kb/design-tcl.md §6 */
void        Tcl_SetStdChannel   (Tcl_Channel channel, int type);
Tcl_Channel Tcl_MakeFileChannel (ClientData handle, int mode);

#ifdef __cplusplus
}
#endif

#endif /* TR_DC_TCL_H */
