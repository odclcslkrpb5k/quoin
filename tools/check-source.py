#!/usr/bin/env python3
"""Source rules the compiler can't state, run by ./build check.

  check-source.py            check src/ and tests/
  check-source.py self-test  prove each rule refuses its bad examples and passes its good ones

Rules:
  nodiscard   a function that reports failure through GError ** and returns a value is
              [[nodiscard]]: a declaration in a header, or a static function. (An exported
              function's definition is covered by its header's declaration.)
  discarded   a call passes NULL for a GError ** parameter and also discards its return
              value: the failure leaves no trace at all. (NULL is fine where the return
              value says what failed.) Which parameters are GError ** comes from the
              preprocessed prelude (every GLib and GTK prototype) and our own headers.
  suppression no local overrides: #pragma GCC/clang diagnostic, NOLINT, cppcheck-suppress,
              and no (void) or (void) ! cast on a call, which silences [[nodiscard]] and
              warn_unused_result. Exceptions are listed in VOID_CASTS below, per file, with
              their exact count and their reason, so one can't be added in passing.
  logpath     a log or abort message (g_debug ... g_critical, g_log, gbk_fatal) carries no
              path, folder, URI or display name: users paste logs into public bug
              reports, and a path holds their user name (DP-3). Matched by the argument's
              name (path, dir, uri, filename, display, dest, src, root, location). src/
              only: the tests' messages name their own temporary folders.
  message     no branching on an error's message text (ER-8): ->message is not an
              argument of a comparison or search (g_str_equal, strcmp, g_str_has_prefix,
              strstr, g_strstr_len, g_regex_match_simple, ...). Branch on domain and code.
  hidden      no literal hidden characters (no-break and zero-width spaces, bidirectional
              controls, line and paragraph separators) in source: write them as escapes
              (CD-7). GCC rejects the bidirectional ones itself; this covers the rest, in
              every file the compiler doesn't read too (meson.build, scripts).
  locpath     gbk_loc_path() (a location's path on its own machine, which may not be this
              one) is called only where a location's path is shown or new locations are
              built from it: LOC_PATH_FILES. Everywhere else a path for a file API comes from
              gbk_loc_as_local(), which refuses locations on other machines (both machines
              have the same layout, so a leaked remote path names a real local file).
  unsequenced one argument of a call hands &x to a nested call while another argument reads
              x: the order the arguments are evaluated in is unspecified, so x may be read
              before the nested call has written it. No compiler warning or analyzer here
              catches it (GCC -Wsequence-point, Clang -Wunsequenced, clang-tidy, cppcheck).
              Likewise two calls to a stream reader (a function whose first parameter is a
              `Reader *`, which advances it) in one initializer or one argument list: their
              order is unspecified too, so fields come out swapped (proto.c, 2026-10-17).

The matching is textual (comments removed, then regular expressions), so it is written to
fail closed: a declaration it can't read is not silently passed, it is simply not seen.
That gap is what the self-test's examples pin down.
"""

import glob
import os
import re
import subprocess
import sys

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
# return type and qualifiers, function name, a parameter list holding GError **, ; or {
FUNCTION = re.compile(
    r"(?:^|[;}\n])\s*((?:\[\[nodiscard\]\]\s*)?(?:static\s+)?(?:inline\s+)?"
    r"[A-Za-z_][A-Za-z_0-9 \t\n*]*?)\b([a-z_][a-z_0-9]*)\s*\(([^;{)]*\bGError\s*\*\*[^;{)]*)\)\s*([;{])")


def nodiscard(path, text):
    problems = []
    header = path.endswith(".h")
    for m in FUNCTION.finditer(COMMENT.sub(" ", text)):
        ret = " ".join(m.group(1).split())
        name = m.group(2)
        if ret in ("", "return", "else") or ret.startswith("typedef") or ret.endswith(" void") \
                or ret == "void" or ret.startswith("extern"):
            continue
        if not header and not ret.startswith("[[nodiscard]] static") and "static" not in ret:
            continue
        if "[[nodiscard]]" not in ret:
            problems.append(f"{path}: {name} reports failure through GError ** but isn't "
                            "[[nodiscard]]")
    return problems


def top_level_args(text, start):
    """The arguments of the call whose '(' is at `start`, split at top-level commas, and the
    index just past its ')'; None if unbalanced."""
    depth, args, begin = 0, [], start + 1
    for i in range(start, len(text)):
        ch = text[i]
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
            if depth == 0:
                args.append(text[begin:i])
                return args, i + 1
        elif ch == "," and depth == 1:
            args.append(text[begin:i])
            begin = i + 1
    return None


