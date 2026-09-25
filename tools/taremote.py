#!/usr/bin/env python3
"""taremote — a Windows machine over SSH, for tacli's remote instances.

Every channel between tacli and the DLL is a file in the game folder (the key and
eye files, the lever files, the trigger/result pairs, the `.ab` captures, `log\\`),
and the DLL on Windows reads and writes the same files. So a remote instance is a
game folder on another machine plus a way to read and write files there, list its
processes, start a scheduled task and save the registry. This module is that way;
tacli routes its verbs through it (`research/notes/tacli-design.md`, "Remote
instances").

THE TRANSPORT is one PowerShell per tacli command: `ssh <user>@<host> powershell
-NoProfile -NonInteractive -Command -`, fed statements on stdin and kept open, so a
statement costs one round trip instead of a PowerShell start-up. The remote login
shell is PowerShell with messages in the machine's own language, and PowerShell
reading stdin has one hazard that shapes everything below: A STATEMENT SPANNING TWO
LINES IS SKIPPED, SILENTLY. So:

  * every script is made by ONE function, `ps_script`, which refuses any statement
    that could continue onto a second line (`ps_check_statement`);
  * every statement it emits runs only if the one before it ran and succeeded (a
    sequence counter), and must print a DONE marker; `Session.run` fails when one is
    missing -- a line PowerShell did not execute is an error here, not an empty
    answer, and nothing after it runs;
  * every string reaches PowerShell as a single-quoted literal or as base64
    (`ps_str`), so no path, quote or non-ASCII character can open a string that
    runs past the end of its line.

Stdlib only, like tacli.
"""

import base64
import hashlib
import ntpath
import os
import queue
import re
import secrets
import subprocess
import threading
import time
from pathlib import Path

# TA's registry key: what a remote launch exports first and restores at stop.
REG_KEY = r"HKCU\Software\Cavedog Entertainment\Total Annihilation"
REG_PSPATH = r"HKCU:\Software\Cavedog Entertainment\Total Annihilation"

STATE_DIR = "tacli-state"                 # tacli's own files inside the test folder
MARKER = "tacli-test-folder.txt"          # written by `remote add` first; `rm` requires it
COPIED = "copied.txt"                     # `<SHA-256> <relative path>`, a line per copied file
WRAPPER = "launch.ps1"                    # the scheduled task's script (`Remote.wrapper_script`)
WRAPPER_LOG = "launch.status"             # its marker lines: nobody reads its stdout

# The registry record, ONE PER USER ON THE MACHINE, beside no test folder: TA's key is
# in HKCU, which every test folder of that user shares, so a record per instance would
# let a second launch export the first one's test values as "the original".
HOME_DIR = "tacli"                        # under %LOCALAPPDATA%
PENDING = "registry-pending.txt"          # exists from before the export until after the verified restore
REG_EXPORT = "registry-before.reg"
REG_CHECK = "registry-after.reg"

# NO FILE OF THE PLAYER'S COPY IS REPLACED OR DELETED WITHOUT ITS ORIGINAL BESIDE IT
# (`RemotePath._guard`). Two tiers:
#   * PROTECTED, the files a launch replaces: `remote add` records each one's original
#     state up front -- a copy (`.tacli-original`) or, when the player's folder had
#     none, an empty `.tacli-absent` -- and nothing touches one while that record is
#     missing;
#   * every other file listed in COPIED: the first replace or delete makes the
#     `.tacli-original` itself, from whichever of the test folder's copy and the
#     player's file still has the SHA-256 `remote add` recorded, and refuses when
#     neither does. The game rewrites some copied files itself (`tagpu_vk.gpus` at
#     every start), so "tacli has not written it yet" does not make a file original;
#     the recorded hash does.
PROTECTED = ("ddraw.dll", "impure.cfg", "totala.ini")
BACKUP = ".tacli-original"
ABSENT = ".tacli-absent"

CHUNK = 1 << 20          # bytes per statement when a file crosses the link
HEAD_BYTES = 256         # talog.HEAD_BYTES: a log file's header line is inside this


class PSScriptError(ValueError):
    """A statement `ps_script` refuses: PowerShell would not run it as one line."""


class RemoteError(RuntimeError):
    """A statement ran and failed on the remote machine, or the link failed."""

    def __init__(self, msg, kind=""):
        super().__init__(msg)
        self.kind = kind            # the .NET exception's type name, when there is one


# ------------------------------------------------------------ the script rule

def ps_str(s) -> str:
    """A PowerShell expression whose value is the string `s`, on one line.

    Printable ASCII becomes a single-quoted literal, in which PowerShell expands
    nothing ($, backticks and double quotes are plain characters) and a quote is
    written twice. Anything else -- a non-ASCII path, a control character --
    travels as base64 and is decoded on the far side, because PowerShell reads its
    stdin in the console's code page and a raw newline would end the statement.
    """
    s = str(s)
    if all(" " <= c <= "~" for c in s):
        return "'" + s.replace("'", "''") + "'"
    b64 = base64.b64encode(s.encode("utf-8")).decode("ascii")
    return f"([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('{b64}')))"


def ps_b64(expr: str) -> str:
    """A PowerShell expression printing `expr`'s value as base64 of its UTF-8. Every
    name or path the far side sends back goes this way: stdout is in the console's
    code page, where a non-ASCII path arrives mangled and then matches nothing."""
    return f"([Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes('' + ({expr}))))"


def _unb64(s: str) -> str:
    return base64.b64decode(s.strip()).decode("utf-8")


_B64_NAME = ps_b64("$_.Name")

_OPEN = {"(": ")", "[": "]", "{": "}"}
_CLOSE = {v: k for k, v in _OPEN.items()}


def ps_check_statement(s: str) -> None:
    """Refuse a statement that could span lines, or that tacli never needs.

    PowerShell reading a script from stdin runs it line by line. A line whose
    brackets or quotes are still open, or that ends in a pipe or a comma, makes it
    read the NEXT line as the rest of the statement -- and in `-Command -` mode such
    a statement is then skipped with no output and no error. So a statement here is
    one line that closes everything it opens:

      * no control characters (a newline or CR splits it) and no non-ASCII (the
        console code page would mangle it; `ps_str` carries such text);
      * single-quoted strings only, closed on the line: a double-quoted string
        expands `$` and backticks, and tacli has no use for either;
      * no backtick (it escapes the newline: an explicit continuation), no `#`
        (a `<#` comment spans lines, and a comment is never needed), no here-string;
      * brackets balanced and correctly nested;
      * not ending in `|` or `,`, which continue onto the next line.

    What this cannot see -- `if ($x)` with its block missing, say -- is caught at
    run time: every statement must print its DONE marker (`ps_script`).
    """
    if not isinstance(s, str) or not s.strip():
        raise PSScriptError("an empty statement: a blank line ends the script")
    for c in s:
        if c < " " and c != "\t" or c == "\x7f":
            raise PSScriptError(f"a control character ({ord(c):#04x}) would split or "
                                f"corrupt the line: {s[:60]!r}")
        if c > "~":
            raise PSScriptError(f"non-ASCII {c!r}: PowerShell reads stdin in the console "
                                f"code page -- pass text through ps_str")
    stack = []
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        if c == "'":
            j = i + 1
            while True:
                j = s.find("'", j)
                if j < 0:
                    raise PSScriptError(f"an unclosed single-quoted string continues on "
                                        f"the next line: {s[:60]!r}")
                if j + 1 < n and s[j + 1] == "'":
                    j += 2                  # '' is a quote inside the string
                    continue
                break
            i = j + 1
            continue
        if c == '"':
            raise PSScriptError(f"a double-quoted string expands $ and backticks; use "
                                f"ps_str: {s[:60]!r}")
        if c == "`":
            raise PSScriptError(f"a backtick continues the statement on the next line: "
                                f"{s[:60]!r}")
        if c == "#":
            raise PSScriptError(f"a comment (and <# spans lines): {s[:60]!r}")
        if c == "@" and i + 1 < n and s[i + 1] in "'\"":
            raise PSScriptError(f"a here-string spans lines: {s[:60]!r}")
        if c in _OPEN:
            stack.append(c)
        elif c in _CLOSE:
            if not stack or stack[-1] != _CLOSE[c]:
                raise PSScriptError(f"an unmatched {c!r}: {s[:60]!r}")
            stack.pop()
        i += 1
    if stack:
        raise PSScriptError(f"an unclosed {stack[-1]!r} continues the statement on the "
                            f"next line: {s[:60]!r}")
    if s.rstrip()[-1] in "|,":
        raise PSScriptError(f"a trailing {s.rstrip()[-1]!r} continues the statement on "
                            f"the next line: {s[:60]!r}")


