#!/usr/bin/env python3
"""Regenerate dc/third_party/jim-list-backslash-newline.patch.

Runs INSIDE the SDK container, against the kos-ports libjimtcl source tree:

    docker run --rm --platform linux/arm64 -v $PWD:/work tuxracer-dc:sdk bash -c \\
      '. $KOS_BASE/environ.sh && make -C $KOS_PORTS/libjimtcl unpack && \\
       python3 /work/tools/gen-jim-patch.py > /work/dc/third_party/jim-list-backslash-newline.patch'

The patch itself is what the build applies; this script exists so the patch can
be regenerated against a newer libjimtcl instead of hand-edited, and so the
three anchors are asserted rather than assumed. It fails loudly if any anchor
stops matching.

Why the patch: see the comment it inserts, and kb/design-tcl.md.
"""

import os
import subprocess
import sys

SRC = os.environ.get(
    "JIM_SRC",
    "/opt/toolchains/dc/kos-ports/libjimtcl/build/libjimtcl-1.0.0/jim.c",
)

HELPER = r"""/* Tcl list syntax: a backslash-newline pair is WHITESPACE - an element
 * separator - not part of an element. Jim's list parser instead treats it as
 * an escape inside a word, so `{ a \<nl> b }` parses as three elements where
 * Tcl gives two.
 *
 * MEASURED on the same input (data/courses/course_idx.tcl:106, which is
 * written that way throughout): tclsh 8.6 reports llength 2, stock jimsh
 * reports 5. Without this the whole course list is garbage and the game exits
 * at boot with "No course specified in open course data".
 */
static int JimListIsBackslashNewline(struct JimParserCtx *pc)
{
    return pc->len >= 2 && pc->p[0] == '\\' && pc->p[1] == '\n';
}

"""

ANCHOR_PARSE_LIST = (
    "/* Parse the next token from a Tcl list string. */\n"
    "static int JimParseList(struct JimParserCtx *pc)\n"
    "{\n"
    "    if (isspace(UCHAR(*pc->p))) {\n"
    "        return JimParseListSep(pc);\n"
    "    }"
)

OLD_SEP = """static int JimParseListSep(struct JimParserCtx *pc)
{
    pc->tstart = pc->p;
    pc->tline = pc->linenr;
    while (isspace(UCHAR(*pc->p))) {
        if (*pc->p == '\\n') {
            pc->linenr++;
        }
        pc->p++;
        pc->len--;
    }"""

NEW_SEP = """static int JimParseListSep(struct JimParserCtx *pc)
{
    pc->tstart = pc->p;
    pc->tline = pc->linenr;
    while (pc->len) {
        if (isspace(UCHAR(*pc->p))) {
            if (*pc->p == '\\n') {
                pc->linenr++;
            }
            pc->p++;
            pc->len--;
        }
        else if (JimListIsBackslashNewline(pc)) {
            pc->linenr++;
            pc->p += 2;
            pc->len -= 2;
        }
        else {
            break;
        }
    }"""

OLD_STR = """    while (pc->len) {
        if (isspace(UCHAR(*pc->p))) {
            pc->tend = pc->p - 1;
            return JIM_OK;
        }
        if (*pc->p == '\\\\') {"""

NEW_STR = """    while (pc->len) {
        if (isspace(UCHAR(*pc->p)) || JimListIsBackslashNewline(pc)) {
            pc->tend = pc->p - 1;
            return JIM_OK;
        }
        if (*pc->p == '\\\\') {"""


def main():
    with open(SRC) as f:
        original = f.read()

    s = original
    for name, old in (("JimParseList", ANCHOR_PARSE_LIST),
                      ("JimParseListSep", OLD_SEP),
                      ("JimParseListStr", OLD_STR)):
        if old not in s:
            sys.exit("anchor for %s no longer matches %s - regenerate by hand"
                     % (name, SRC))

    s = s.replace(
        ANCHOR_PARSE_LIST,
        HELPER + ANCHOR_PARSE_LIST.replace(
            "if (isspace(UCHAR(*pc->p))) {",
            "if (isspace(UCHAR(*pc->p)) || JimListIsBackslashNewline(pc)) {"),
        1)
    s = s.replace(OLD_SEP, NEW_SEP, 1)
    s = s.replace(OLD_STR, NEW_STR, 1)

    with open("/tmp/jim.c.orig", "w") as f:
        f.write(original)
    with open("/tmp/jim.c.new", "w") as f:
        f.write(s)

    # diff exits 1 when files differ, which is the expected case here.
    p = subprocess.run(
        ["diff", "-u", "--label", "a/jim.c", "--label", "b/jim.c",
         "/tmp/jim.c.orig", "/tmp/jim.c.new"],
        capture_output=True, text=True)
    if p.returncode not in (0, 1):
        sys.exit("diff failed: %s" % p.stderr)
    sys.stdout.write(p.stdout)


if __name__ == "__main__":
    main()