LVALUE = r"[A-Za-z_][A-Za-z_0-9]*(?:(?:->|\.)[A-Za-z_][A-Za-z_0-9]*)*"
STRING = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')


def unsequenced(path, text):
    problems = []
    code = STRING.sub('""', COMMENT.sub(" ", text))
    for m in re.finditer(r"\b[A-Za-z_][A-Za-z_0-9]*\s*\(", code):
        parsed = top_level_args(code, m.end() - 1)
        if parsed is None:
            continue
        args = parsed[0]
        for i, arg in enumerate(args):
            # &x inside a nested call in this argument (an out-parameter being written)
            for out in re.findall(r"\(\s*[^()]*&\s*(" + LVALUE + ")", arg):
                read = re.compile(r"(?<![&\w>.])" + re.escape(out) + r"(?![\w(])")
                for j, other in enumerate(args):
                    # sizeof's operand is not evaluated: not a read
                    other = re.sub(r"\bsizeof\s*\(?\s*" + LVALUE + r"\s*\)?", "sizeof ()", other)
                    if j != i and read.search(other):
                        line = code.count("\n", 0, m.start()) + 1
                        problems.append(f"{path}:{line}: {out} is written through &{out} in one "
                                        "argument and read in another; call first, then read")
    problems += reader_order(path, code)
    return sorted(set(problems))


READER_FN = re.compile(r"\b([a-z_][a-z_0-9]*)\s*\(\s*(?:const\s+)?Reader\s*\*")
LIST_START = re.compile(r"=\s*\{|\(\s*(?:const\s+)?[A-Z][A-Za-z_0-9]*\s*\)\s*\{|\b[A-Za-z_][A-Za-z_0-9]*\s*\(")


def reader_order(path, code):
    readers = set(READER_FN.findall(code))
    if not readers:
        return []
    call = re.compile(r"\b(" + "|".join(sorted(readers)) + r")\s*\(")
    problems = []
    for m in LIST_START.finditer(code):
        parsed = top_level_args(code, m.end() - 1)
        if parsed is None:
            continue
        with_reader = [i for i, el in enumerate(parsed[0]) if call.search(el)]
        if len(with_reader) > 1:
            line = code.count("\n", 0, m.start()) + 1
            problems.append(f"{path}:{line}: two stream reads in one initializer or argument "
                            "list: their order is unspecified; read into variables first")
    return problems


CALL = re.compile(r"\b([A-Za-z_][A-Za-z_0-9]*)\s*\(")
GERROR_PARAM = re.compile(r"\bGError\s*\*\s*\*")


def gerror_params(text):
    """{function: positions of its GError ** parameters}, from declarations in `text`."""
    table = {}
    for m in CALL.finditer(text):
        parsed = top_level_args(text, m.end() - 1)
        if parsed is not None:
            for i, arg in enumerate(parsed[0]):
                if GERROR_PARAM.search(arg):
                    table.setdefault(m.group(1), set()).add(i)
    return table


def prototypes():
    """Every GLib, GTK and project function taking GError **, as the compiler sees them."""
    flags = subprocess.run(["pkg-config", "--cflags", "gtk+-3.0"], capture_output=True,
                           text=True, check=True).stdout.split()
    builds = sorted(glob.glob("out/*/gbk-config.h"))
    if not builds:
        sys.exit("check-source: no configured build (gbk-config.h) to preprocess the prelude with")
    pre = subprocess.run(["gcc", "-E", "-P", "-std=gnu23", "-D_GNU_SOURCE",
                          "-I" + builds[0].rsplit("/", 1)[0]] + flags
                         + ["-include", "src/prelude.h", "-x", "c", "/dev/null"],
                         capture_output=True, text=True)
    if pre.returncode != 0:
        sys.exit("check-source: preprocessing the prelude failed\n" + pre.stderr[-2000:])
    ours = "".join(COMMENT.sub(" ", open(h).read()) for h in sorted(glob.glob("src/*.h")))
    table = gerror_params(pre.stdout + ours)
    if len(table) < 100:
        sys.exit(f"check-source: only {len(table)} functions take GError **; the prelude parse broke")
    return table


GERROR_TABLE = {}