def ps_script(statements, token: str, batch: int, sink: "str | None" = None) -> str:
    """THE one place a PowerShell script is made: one statement a line, a blank line
    at the end.

    STATEMENT i RUNS ONLY IF STATEMENT i-1 RAN AND SUCCEEDED, by a sequence counter:
    line i is guarded by `$__seq -eq i` and sets `$__seq = i+1` only after its
    statement returned. A line PowerShell skipped -- it did not parse as one line --
    ran nothing, so it never advanced the counter, and no line after it runs either.
    A failure sets the counter to -1, which no line matches.

    Each line also resets `$ErrorActionPreference` to 'Stop' first (a statement that
    lowers it around a native command cannot leave it lowered for the next one),
    prints `DONE <batch> <i>` after its statement succeeded, and on a failure prints
    `ERR <batch> <i> <type> <message>`. The type and message are the INNERMOST
    exception's: PowerShell wraps a .NET method's exception (a missing file reads as
    MethodInvocationException around FileNotFoundException), and the caller keys on
    the inner one. The message travels in base64: stdout is in the console's code page
    and the message in the machine's language. `Session.run` fails on an ERR, and on a
    statement with no DONE -- the one a skipped line leaves.

    `token` makes the markers unforgeable by anything a statement prints; `batch`
    numbers every marker, so a late line of an earlier batch is never read as this
    one's. `sink`, a PowerShell expression naming a file, makes every marker line be
    appended to that file as well: a script that runs with nobody reading its stdout
    (the launch wrapper) reports through it. Every line is checked, the generated
    ones too: the rule is about what reaches PowerShell, not about what the caller
    meant.
    """
    if not re.fullmatch(r"[0-9a-f]{8,32}", token or ""):
        raise PSScriptError("the marker token must be 8-32 lowercase hex digits")
    tag, b = f"@@{token}", int(batch)
    emit = "Write-Output $t" if sink is None else (
        f"Write-Output $t; [IO.File]::AppendAllText({sink}, $t + [Environment]::NewLine)")
    lines = [f"function __m($t) {{ {emit} }}", "$__seq = 0"]
    for i, s in enumerate(statements):
        ps_check_statement(s)
        lines.append(
            f"if ($__seq -eq {i}) {{ $ErrorActionPreference = 'Stop'; try {{ {s}; "
            f"$__seq = {i + 1}; __m '{tag} DONE {b} {i}' }} catch {{ $__seq = -1; "
            f"$__e = $_.Exception; while ($__e.InnerException) {{ $__e = $__e.InnerException }}; "
            f"__m ('{tag} ERR {b} {i} ' + $__e.GetType().Name + ' ' + [Convert]::ToBase64String("
            f"[Text.Encoding]::UTF8.GetBytes('' + $__e.Message))) }} }}")
    lines.append(f"__m '{tag} END {b}'")
    for ln in lines:
        ps_check_statement(ln)
    return "\n".join(lines) + "\n\n"


# Run once per session, before anything else: failures become catchable errors,
# no progress bars on stdout, and messages (French on the Windows test setup) as UTF-8.
SESSION_INIT = (
    "$ErrorActionPreference = 'Stop'",
    "$ProgressPreference = 'SilentlyContinue'",
    "[Console]::OutputEncoding = [Text.Encoding]::UTF8",
)


# ----------------------------------------------------------------- the link

def ssh_argv(ssh: str, key=None) -> list:
    """`ssh` with the options every remote call uses. BatchMode: a key that does not
    work fails at once instead of prompting. IdentitiesOnly with the key named: the
    agent's other keys are not offered to the remote machine."""
    argv = ["ssh", "-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=15",
            "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=4"]
    if key:
        argv += ["-i", str(key), "-o", "IdentitiesOnly=yes"]
    return argv + [ssh]


class Session:
    """One remote PowerShell reading statements from stdin, for the life of one tacli
    command. Opened on first use; `close()` ends it with a blank line."""

    def __init__(self, ssh: str, key=None):
        self.ssh, self.key = ssh, key
        self.token = secrets.token_hex(6)
        self.batch = 0
        self.proc = None
        self.q = queue.Queue()
        self.stderr = []

    def _start(self):
        # A FRESH QUEUE PER PROCESS: a killed session's reader thread still holds the
        # old queue and ends it with None, which must never reach the new process's
        # reads.
        self.q = queue.Queue()
        self._open()
        self._send(SESSION_INIT, timeout=60.0)

    def _open(self):
        argv = ssh_argv(self.ssh, self.key) + [
            "powershell", "-NoProfile", "-NonInteractive", "-Command", "-"]
        self.proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE)

        def pump(stream, into):
            for raw in iter(stream.readline, b""):
                into(raw.decode("utf-8", "replace").rstrip("\r\n"))
            into(None)

        threading.Thread(target=pump, args=(self.proc.stdout, self.q.put),
                         daemon=True).start()
        threading.Thread(target=pump, args=(self.proc.stderr, self._err),
                         daemon=True).start()

    def _write(self, script: str):
        self.proc.stdin.write(script.encode("ascii"))
        self.proc.stdin.flush()

    def _err(self, line):
        if line is not None:
            self.stderr = (self.stderr + [line])[-40:]

    def run(self, statements, timeout: float = 60.0) -> list:
        """Run statements; return what they printed, one string a line. Raises
        RemoteError on the first statement that failed or did not run."""
        if self.proc is None:
            self._start()
        return self._send(statements, timeout)

    def _send(self, statements, timeout):
        statements = list(statements)
        self.batch += 1
        script = ps_script(statements, self.token, self.batch)
        try:
            self._write(script)
        except (BrokenPipeError, OSError) as e:
            raise RemoteError(self._dead(f"the link to the remote machine is closed ({e})"))
        tag = f"@@{self.token} "
        out, done = [], set()
        deadline = time.time() + timeout
        while True:
            try:
                line = self.q.get(timeout=max(0.05, deadline - time.time()))
            except queue.Empty:
                self.kill()
                raise RemoteError(f"no answer from the remote machine in {timeout:g}s "
                                  f"(the link was closed)")
            if line is None:
                raise RemoteError(self._dead("the link to the remote machine closed"))
            if not line.startswith(tag):
                out.append(line)
                continue
            parts = line[len(tag):].split(" ", 4)
            if len(parts) < 2 or parts[1] != str(self.batch):
                continue                        # another batch's marker
            word = parts[0]
            if word == "END":
                break
            if word == "DONE" and len(parts) > 2 and parts[2].isdigit():
                done.add(int(parts[2]))
            elif word == "ERR" and len(parts) > 3 and parts[2].isdigit():
                idx, kind = int(parts[2]), parts[3]
                msg = parts[4] if len(parts) > 4 else ""
                msg = base64.b64decode(msg.strip() or "").decode("utf-8", "replace")
                msg = " ".join(msg.split())
                where = statements[idx][:120] if idx < len(statements) else "?"
                raise_after = RemoteError(f"{msg} [{kind}] (in: {where})", kind)
                self._drain_to_end(deadline)
                raise raise_after
        missing = [i for i in range(len(statements)) if i not in done]
        if missing:
            i = missing[0]
            raise RemoteError(f"PowerShell did not run statement {i} (it did not parse "
                              f"as one complete line): {statements[i][:160]}"
                              + self._stderr_tail())
        return out

    def _drain_to_end(self, deadline):
        """Read up to this batch's END so the next batch starts clean."""
        tag = f"@@{self.token} END {self.batch}"
        while time.time() < deadline:
            try:
                line = self.q.get(timeout=max(0.05, deadline - time.time()))
            except queue.Empty:
                return
            if line is None or line == tag:
                return

    def _stderr_tail(self):
        time.sleep(0.2)
        tail = [ln for ln in self.stderr if ln.strip()][-6:]
        return (" | remote stderr: " + " / ".join(tail)) if tail else ""

    def _dead(self, msg):
        return msg + self._stderr_tail()

    def close(self):
        if self.proc is None:
            return
        try:
            self.proc.stdin.write(b"\n")        # a blank line ends the script
            self.proc.stdin.close()
            self.proc.wait(timeout=5)
        except (OSError, subprocess.TimeoutExpired):
            self.kill()
        self.proc = None

    def kill(self):
        if self.proc is not None:
            try:
                self.proc.kill()
            except OSError:
                pass
            self.proc = None