def discarded(path, text):
    problems = []
    code = STRING.sub('""', COMMENT.sub(" ", text))
    for m in CALL.finditer(code):
        params = GERROR_TABLE.get(m.group(1))
        if not params:
            continue
        parsed = top_level_args(code, m.end() - 1)
        if parsed is None:
            continue
        args = [a.strip() for a in parsed[0]]
        if not any(i < len(args) and args[i] in ("nullptr", "NULL") for i in params):
            continue
        before = code[:m.start()].rstrip()
        after = code[parsed[1]:].lstrip()
        if (before == "" or before[-1] in ";{}" or before.endswith("(void)")) and after.startswith(";"):
            line = code.count("\n", 0, m.start()) + 1
            problems.append(f"{path}:{line}: {m.group(1)} with no GError and its result discarded:"
                            " a failure here leaves no trace")
    return problems


# Files allowed (void) casts on calls: exact count, and why.
VOID_CASTS = {
    "src/fileops.c": (6, "copying keeps owner, xattrs, mode and times as far as the "
                         "destination allows, as cp -a does: each failure is expected and "
                         "the copy itself has already succeeded"),
    "src/window.c": (1, "a device action for an id that names no device (a stale menu or a "
                        "stray D-Bus activation) does nothing"),
}
VOID_CAST = re.compile(r"\(\s*void\s*\)\s*!?\s*[A-Za-z_][A-Za-z_0-9]*\s*\(")
SUPPRESSION = re.compile(r"#\s*pragma\s+(GCC|clang)\s+diagnostic|\bNOLINT|cppcheck-suppress")


def suppression(path, text):
    problems = []
    for m in SUPPRESSION.finditer(text):
        line = text.count("\n", 0, m.start()) + 1
        problems.append(f"{path}:{line}: suppression ({m.group(0)}): put the exception in the "
                        "tool's configuration, scoped to its files")
    code = STRING.sub('""', COMMENT.sub(" ", text))
    casts = [code.count("\n", 0, m.start()) + 1 for m in VOID_CAST.finditer(code)]
    allowed = VOID_CASTS.get(path, (0, ""))[0]
    if len(casts) != allowed:
        where = ", ".join(str(n) for n in casts)
        problems.append(f"{path}: {len(casts)} (void) casts on calls (lines {where or 'none'}), "
                        f"{allowed} allowed by VOID_CASTS in tools/check-source.py")
    return problems


LOG_CALL = re.compile(r"\b(g_debug|g_info|g_message|g_warning|g_critical|g_log|gbk_fatal)\s*\(")
PATHY = re.compile(r"(?<![A-Za-z_0-9])(?:[A-Za-z_0-9]+(?:->|\.))*"
                   r"(path|dir|uri|filename|display|dest|src|root|location)\b")


def logpath(path, text):
    problems = []
    if not path.startswith("src/"):
        return problems                # tests log their own temporary folders
    code = STRING.sub('""', COMMENT.sub(" ", text))
    for m in LOG_CALL.finditer(code):
        parsed = top_level_args(code, m.end() - 1)
        if parsed is None:
            continue
        for arg in parsed[0][1:]:
            hit = PATHY.search(arg)
            if hit:
                line = code.count("\n", 0, m.start()) + 1
                problems.append(f"{path}:{line}: {m.group(1)} logs {hit.group(0)}: logs name "
                                "kinds and codes, never paths (they hold the user's name)")
    return problems


COMPARE_CALL = re.compile(r"\b(g_str_equal|strcmp|strncmp|g_strcmp0|g_str_has_prefix|"
                          r"g_str_has_suffix|strstr|g_strstr_len|g_strrstr|strcasecmp|"
                          r"g_ascii_strcasecmp|g_regex_match_simple|g_pattern_match_simple)\s*\(")


def message(path, text):
    problems = []
    code = COMMENT.sub(" ", text)
    for m in COMPARE_CALL.finditer(code):
        parsed = top_level_args(code, m.end() - 1)
        if parsed is not None and any("->message" in a for a in parsed[0]):
            line = code.count("\n", 0, m.start()) + 1
            problems.append(f"{path}:{line}: {m.group(1)} on an error's message: branch on "
                            "its domain and code (g_error_matches)")
    return problems


LOC_PATH_CALL = re.compile(r"\bgbk_loc_path\s*\(")
LOC_PATH_FILES = {
    "src/loc.c": "defines it",
    "src/loc.h": "declares it",
    "src/pathbar.c": "splits the shown location into buttons and builds their locations",
    "src/dirmodel.c": "asks the other machine to watch its own folder (gbk_conn_watch)",
    "src/open.c": "hands another machine's folder to ssh, hex-encoded (gbk_conn_shell_argv)",
    "src/props.c": "asks the other machine for its own file's details (gbk_conn_info)",
    "tests/test-loc.c": "tests it",
    "tests/test-remotewin.c": "checks which folder on the other machine a window shows",
}