# Tests replace this to run the routing with no machine behind it.
SESSION_FACTORY = Session
_SESSIONS = {}


def session_for(ssh: str, key=None) -> Session:
    """One session per link in a tacli process, however many Instance objects name it."""
    k = (ssh, str(key) if key else None)
    if k not in _SESSIONS:
        _SESSIONS[k] = SESSION_FACTORY(ssh, key)
    return _SESSIONS[k]


def close_all():
    """End every session with its blank line (tacli registers this at exit)."""
    while _SESSIONS:
        _, s = _SESSIONS.popitem()
        s.close()


# ------------------------------------------------------------- remote paths

class _Stat:
    def __init__(self, size, ticks):
        self.st_size = size
        # .NET ticks (100 ns since 0001-01-01 UTC) -> the Unix epoch, as os.stat has it
        self.st_mtime_ns = (ticks - 621355968000000000) * 100
        self.st_mtime = self.st_mtime_ns / 1e9


def _component(name: str) -> str:
    """One path component under the test folder: never absolute, never `..`, no
    drive, no separator -- so no RemotePath can name anything outside it."""
    if (not name or name in (".", "..") or any(c in name for c in '\\/:*?"<>|')
            or name != name.strip()):
        raise ValueError(f"refusing the remote path component {name!r}")
    return name


class RemotePath:
    """A file under a remote instance's test folder, with the handful of
    `pathlib.Path` methods the file channels use.

    NOT os.PathLike, deliberately: handed to `open()`, `shutil` or `subprocess` by a
    code path nobody routed, it raises TypeError instead of acting on a local file
    with the same name."""

    def __init__(self, remote: "Remote", parts):
        self.remote = remote
        self.parts = tuple(_component(p) for p in parts)

    @property
    def win(self) -> str:
        return ntpath.join(self.remote.folder, *self.parts)

    @property
    def name(self) -> str:
        return self.parts[-1] if self.parts else ntpath.basename(self.remote.folder)

    @property
    def suffix(self) -> str:
        return ntpath.splitext(self.name)[1]

    @property
    def parent(self):
        return RemotePath(self.remote, self.parts[:-1]) if self.parts else self

    def __truediv__(self, name):
        return RemotePath(self.remote, self.parts + tuple(str(name).split("/")))

    def __str__(self):
        return self.win

    def __repr__(self):
        return f"RemotePath({self.win!r})"

    def __eq__(self, other):
        return isinstance(other, RemotePath) and self.win.lower() == other.win.lower()

    def __hash__(self):
        return hash(self.win.lower())

    def _run(self, statements, timeout=60.0):
        return self.remote.run(statements, timeout)

    # -- queries
    def exists(self) -> bool:
        p = ps_str(self.win)
        return self._run([f"Write-Output ([IO.File]::Exists({p}) -or "
                          f"[IO.Directory]::Exists({p}))"]) == ["True"]

    def is_symlink(self) -> bool:
        return False

    def stat(self):
        out = self._run([f"$i = New-Object IO.FileInfo {ps_str(self.win)}; if ($i.Exists) "
                         f"{{ Write-Output ('' + $i.Length + ' ' + $i.LastWriteTimeUtc.Ticks) }} "
                         f"else {{ Write-Output 'absent' }}"])
        if not out or out[0] == "absent":
            raise FileNotFoundError(self.win)
        size, ticks = out[0].split()
        return _Stat(int(size), int(ticks))

    def read_bytes(self, offset: int = 0, length=None) -> bytes:
        """The file's bytes (or `length` of them from `offset`), opened so the DLL can
        go on writing, renaming or deleting it meanwhile."""
        p = ps_str(self.win)
        data = bytearray()
        pos = offset
        while True:
            want = CHUNK if length is None else min(CHUNK, offset + length - pos)
            if want <= 0:
                break
            try:
                out = self._run([_read_stmt(p, pos, want)], timeout=120.0)
            except RemoteError as e:
                if e.kind in ("FileNotFoundException", "DirectoryNotFoundException"):
                    raise FileNotFoundError(self.win) from None
                raise
            chunk = base64.b64decode(out[0] if out else "")
            data += chunk
            pos += len(chunk)
            if len(chunk) < want:
                break
        return bytes(data)

    def read_text(self, errors=None, encoding="utf-8") -> str:
        return self.read_bytes().decode(encoding, errors or "strict")

    def glob(self, pattern: str):
        out = self._run([f"if ([IO.Directory]::Exists({ps_str(self.win)})) {{ "
                         f"Get-ChildItem -LiteralPath {ps_str(self.win)} -Filter "
                         f"{ps_str(pattern)} -File | ForEach-Object {{ Write-Output {_B64_NAME} }} }}"])
        return [self / _unb64(name) for name in out if name]

    # -- changes
    def _guard(self):
        """The statement that runs before this file is replaced or deleted: it keeps
        the player's original beside it (the two tiers above PROTECTED)."""
        p, b = ps_str(self.win), ps_str(self.win + BACKUP)
        if len(self.parts) == 1 and self.name.lower() in PROTECTED:
            return [f"if ([IO.File]::Exists({p}) -and -not [IO.File]::Exists({b}) -and -not "
                    f"[IO.File]::Exists({ps_str(self.win + ABSENT)})) {{ throw ('refusing to "
                    f"replace ' + {p} + ': no record of the original beside it') }}"]
        listed = ps_str(self.remote.state(COPIED))
        rel = chr(92).join(self.parts)
        player = ps_str(ntpath.join(self.remote.player, *self.parts))
        return [f"if ([IO.File]::Exists({p}) -and -not [IO.File]::Exists({b}) -and "
                f"[IO.File]::Exists({listed})) {{ $__row = @([IO.File]::ReadAllLines({listed}) | "
                f"Where-Object {{ $_.Length -gt 65 -and $_.Substring(65) -eq {ps_str(rel)} }}); "
                f"if ($__row.Count -gt 0) {{ $__want = $__row[0].Substring(0, 64); "
                f"if ((Get-FileHash -LiteralPath {p} -Algorithm SHA256).Hash -eq $__want) "
                f"{{ $__src = {p} }} elseif ([IO.File]::Exists({player}) -and (Get-FileHash "
                f"-LiteralPath {player} -Algorithm SHA256).Hash -eq $__want) {{ $__src = {player} }} "
                f"else {{ throw ('refusing to replace ' + {p} + ': neither it nor the player''s "
                f"copy is still the file remote add copied, so there is no original to keep') }}; "
                f"Copy-Item -LiteralPath $__src -Destination {b}; if ((Get-FileHash -LiteralPath "
                f"{b} -Algorithm SHA256).Hash -ne $__want) {{ throw ('the backup of ' + {p} + "
                f"' is not the original') }} }} }}"]

    def write_bytes(self, data: bytes):
        """Replace the file whole: written under a temporary name, then moved over
        the target, so a reader never sees half of it."""
        stmts = self._guard() + _write_stmts(self.win, data)
        # the move can meet a reader holding the target open: retry, never append
        for attempt in range(4):
            try:
                self._run(stmts, timeout=120.0)
                return
            except RemoteError as e:
                if attempt == 3 or e.kind != "IOException":
                    raise
                time.sleep(0.1)

    def write_text(self, text: str, encoding="utf-8"):
        self.write_bytes(text.encode(encoding))

    def create_new(self, data: bytes) -> bool:
        """Put the file in place only if it is absent; False if it is there.

        The key file's protocol: the DLL reads `tagpu_keys.txt` whole and deletes it.
        Appending to a file it is reading could land after its read and be deleted
        with it, so a batch is written under a temporary name and MOVED into place
        when the previous one is gone. A move never overwrites, so tacli is the only
        writer of the name and the DLL only ever sees a complete batch."""
        tmp, p = ps_str(self.win + ".tacli-tmp"), ps_str(self.win)
        b64 = base64.b64encode(data).decode("ascii")
        out = self._run([
            f"[IO.File]::WriteAllBytes({tmp}, [Convert]::FromBase64String('{b64}'))",
            f"if ([IO.File]::Exists({p})) {{ [IO.File]::Delete({tmp}); Write-Output 'busy' }} "
            f"else {{ [IO.File]::Move({tmp}, {p}); Write-Output 'placed' }}"])
        return out == ["placed"]

    def unlink(self, missing_ok: bool = False):
        p = ps_str(self.win)
        stmts = self._guard()
        stmts.append(f"if ([IO.File]::Exists({p})) {{ [IO.File]::Delete({p}); Write-Output 'gone' }} "
                     f"else {{ Write-Output 'absent' }}")
        if self._run(stmts) == ["absent"] and not missing_ok:
            raise FileNotFoundError(self.win)

    def rename(self, target):
        target = target if isinstance(target, RemotePath) else self.parent / str(target)
        stmts = self._guard() + target._guard() + [
            f"Move-Item -LiteralPath {ps_str(self.win)} -Destination {ps_str(target.win)} -Force"]
        self._run(stmts)
        return target

    def mkdir(self, parents=True, exist_ok=True):
        self._run([f"[void][IO.Directory]::CreateDirectory({ps_str(self.win)})"])

    def upload(self, local: Path):
        """Replace the file with a local one, and prove it arrived: the MD5 the far
        side computes must equal this side's."""
        data = Path(local).read_bytes()
        self.write_bytes(data)
        out = self._run([f"Write-Output (Get-FileHash -LiteralPath {ps_str(self.win)} "
                         f"-Algorithm MD5).Hash"])
        want = hashlib.md5(data).hexdigest().upper()
        if not out or out[0].strip().upper() != want:
            raise RemoteError(f"{self.win}: MD5 {out[0] if out else '?'} after the upload, "
                              f"{want} sent")