def locpath(path, text):
    if path in LOC_PATH_FILES:
        return []
    code = STRING.sub('""', COMMENT.sub(" ", text))
    return [f"{path}:{code.count(chr(10), 0, m.start()) + 1}: gbk_loc_path outside "
            "LOC_PATH_FILES: get a path for file APIs from gbk_loc_as_local"
            for m in LOC_PATH_CALL.finditer(code)]


HIDDEN = re.compile("[\u00a0\u00ad\u180e\u2000-\u200f\u2028-\u202f\u205f-\u206f\ufeff]")


def hidden(path, text):
    return [f"{path}:{text.count(chr(10), 0, m.start()) + 1}: hidden character "
            f"U+{ord(m.group(0)):04X}: write it as an escape"
            for m in HIDDEN.finditer(text)]


RULES = [nodiscard, unsequenced, discarded, suppression, logpath, message, hidden, locpath]
# Declarations of GLib's own functions, for the analyzer: not ours to mark.
EXEMPT = {"src/analyzer-glib.h"}


def check():
    GERROR_TABLE.update(prototypes())
    problems = []
    files = sorted(glob.glob("src/*.[ch]") + glob.glob("tests/*.[ch]") + glob.glob("tests/*/*.[ch]"))
    other = sorted(glob.glob("meson.build") + glob.glob("*/meson.build") + glob.glob("tools/*")
                   + ["build", "install.sh", "Dockerfile", "compose.yaml", ".clang-tidy"])
    if len(files) < 20:
        sys.exit(f"check-source: found only {len(files)} files; run from the repository root")
    for path in files:
        if path in EXEMPT:
            continue
        text = open(path, encoding="utf-8").read()
        for rule in RULES:
            problems += rule(path, text)
    for path in (p for p in other if os.path.isfile(p)):
        problems += hidden(path, open(path, encoding="utf-8").read())
    for p in problems:
        print("  " + p)
    if problems:
        sys.exit(f"check-source: {len(problems)} problems")
    print(f"check-source: {len(files)} files, {len(RULES)} rules, no problems")