# ------------------------------------------------------------- the machine

WIN_USER_RX = re.compile(r"[A-Za-z0-9._-]{1,64}@[A-Za-z0-9._-]{1,253}")


class Remote:
    """A remote instance's transport and folders, as recorded in its metadata.

    ssh      `<user>@<host>` for the key login
    key      the local private key file, or None for ssh's own choice
    player   the player's game folder (never written: `remote add` copies it once)
    folder   the test folder the game runs from
    """

    def __init__(self, spec: dict):
        self.ssh = spec["ssh"]
        self.key = spec.get("key")
        self.player = ntpath.normpath(spec["player"])
        self.folder = ntpath.normpath(spec["folder"])
        self.task = spec.get("task") or ""
        self.console_user = spec.get("console_user") or ""
        self._home = None
        if not WIN_USER_RX.fullmatch(self.ssh):
            raise ValueError("--ssh: say <user>@<host> (letters, digits, . _ -)")
        check_folders(self.player, self.folder)

    @property
    def session(self) -> Session:
        return session_for(self.ssh, self.key)

    def run(self, statements, timeout=60.0):
        return self.session.run(statements, timeout)

    @property
    def root(self) -> RemotePath:
        return RemotePath(self, ())

    # -- processes
    def procs(self):
        """Every TotalA.exe on the machine, as (pid, path) -- ours or anyone's."""
        out = self.run(["Get-Process TotalA -ErrorAction SilentlyContinue | ForEach-Object { "
                        "$p = ''; try { $p = $_.Path } catch { $p = '?' }; "
                        f"Write-Output ('' + $_.Id + '|' + {ps_b64('$p')}) }}"])
        rows = []
        for line in out:
            pid, _, path = line.partition("|")
            if pid.strip().isdigit():
                rows.append((int(pid), _unb64(path) if path.strip() else ""))
        return rows

    def is_ours(self, path: str) -> bool:
        return (path or "").lower().startswith(self.folder.lower().rstrip("\\") + "\\")

    def pid(self):
        """The pid of the TotalA.exe running from the test folder, or None."""
        for pid, path in self.procs():
            if self.is_ours(path):
                return pid
        return None

    def console(self) -> str:
        """The user logged on at the console (an interactive task runs as them), and
        a check that it is the SSH login's user: TA reads HKCU, so the registry the
        launch wrapper exports must be the hive the game will use."""
        out = self.run([
            f"Write-Output {ps_b64('(Get-CimInstance Win32_ComputerSystem).UserName')}",
            f"Write-Output {ps_b64('[Security.Principal.WindowsIdentity]::GetCurrent().Name')}"])
        console = _unb64(out[0]) if out else ""
        me = _unb64(out[1]) if len(out) > 1 else ""
        if not console:
            raise RemoteError("nobody is logged on at the remote console, so a game "
                              "launched there has no desktop to open on")
        if console.lower() != me.lower():
            raise RemoteError(f"the console user is {console} but SSH logs in as {me}: "
                              f"the game would read another user's registry than the one "
                              f"the launch wrapper saves and restores")
        return console

    def home(self) -> str:
        """`%LOCALAPPDATA%\\tacli`: where the registry record and its export live, one
        per user of the machine (HKCU is per user), beside no test folder."""
        if self._home is None:
            expr = ps_b64("[Environment]::GetFolderPath('LocalApplicationData')")
            out = self.run([f"Write-Output {expr}"])
            base = _unb64(out[0]) if out else ""
            if not re.fullmatch(r"[A-Za-z]:\\.+", base):
                raise RemoteError(f"the remote machine reported no local application data "
                                  f"folder ({base!r})")
            self._home = ntpath.join(base, HOME_DIR)
        return self._home

    # -- the test folder
    def resolve_folders(self):
        """The player's folder and the test folder as the FILE SYSTEM names them, and
        the refusals a string compare cannot make (`check_folders` runs again on
        what this returns):

          * each existing component is replaced by the long name the directory
            listing gives for it, so an 8.3 short name reads as the folder it names;
            `GetFullPath` first drops `.`, `..` and a trailing dot or space;
          * a junction or symbolic link anywhere in either path is refused: through
            one, two different strings can name one folder;
          * two different drive letters on one volume (a `subst` alias) are refused.
        Nothing is written: `remote add` runs this before its first write."""
        out = self.run([
            _resolve_stmt(ps_str(self.player)), f"$__a = $__real; Write-Output {ps_b64('$__a')}",
            _resolve_stmt(ps_str(self.folder)), f"$__b = $__real; Write-Output {ps_b64('$__b')}",
            "$__da = $__a.Substring(0, 2); $__db = $__b.Substring(0, 2); if ($__da -ne $__db) { "
            "$__sa = @(Get-CimInstance Win32_LogicalDisk -Filter ('DeviceID=''' + $__da + '''')); "
            "$__sb = @(Get-CimInstance Win32_LogicalDisk -Filter ('DeviceID=''' + $__db + '''')); "
            "if ($__sa.Count -ne 1 -or $__sb.Count -ne 1 -or -not $__sa[0].VolumeSerialNumber -or "
            "$__sa[0].VolumeSerialNumber -eq $__sb[0].VolumeSerialNumber) { throw ('the drives ' + "
            "$__da + ' and ' + $__db + ' may be one volume under two letters; put the test folder "
            "on the player''s drive or on a volume of its own') } }",
        ], timeout=120.0)
        player, folder = _unb64(out[0]), _unb64(out[1])
        check_folders(player, folder)
        return player, folder

    def check_copy(self) -> dict:
        """What `copy_player` needs to be true first: the player's folder is a game
        folder and not a test folder, the test folder does not exist yet, and its
        drive has the room."""
        src, dst = ps_str(self.player), ps_str(self.folder)
        out = self.run([
            f"if (-not [IO.File]::Exists({ps_str(ntpath.join(self.player, 'TotalA.exe'))})) "
            f"{{ throw 'the player''s folder has no TotalA.exe' }}",
            f"if ((Test-Path -LiteralPath {ps_str(ntpath.join(self.player, MARKER))}) -or (Test-Path "
            f"-LiteralPath {ps_str(ntpath.join(self.player, STATE_DIR))})) {{ throw 'the player''s "
            f"folder is itself a tacli test folder' }}",
            f"if (Test-Path -LiteralPath {dst}) {{ throw 'the test folder already exists' }}",
            f"$m = Get-ChildItem -LiteralPath {src} -Recurse -File -Force | Measure-Object "
            f"-Property Length -Sum; Write-Output ('' + $m.Count + ' ' + $m.Sum)",
            f"$d = (Get-Item -LiteralPath ([IO.Path]::GetPathRoot({dst}))).PSDrive; "
            f"Write-Output ('' + $d.Free)",
        ], timeout=300.0)
        files, size = (int(x or 0) for x in (out[0].split() + ["0", "0"])[:2])
        free = int(out[1]) if len(out) > 1 and out[1].strip().isdigit() else None
        if free is not None and free < size + (256 << 20):
            raise RemoteError(f"the test folder's drive has {free >> 20} MB free and the "
                              f"copy needs {size >> 20} MB")
        return {"files": files, "bytes": size}

    def begin_folder(self, instance: str):
        """`remote add`'s FIRST write: the test folder with its marker in it, before a
        byte is copied, so a copy that fails half-way leaves a folder `rm` recognises
        as this instance's."""
        mark = f"instance={instance}\nplayer={self.player}\n"
        self.run([
            f"if (Test-Path -LiteralPath {ps_str(self.folder)}) {{ throw 'the test folder "
            f"already exists' }}",
            f"[void][IO.Directory]::CreateDirectory({ps_str(self.state(''))})",
            f"[IO.File]::WriteAllText({ps_str(ntpath.join(self.folder, MARKER))}, {ps_str(mark)})",
        ])

    def copy_player(self) -> dict:
        """Copy the player's folder, once, into the test folder `begin_folder` made;
        record every copied file's SHA-256 (COPIED) and the original of every
        PROTECTED file. Reads the player's folder and writes nothing into it."""
        src, dst = ps_str(self.player), ps_str(self.folder)
        # tacli's own files in the test folder, which the player's folder does not have
        ours = (f"Where-Object {{ $_.FullName -ne {ps_str(ntpath.join(self.folder, MARKER))} "
                f"-and -not $_.FullName.StartsWith({ps_str(self.state(''))}, "
                f"[StringComparison]::OrdinalIgnoreCase) }}")
        self.run([
            f"$ErrorActionPreference = 'Continue'; & robocopy.exe {src} {dst} /E /COPY:DAT "
            f"/DCOPY:T /R:1 /W:1 /NP /NFL /NDL /NJH /NJS 2>$null | Out-Null; "
            f"$rc = $LASTEXITCODE; $ErrorActionPreference = 'Stop'; "
            f"if ($rc -ge 8) {{ throw ('robocopy exited ' + $rc) }}",
        ], timeout=1800.0)
        out = self.run([
            f"$m = Get-ChildItem -LiteralPath {src} -Recurse -File -Force | Measure-Object "
            f"-Property Length -Sum; Write-Output ('' + $m.Count + ' ' + $m.Sum)",
            f"$m = Get-ChildItem -LiteralPath {dst} -Recurse -File -Force | {ours} | Measure-Object "
            f"-Property Length -Sum; Write-Output ('' + $m.Count + ' ' + $m.Sum)"], timeout=300.0)
        if out[0] != out[1]:
            raise RemoteError(f"the copy holds {out[1]} (files bytes) and the player's "
                              f"folder {out[0]}: the test folder is incomplete")
        # the list `_guard` reads, hashed before tacli or the game changes a byte
        self.run([f"$n = ({dst}).Length + 1; $l = @(Get-ChildItem -LiteralPath {dst} -Recurse "
                  f"-File -Force | {ours} | ForEach-Object {{ (Get-FileHash -LiteralPath "
                  f"$_.FullName -Algorithm SHA256).Hash + ' ' + $_.FullName.Substring($n) }}); "
                  f"[IO.File]::WriteAllLines({ps_str(self.state(COPIED))}, [string[]]$l)"],
                 timeout=1800.0)
        stmts = []
        for name in PROTECTED:
            f = ntpath.join(self.folder, name)
            stmts.append(
                f"if ([IO.File]::Exists({ps_str(f)})) {{ Copy-Item -LiteralPath {ps_str(f)} "
                f"-Destination {ps_str(f + BACKUP)}; if ((Get-FileHash -LiteralPath "
                f"{ps_str(f)}).Hash -ne (Get-FileHash -LiteralPath {ps_str(f + BACKUP)}).Hash) "
                f"{{ throw ('the backup of ' + {ps_str(name)} + ' differs from it') }}; "
                f"Write-Output ({ps_str(name)} + ' copied') }} else {{ [IO.File]::WriteAllBytes("
                f"{ps_str(f + ABSENT)}, [byte[]]@()); Write-Output ({ps_str(name)} + ' absent') }}")
        backups = self.run(stmts)
        files, size = (int(x) for x in out[0].split())
        return {"files": files, "bytes": size, "protected": backups}

    def remove_folder(self, instance: str):
        """Delete the test folder -- only when its marker names this instance, when it
        still resolves to the folder `remote add` recorded, and when nothing inside it
        is a junction or link (so the delete cannot reach through one). The delete is
        `Directory.Delete`, not `Remove-Item -Recurse`."""
        marker = ps_str(ntpath.join(self.folder, MARKER))
        folder = ps_str(self.folder)
        self.run([
            f"if (-not [IO.File]::Exists({marker})) {{ throw 'no tacli marker in the test "
            f"folder; nothing was deleted' }}",
            f"$t = [IO.File]::ReadAllText({marker}); if (-not $t.StartsWith("
            f"{ps_str('instance=' + instance + chr(10))})) {{ throw ('the marker names '"
            f" + $t.Split([char]10)[0] + '; nothing was deleted') }}",
            _resolve_stmt(folder),
            f"if ($__real -ne {folder}) {{ throw ('the test folder now resolves to ' + $__real "
            f"+ '; nothing was deleted') }}",
            f"$__st = New-Object Collections.Stack; $__st.Push({folder}); while ($__st.Count "
            f"-gt 0) {{ $__d = $__st.Pop(); foreach ($__x in [IO.Directory]::GetDirectories("
            f"$__d)) {{ if (([IO.File]::GetAttributes($__x) -band [IO.FileAttributes]::"
            f"ReparsePoint) -ne 0) {{ throw ($__x + ' is a junction or link inside the test "
            f"folder; nothing was deleted') }}; $__st.Push($__x) }}; foreach ($__x in "
            f"[IO.Directory]::GetFiles($__d)) {{ $__at = [IO.File]::GetAttributes($__x); "
            f"if (($__at -band [IO.FileAttributes]::ReparsePoint) -ne 0) {{ throw ($__x + ' is "
            f"a link inside the test folder; nothing was deleted') }}; if (($__at -band "
            f"[IO.FileAttributes]::ReadOnly) -ne 0) {{ [IO.File]::SetAttributes($__x, $__at "
            f"-bxor [IO.FileAttributes]::ReadOnly) }} }} }}",
            f"[IO.Directory]::Delete({folder}, $true)",
        ], timeout=900.0)

    # -- the scheduled task and its wrapper
    def task_start(self, wrapper: str):
        """Run the launch wrapper on the console user's desktop. A process started from
        the SSH session runs where nobody can see it; a scheduled task with an
        interactive principal runs on the desktop, and needs no password. The action
        is `powershell.exe -File <wrapper>`: a file, not `-EncodedCommand`, because
        the wrapper is longer than a command line, and not stdin, because nothing
        stays attached to the task to feed it."""
        path, name = self.task_parts()
        argline = (f"-NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden "
                   f"-File \"{wrapper}\"")
        self.run([
            "$a = New-ScheduledTaskAction -Execute ([IO.Path]::Combine($env:SystemRoot, "
            "'System32\\WindowsPowerShell\\v1.0\\powershell.exe')) -Argument "
            f"{ps_str(argline)} -WorkingDirectory {ps_str(self.folder)}",
            f"$pr = New-ScheduledTaskPrincipal -UserId {ps_str(self.console_user)} "
            f"-LogonType Interactive",
            "$st = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) "
            "-AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew",
            f"Register-ScheduledTask -TaskPath {ps_str(path)} -TaskName {ps_str(name)} "
            f"-Action $a -Principal $pr -Settings $st -Force | Out-Null",
            f"Start-ScheduledTask -TaskPath {ps_str(path)} -TaskName {ps_str(name)}",
        ])

    def task_parts(self):
        path, _, name = self.task.rpartition("\\")
        return (path + "\\") if path else "\\", name

    def task_remove(self):
        path, name = self.task_parts()
        self.run([f"$t = Get-ScheduledTask -TaskPath {ps_str(path)} -TaskName {ps_str(name)} "
                  f"-ErrorAction SilentlyContinue; if ($t) {{ Unregister-ScheduledTask "
                  f"-TaskPath {ps_str(path)} -TaskName {ps_str(name)} -Confirm:$false }}"])

    def stop_pid(self, pid: int, timeout: float = 15.0) -> bool:
        self.run([f"Stop-Process -Id {int(pid)} -Force -ErrorAction SilentlyContinue"])
        deadline = time.time() + timeout
        while time.time() < deadline:
            if pid not in [p for p, _ in self.procs()]:
                return True
            time.sleep(0.3)
        return False

    def state(self, name: str) -> str:
        return ntpath.join(self.folder, STATE_DIR, name)

    # -- the registry: every write is the wrapper's, inside the game's lifetime
    def registry_state(self):
        """(the tacli tasks that are Running or Queued, the pending registry record or
        None), in that order, in one batch. A record with no running task is a
        restore that never happened: the wrapper removes its record before it
        exits, and the task is Running for as long as the wrapper lives."""
        pend = ps_str(ntpath.join(self.home(), PENDING))
        out = self.run([
            f"Get-ScheduledTask -TaskPath {ps_str(TASK_DIR)} -ErrorAction SilentlyContinue | "
            f"Where-Object {{ $_.State -eq 'Running' -or $_.State -eq 'Queued' }} | "
            f"ForEach-Object {{ Write-Output ('task ' + {ps_b64('$_.TaskName')}) }}",
            f"if ([IO.File]::Exists({pend})) {{ Write-Output ('pending ' + [Convert]::"
            f"ToBase64String([IO.File]::ReadAllBytes({pend}))) }}"])
        busy = [_unb64(ln[5:]) for ln in out if ln.startswith("task ")]
        record = None
        for ln in out:
            if ln.startswith("pending "):
                record = parse_record(base64.b64decode(ln[8:]).decode("utf-8", "replace"))
        return busy, record

    def _restore_statements(self) -> list:
        """Put TA's key back as the pending record says it was, prove it by exporting
        again, and only then remove the record. The launch wrapper runs these after
        the game; `restore` runs them on request."""
        pend = ps_str(ntpath.join(self.home(), PENDING))
        exp = ps_str(ntpath.join(self.home(), REG_EXPORT))
        chk = ps_str(ntpath.join(self.home(), REG_CHECK))
        key, pspath = ps_str(REG_KEY), ps_str(REG_PSPATH)
        return [
            f"$__k = @([IO.File]::ReadAllLines({pend}) | Where-Object {{ $_.StartsWith('key=') }}); "
            f"$__want = ''; if ($__k.Count -gt 0) {{ $__want = $__k[$__k.Count - 1].Substring(4) }}",
            f"if ($__want.StartsWith('present ')) {{ if (-not [IO.File]::Exists({exp})) {{ throw "
            f"'the export taken at launch is missing; the key was left as it is' }}; $__h = "
            f"(Get-FileHash -LiteralPath {exp} -Algorithm SHA256).Hash; if ($__h -ne "
            f"$__want.Substring(8)) {{ throw ('the export taken at launch has changed (' + $__h + "
            f"'); the key was left as it is') }} }}",
            f"if ($__want -ne '' -and (Test-Path -LiteralPath {pspath})) {{ "
            + _native(f"& reg.exe delete {key} /f", "reg delete") + " }",
            f"if ($__want.StartsWith('present ')) {{ "
            + _native(f"& reg.exe import {exp}", "reg import") + " }",
            f"if ($__want.StartsWith('present ')) {{ "
            + _native(f"& reg.exe export {key} {chk} /y", "reg export")
            + f"; $__h = (Get-FileHash -LiteralPath {chk} -Algorithm SHA256).Hash; [IO.File]::"
            f"Delete({chk}); if ($__h -ne $__want.Substring(8)) {{ throw ('TA''s registry key "
            f"reads ' + $__h + ' after the restore, not the export''s ' + $__want.Substring(8)) }} "
            f"}} elseif ($__want -eq 'absent' -and (Test-Path -LiteralPath {pspath})) {{ throw "
            f"'TA''s registry key is still there after deleting it' }}",
            f"[IO.File]::Delete({pend}); __m ('restored ' + $__want)",
        ]

    def wrapper_script(self, instance: str, argv, values, token: str) -> str:
        """The scheduled task's script, in two batches of `ps_script` whose markers go
        to `tacli-state\\launch.status`:

          1. refuse beside any running TotalA.exe; take the pending record with
             CreateNew -- the lock: a second wrapper fails here and touches nothing;
             export TA's key and record its SHA-256 (or its absence) in the record;
             write the test values; start TotalA.exe from the test folder and wait
             for it to exit;
          2. whatever batch 1 did after taking the record, and only if it took it:
             wait until no TotalA.exe runs, then restore and verify
             (`_restore_statements`), which removes the record.

        So the key is exported before the first test value is written, restored after
        the game has exited, and the record says so the whole time in between. The
        only way to leave it unrestored is to end the wrapper itself (a restart, the
        task ended), and the record left behind says exactly that."""
        home = self.home()
        pend = ps_str(ntpath.join(home, PENDING))
        exp = ps_str(ntpath.join(home, REG_EXPORT))
        key, pspath = ps_str(REG_KEY), ps_str(REG_PSPATH)
        nl = "[Environment]::NewLine"
        first = [
            "$__mine = $false",
            "if (@(Get-Process TotalA -ErrorAction SilentlyContinue).Count -gt 0) { throw "
            "'TotalA.exe is already running; the wrapper touches nothing beside it' }",
            f"[void][IO.Directory]::CreateDirectory({ps_str(home)})",
            f"$__f = [IO.File]::Open({pend}, 'CreateNew', 'Write', 'Read'); $__mine = $true; "
            f"try {{ $__b = [Text.Encoding]::UTF8.GetBytes('instance=' + {ps_str(instance)} + {nl} "
            f"+ 'folder=' + {ps_str(self.folder)} + {nl} + 'wrapper=' + $PID + {nl} + 'started=' "
            f"+ (Get-Date).ToString('s') + {nl}); $__f.Write($__b, 0, $__b.Length) }} finally "
            f"{{ $__f.Close() }}",
            f"if (Test-Path -LiteralPath {pspath}) {{ "
            + _native(f"& reg.exe export {key} {exp} /y", "reg export")
            + f"; $__line = 'key=present ' + (Get-FileHash -LiteralPath {exp} -Algorithm "
            f"SHA256).Hash }} else {{ $__line = 'key=absent' }}; [IO.File]::AppendAllText({pend}, "
            f"$__line + {nl})",
        ]
        for sub, name, kind, data in values:
            path = ps_str(REG_PSPATH + (("\\" + sub) if sub else ""))
            ptype = {"REG_DWORD": "DWord", "REG_SZ": "String"}[kind]
            val = str(int(data)) if kind == "REG_DWORD" else ps_str(data)
            first.append(f"if (-not (Test-Path -LiteralPath {path})) {{ New-Item -Path {path} "
                         f"-Force | Out-Null }}; New-ItemProperty -LiteralPath {path} -Name "
                         f"{ps_str(name)} -PropertyType {ptype} -Value {val} -Force | Out-Null")
        argline = " ".join(argv)
        first += [
            f"$__g = Start-Process -FilePath {ps_str(ntpath.join(self.folder, 'TotalA.exe'))} "
            f"-WorkingDirectory {ps_str(self.folder)}"
            + (f" -ArgumentList {ps_str(argline)}" if argline else "")
            + " -PassThru; __m ('game ' + $__g.Id)",
            "$__g.WaitForExit()",
        ]
        second = [
            "if (-not $__mine) { throw 'this wrapper took no registry record, so it restores "
            "nothing' }",
            "while (@(Get-Process TotalA -ErrorAction SilentlyContinue).Count -gt 0) { "
            "Start-Sleep -Seconds 2 }",
        ] + self._restore_statements()
        sink = ps_str(self.state(WRAPPER_LOG))
        return (ps_script(first, token, 1, sink=sink) + ps_script(second, token, 2, sink=sink))

    def restore(self) -> str:
        """`tacli remote restore`: the restore a wrapper that was ended never ran.
        Refuses, in the same batch, while any TotalA.exe runs (a restore under a
        running game is overwritten by it, or deletes a key a player's game reads)
        and while any tacli task is Running or Queued (that wrapper restores the key
        itself)."""
        pend = ps_str(ntpath.join(self.home(), PENDING))
        out = self.run([
            "if (@(Get-Process TotalA -ErrorAction SilentlyContinue).Count -gt 0) { throw "
            "'TotalA.exe is running on the remote machine; the key is not restored under a "
            "running game' }",
            f"$__busy = @(Get-ScheduledTask -TaskPath {ps_str(TASK_DIR)} -ErrorAction "
            f"SilentlyContinue | Where-Object {{ $_.State -eq 'Running' -or $_.State -eq "
            f"'Queued' }}); if ($__busy.Count -gt 0) {{ throw ('a launch wrapper is still "
            f"running (' + $__busy[0].TaskName + '); it restores the key itself') }}",
            f"if (-not [IO.File]::Exists({pend})) {{ throw 'no registry record is pending' }}",
        ] + self._restore_statements(), timeout=120.0)
        return restore_note(out)

    def home_exists(self, name: str) -> bool:
        return self.run([f"Write-Output ([IO.File]::Exists("
                         f"{ps_str(ntpath.join(self.home(), name))}))"]) == ["True"]

    def home_read(self, name: str) -> bytes:
        out = self.run([_read_stmt(ps_str(ntpath.join(self.home(), name)), 0, CHUNK)],
                       timeout=120.0)
        return base64.b64decode(out[0] if out else "")

    def home_write(self, name: str, data: bytes):
        """Put a file into the registry home (the export, from its local copy)."""
        if len(data) > CHUNK:
            raise RemoteError(f"{name}: {len(data)} bytes is more than a registry export")
        self.run([f"[void][IO.Directory]::CreateDirectory({ps_str(self.home())})"]
                 + _write_stmts(ntpath.join(self.home(), name), data), timeout=120.0)


TASK_DIR = "\\tacli\\"          # every remote instance's task lives in this folder


def parse_record(text: str) -> dict:
    """The pending record: `k=v` lines; `key` is `present <SHA-256>` or `absent`, and
    missing when the wrapper stopped before its export."""
    rec = {}
    for line in text.splitlines():
        k, sep, v = line.partition("=")
        if sep:
            rec[k.strip()] = v.strip()
    return rec


def restore_note(lines) -> str:
    """What the restore's last statement printed, in words."""
    for line in lines:
        if line.startswith("restored"):
            what = line[len("restored"):].strip()
            if what.startswith("present "):
                return (f"restored TA's registry key ({what[8:20]}…, verified by a fresh "
                        f"export)")
            if what == "absent":
                return ("removed TA's registry key again: it did not exist before the "
                        "launch")
            return "nothing to restore: the wrapper ended before its export"
    return "the restore printed no result"


def parse_status(text: str, token: str) -> dict:
    """The launch wrapper's marker lines (`launch.status`): per batch, the statements
    that finished, the error if one failed, whether it reached its end; and the
    game's pid and the restore's result, which its statements print."""
    tag = f"@@{token} "
    res = {"game": None, "restored": None, "done": {1: set(), 2: set()}, "err": {},
           "end": set()}
    for line in text.splitlines():
        if line.startswith("game ") and line[5:].strip().isdigit():
            res["game"] = int(line[5:])
        elif line.startswith("restored"):
            res["restored"] = restore_note([line])
        elif line.startswith(tag):
            parts = line[len(tag):].split(" ", 4)
            if len(parts) < 2 or not parts[1].isdigit():
                continue
            b = int(parts[1])
            if parts[0] == "END":
                res["end"].add(b)
            elif parts[0] == "DONE" and len(parts) > 2 and parts[2].isdigit():
                res["done"].setdefault(b, set()).add(int(parts[2]))
            elif parts[0] == "ERR" and len(parts) > 3:
                msg = base64.b64decode((parts[4] if len(parts) > 4 else "").strip() or "")
                res["err"][b] = (int(parts[2]), parts[3],
                                 " ".join(msg.decode("utf-8", "replace").split()))
    return res