# (rule, file name, source, expected number of problems)
EXAMPLES = [
    (nodiscard, "x.h", "bool f (const char *p, GError **error);", 1),
    (nodiscard, "x.h", "[[nodiscard]] bool f (const char *p, GError **error);", 0),
    (nodiscard, "x.h", "GbkThing *f (int a,\n           GError **error);", 1),
    (nodiscard, "x.h", "void f (GError **error);", 0),
    (nodiscard, "x.h", "/* bool f (GError **error); */", 0),
    (nodiscard, "x.h", "typedef bool (*Fn) (GError **error);", 0),
    (nodiscard, "x.c", "static bool\nf (GError **error)\n{\n  return true;\n}", 1),
    (nodiscard, "x.c", "[[nodiscard]] static bool\nf (GError **error)\n{\n  return true;\n}", 0),
    (nodiscard, "x.c", "bool\ngbk_f (GError **error)\n{\n  return true;\n}", 0),
    (nodiscard, "x.c", "  if (f (&error))\n    return g (x, &error);", 0),
    (unsequenced, "x.c", "agree (gbk_ops_run (a, &done->error), done->error);", 1),
    (unsequenced, "x.c", "agree (run (&e), e);", 1),
    (unsequenced, "x.c", "show (parse (s, &n), n + 1);", 1),
    (unsequenced, "x.c", "ok = run (&e);\nagree (ok, e);", 0),
    (unsequenced, "x.c", "if (!f (a, &error))\n  show (error->message);", 0),
    (unsequenced, "x.c", "g (f (&e), e2);", 0),
    (unsequenced, "x.c", "g (f (&d->e), d->error);", 0),
    (unsequenced, "x.c", "static int get_u8 (Reader *r);\nT t = { .a = get_u8 (r), .b = get_u8 (r) };", 1),
    (unsequenced, "x.c", "static int get_u8 (Reader *r);\nT t = (T) { get_u8 (r), 0, get_u8 (r) };", 1),
    (unsequenced, "x.c", "static int get_u8 (Reader *r);\nshow (get_u8 (r), get_u8 (r));", 1),
    (unsequenced, "x.c", "static int get_u8 (Reader *r);\nint a = get_u8 (r);\nint b = get_u8 (r);\nT t = { .a = a, .b = b };", 0),
    (unsequenced, "x.c", "static int get_u8 (Reader *r);\nT t = { .a = get_u8 (r), .b = 2 };", 0),
    (unsequenced, "x.c", "int get_u8 (int *r);\nshow (get_u8 (r), get_u8 (r));", 0),
    (unsequenced, "x.c", "g (&e, f (e));", 0),
    (unsequenced, "x.c", "g (f (\"&e\"), e);", 0),
    (unsequenced, "x.c", "check (read (fd, &v, sizeof v), sizeof v);", 0),
    (unsequenced, "x.c", "check (read (fd, &v, sizeof (v)), v);", 1),
    (discarded, "x.c", "  load (path, nullptr);", 1),
    (discarded, "x.c", "{ load (path, NULL); }", 1),
    (discarded, "x.c", "(void) load (path, nullptr);", 1),
    (discarded, "x.c", "  if (!load (path, nullptr))\n    return;", 0),
    (discarded, "x.c", "  ok = load (path, nullptr);", 0),
    (discarded, "x.c", "  load (path, &error);", 0),
    (discarded, "x.c", "  g_autofree char *s = read (0, 1, nullptr);", 0),
    (discarded, "x.c", "  return load (path, nullptr);", 0),
    (suppression, "x.c", "#pragma GCC diagnostic ignored \"-Wshadow\"", 1),
    (suppression, "x.c", "# pragma clang diagnostic push", 1),
    (suppression, "x.c", "int x = f (); // NOLINT", 1),
    (suppression, "x.c", "// cppcheck-suppress nullPointer\nint x;", 1),
    (suppression, "x.c", "  (void) load (p, &e);", 1),
    (suppression, "x.c", "  (void) !write (fd, b, n);", 1),
    (suppression, "x.c", "  (void) 0;\n  (void) unused_param;", 0),
    (suppression, "x.c", "  x = (void *) f (y);", 0),
    (suppression, "x.c", "  /* (void) f (); */", 0),
    (logpath, "src/x.c", "g_warning (\"readdir %s: %s\", s->path, g_strerror (errno));", 1),
    (logpath, "src/x.c", "gbk_fatal (\"no button for %s\", dir);", 1),
    (logpath, "src/x.c", "g_debug (\"opened %s\", it->display);", 1),
    (logpath, "src/x.c", "g_warning (\"readdir: %s\", g_strerror (errno));", 0),
    (logpath, "src/x.c", "gbk_fatal (\"bad kind %d\", (int) kind);", 0),
    (logpath, "src/x.c", "g_warning (\"no path here\");", 0),
    (logpath, "tests/x.c", "gbk_fatal (\"opendir %s\", dir);", 0),
    (locpath, "src/window.c", "open (gbk_loc_path (loc), O_RDONLY);", 1),
    (locpath, "tests/x.c", "const char *p = gbk_loc_path(loc);", 1),
    (locpath, "src/pathbar.c", "const char *rest = gbk_loc_path (bar->deep);", 0),
    (locpath, "src/window.c", "/* gbk_loc_path (loc) */ g_print (\"gbk_loc_path (x)\");", 0),
    (locpath, "src/window.c", "if (gbk_loc_as_local (loc, &p)) open (p, O_RDONLY);", 0),
    (message, "x.c", "if (g_str_has_prefix (error->message, \"No such\"))", 1),
    (message, "x.c", "if (strcmp (e->message, m) == 0)", 1),
    (message, "x.c", "if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))", 0),
    (message, "x.c", "label = g_strdup (error->message);", 0),
    (hidden, "x.c", "const char *s = \"a\u00a0b\";", 1),
    (hidden, "x.c", "/* a\u200bb */", 1),
    (hidden, "x.c", "const char *s = \"a\\u00a0b\";", 0),
]


def self_test():
    GERROR_TABLE.update(gerror_params("bool load (const char *p, GError **error);\n"
                                       "char *read (int fd, int n, GError **error);"))
    failed = 0
    for rule, name, source, expected in EXAMPLES:
        got = len(rule(name, source))
        if got != expected:
            failed += 1
            print(f"  {rule.__name__}: expected {expected}, got {got}: {source!r}")
    if failed:
        sys.exit(f"check-source self-test: {failed} of {len(EXAMPLES)} examples wrong")
    print(f"check-source self-test: {len(EXAMPLES)} examples")


if __name__ == "__main__":
    {"self-test": self_test}.get(sys.argv[1] if len(sys.argv) > 1 else "", check)()