def _native(cmd: str, what: str) -> str:
    """A native command inside a statement, judged by its exit code: its stderr is
    dropped (it would be a terminating error under 'Stop'), and the preference is
    restored before the check. ps_script also resets it at the next line."""
    return (f"$ErrorActionPreference = 'Continue'; {cmd} 2>$null | Out-Null; "
            f"$__rc = $LASTEXITCODE; $ErrorActionPreference = 'Stop'; "
            f"if ($__rc -ne 0) {{ throw ('{what} exited ' + $__rc) }}")


def _resolve_stmt(p: str) -> str:
    """One statement leaving in `$__real` the path `p` (a PowerShell expression) as the
    file system names it; see `Remote.resolve_folders`. `GetDirectories` with the
    component as its pattern answers with the entry's long name, whichever of its
    two names the pattern gave."""
    return (
        f"$__p = [IO.Path]::GetFullPath({p}); $__root = [IO.Path]::GetPathRoot($__p); "
        f"$__real = $__root; $__gone = $false; foreach ($__c in $__p.Substring("
        f"$__root.Length).Split([char]92)) {{ if ($__c -eq '') {{ continue }}; if ($__gone) "
        f"{{ $__real = [IO.Path]::Combine($__real, $__c); continue }}; if ($__c.IndexOfAny("
        f"[char[]]@([char]42, [char]63)) -ge 0) {{ throw ('a wildcard in ' + $__p) }}; "
        f"$__m = @([IO.Directory]::GetDirectories($__real, $__c)); if ($__m.Count -eq 0) {{ "
        f"if ([IO.File]::Exists([IO.Path]::Combine($__real, $__c))) {{ throw ([IO.Path]::"
        f"Combine($__real, $__c) + ' is a file, not a folder') }}; $__gone = $true; $__real = "
        f"[IO.Path]::Combine($__real, $__c); continue }}; if ($__m.Count -ne 1) {{ throw ("
        f"'more than one folder answers to ' + [IO.Path]::Combine($__real, $__c)) }}; if (("
        f"[IO.File]::GetAttributes($__m[0]) -band [IO.FileAttributes]::ReparsePoint) -ne 0) "
        f"{{ throw ($__m[0] + ' is a junction or a link: a folder reached through one can "
        f"have two names') }}; $__real = $__m[0] }}")


def _read_stmt(p: str, pos: int, want: int) -> str:
    """Read `want` bytes from `pos` of the file `p` (a PowerShell expression), opened so
    the game can go on writing, renaming or deleting it meanwhile; prints base64."""
    return (f"$f = [IO.File]::Open({p}, 'Open', 'Read', 'ReadWrite, Delete'); try {{ "
            f"[void]$f.Seek({pos}, 'Begin'); $b = New-Object byte[] {want}; $t = 0; "
            f"while ($t -lt {want}) {{ $r = $f.Read($b, $t, {want} - $t); "
            f"if ($r -le 0) {{ break }}; $t += $r }}; "
            f"Write-Output ([Convert]::ToBase64String($b, 0, $t)) }} "
            f"finally {{ $f.Close() }}")


def _write_stmts(win: str, data: bytes) -> list:
    """Write `data` whole to `win`: under a temporary name, then moved over it, so a
    reader never sees half of it."""
    tmp = ps_str(win + ".tacli-tmp")
    stmts = []
    view = memoryview(data)
    for off in range(0, max(len(data), 1), CHUNK):
        b64 = base64.b64encode(view[off:off + CHUNK]).decode("ascii")
        if off == 0:
            stmts.append(f"[IO.File]::WriteAllBytes({tmp}, [Convert]::FromBase64String('{b64}'))")
        else:
            stmts.append(f"$s = [IO.File]::Open({tmp}, 'Append', 'Write'); try {{ "
                         f"$c = [Convert]::FromBase64String('{b64}'); "
                         f"$s.Write($c, 0, $c.Length) }} finally {{ $s.Close() }}")
    stmts.append(f"Move-Item -LiteralPath {tmp} -Destination {ps_str(win)} -Force")
    return stmts


def user_profile(ssh: str, key=None) -> str:
    """The SSH user's profile folder on the remote machine: where `remote add` puts a
    test folder when it is not told where."""
    out = session_for(ssh, key).run([f"Write-Output {ps_b64('$env:USERPROFILE')}"])
    prof = _unb64(out[0]) if out else ""
    if not prof.strip():
        raise RemoteError("the remote machine reported no user profile folder")
    return prof.strip()


def check_folders(player: str, folder: str):
    """The test folder is neither the player's folder, nor inside it, nor around it:
    nothing tacli writes may land in what the player plays from. This compares the
    strings, which is only the first check: `Remote.resolve_folders` asks the file
    system, before anything is written, what they name. A component the file system
    would rename (a trailing dot or space) or a wildcard is refused here."""
    for p in (player, folder):
        if not re.fullmatch(r"[A-Za-z]:\\.+", p or ""):
            raise ValueError(f"{p!r} is not an absolute Windows path (C:\\...)")
        for c in p[3:].split("\\"):
            if c and (c != c.rstrip(". ") or any(ch in c for ch in '*?"<>|/:')):
                raise ValueError(f"{p!r}: the component {c!r} names another folder than it "
                                 f"spells (a trailing dot or space) or is not a folder name")
    a, b = player.lower().rstrip("\\") + "\\", folder.lower().rstrip("\\") + "\\"
    if a == b or b.startswith(a) or a.startswith(b):
        raise ValueError(f"the test folder {folder} and the player's folder {player} "
                         f"overlap; the test folder must be separate")


# ------------------------------------------------------------- the log mirror

_HEADER_RX = re.compile(rb"^(?:# )?log: run (\S+) part (\d+) ")


def _key(head: bytes):
    m = _HEADER_RX.match(head)
    return (m.group(1).decode(), int(m.group(2))) if m else None


def _stream_of(name: str):
    """`tagpu.3.log` -> `tagpu`; the stream names talog knows."""
    m = re.fullmatch(r"(.+?)(?:\.\d+)?\.log", name)
    return m.group(1) if m else None


def _read_keyed(remote: Remote, path: str, key, start: int, end: int):
    """Bytes [start, end) of a log file, or None if the file under that name is no
    longer the one whose header is `key`. Each statement reads the header and its
    chunk through ONE open handle, so both come from the same file even when the
    sink renames it between two statements -- a rename moves the name, not the
    handle's file."""
    p = ps_str(path)
    data = bytearray()
    pos = start
    while pos < end:
        want = min(CHUNK, end - pos)
        out = remote.run([
            f"$f = [IO.File]::Open({p}, 'Open', 'Read', 'ReadWrite, Delete'); try {{ "
            f"$n = [Math]::Min({HEAD_BYTES}, $f.Length); $h = New-Object byte[] $n; "
            f"$r = $f.Read($h, 0, $n); Write-Output ([Convert]::ToBase64String($h, 0, $r)); "
            f"[void]$f.Seek({pos}, 'Begin'); $b = New-Object byte[] {want}; $t = 0; "
            f"while ($t -lt {want}) {{ $r = $f.Read($b, $t, {want} - $t); "
            f"if ($r -le 0) {{ break }}; $t += $r }}; "
            f"Write-Output ([Convert]::ToBase64String($b, 0, $t)) }} finally {{ $f.Close() }}"],
            timeout=120.0)
        if len(out) != 2 or _key(base64.b64decode(out[0])) != key:
            return None
        chunk = base64.b64decode(out[1])
        if not chunk:
            break
        data += chunk
        pos += len(chunk)
    return bytes(data)


# A log file's name as the sink writes it: `<stream>.log` or `<stream>.<n>.log`. Names
# the remote machine sends back are matched against this before they become local
# paths, so no listing can name a file outside the mirror.
_LOG_NAME_RX = re.compile(r"[A-Za-z0-9_-]{1,64}(?:\.[0-9]{1,4})?\.log")


def sync_logs(remote: Remote, local_root: Path, attempts: int = 4) -> Path:
    """Mirror the current run of every log stream under `<test folder>\\log` into
    `<local_root>/log`, so talog reads it exactly as it reads a local gamedir.

    A file is known by its header (`run/part`), as talog knows it: after a rotation
    the file that was `tagpu.log` is `tagpu.1.log`, and its bytes are reused from
    the mirror, so only what was appended since the last sync crosses the link, and
    a file whose name, header and length are all unchanged is not rewritten. Files of
    older runs are dropped from the mirror (every verb reads the current run). A
    rotation between the listing and a read shows up as a header that no longer
    matches or a file that is gone, and the sync starts again, `attempts` times."""
    logdir = ntpath.join(remote.folder, "log")
    mirror = Path(local_root) / "log"
    mirror.mkdir(parents=True, exist_ok=True)
    for _ in range(attempts):
        listing = remote.run([
            f"if ([IO.Directory]::Exists({ps_str(logdir)})) {{ Get-ChildItem -LiteralPath "
            f"{ps_str(logdir)} -Filter *.log -File | ForEach-Object {{ $f = [IO.File]::Open("
            f"$_.FullName, 'Open', 'Read', 'ReadWrite, Delete'); try {{ $n = [Math]::Min("
            f"{HEAD_BYTES}, $f.Length); $b = New-Object byte[] $n; $r = $f.Read($b, 0, $n); "
            f"Write-Output ({_B64_NAME} + '|' + $f.Length + '|' + [Convert]::ToBase64String("
            f"$b, 0, $r)) }} finally {{ $f.Close() }} }} }}"])
        remote_files = []
        for line in listing:
            name, length, head = line.split("|", 2)
            name = _unb64(name)
            if _LOG_NAME_RX.fullmatch(name):
                remote_files.append((name, int(length), base64.b64decode(head)))
        current = {}
        for name, _, head in remote_files:
            st = _stream_of(name)
            if st and name == f"{st}.log":
                current[st] = _key(head)
        keep = [(n, ln, h) for n, ln, h in remote_files
                if _stream_of(n) in current and (
                    n == f"{_stream_of(n)}.log" or (
                        _key(h) and current[_stream_of(n)]
                        and _key(h)[0] == current[_stream_of(n)][0]))]
        local = {}
        for p in mirror.glob("*.log"):
            with open(p, "rb") as f:
                k = _key(f.read(HEAD_BYTES))
            if k:
                local[k] = p
        staged, unchanged, moved = {}, set(), False
        for name, length, head in keep:
            k = _key(head)
            src = local.get(k) if k else None
            if src is not None and src.name == name and src.stat().st_size == length:
                unchanged.add(name)
                continue
            have = src.read_bytes() if src is not None else b""
            if len(have) > length:
                have = b""
            try:
                data = _read_keyed(remote, ntpath.join(logdir, name), k, len(have), length)
            except RemoteError as e:
                if e.kind not in ("FileNotFoundException", "DirectoryNotFoundException"):
                    raise
                data = None                 # rotated or deleted since the listing
            if data is None:
                moved = True
                break
            staged[name] = have + data
        if moved:
            continue
        for name, data in staged.items():
            (mirror / (name + ".sync")).write_bytes(data)
        for p in mirror.glob("*.log"):
            if p.name not in staged and p.name not in unchanged:
                p.unlink()
        for name in staged:
            os.replace(mirror / (name + ".sync"), mirror / name)
        return Path(local_root)
    raise RemoteError(f"the remote log kept rotating under the sync ({attempts} tries)")
