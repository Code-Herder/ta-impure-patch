#!/usr/bin/env python3
"""taremote — a Windows machine over SSH, for tacli's remote instances.

Every channel between tacli and the DLL is a file in the game folder (the key and
eye files, the lever files, the trigger/result pairs, the `.ab` captures, `log\\`),
and the DLL on Windows reads and writes the same files. So a remote instance is a
game folder on another machine plus a way to read and write files there, list its
processes and start a scheduled task. TA's registry is one of those files too: in a
test launch the DLL answers TotalA.exe's registry calls from `tacli-state\\registry.txt`
(tagpu_regstore.h), so TA's settings key on the remote machine is never written, and
nothing here writes a registry value. (What the hooks do not reach, the Task Scheduler's
own records among it, is listed there.) This module is that way; tacli routes its verbs through it (`research/notes/tacli-design.md`,
"Remote instances").

THE TRANSPORT is one PowerShell per tacli command: `ssh <user>@<host> powershell
-NoProfile -NonInteractive -Command -`, fed statements on stdin and kept open, so a
statement costs one round trip instead of a PowerShell start-up. The remote login
shell is PowerShell with messages in the machine's own language, and PowerShell
reading stdin has one hazard that shapes everything below: A LINE THAT DOES NOT PARSE AS
ONE STATEMENT IS SKIPPED, with nothing but a parser error on stderr (`if ($true)` with no
block; while a statement split after an open brace is read on into the next line). So:

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

STATE_DIR = "tacli-state"                 # tacli's own files inside the test folder
MARKER = "tacli-test-folder.txt"          # written by `remote add` first; `rm` requires it
COPIED = "copied.txt"                     # `<SHA-256> <relative path>`, a line per copied file

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
    read the NEXT line as the rest of the statement: the next line's statement is then
    swallowed into this one, or the pair fails to parse and is skipped with only a
    parser error on stderr. So a statement here is one line that closes everything it
    opens:

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


def ps_script(statements, token: str, batch: int) -> str:
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
    one's. Every line is checked, the generated ones too: the rule is about what
    reaches PowerShell, not about what the caller meant.
    """
    if not re.fullmatch(r"[0-9a-f]{8,32}", token or ""):
        raise PSScriptError("the marker token must be 8-32 lowercase hex digits")
    tag, b = f"@@{token}", int(batch)
    lines = ["function __m($t) { Write-Output $t }", "$__seq = 0"]
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
        a check that it is the SSH login's user: the test folder and the registry store
        `remote add` seeds from HKCU are that user's, and the game must run as the
        player they were taken from."""
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
                              f"the game would run as another user than the one whose "
                              f"settings and folder the test starts from")
        return console

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

    # -- the scheduled task
    def task_start(self, argv):
        """Start TotalA.exe from the test folder on the console user's desktop. A process
        started from the SSH session runs where nobody can see it; a scheduled task with
        an interactive principal runs on the desktop, and needs no password. The action
        is the game itself, with the test folder as its working directory and TEST_TOKEN
        first on its command line: the DLL's sign of a test launch (tagpu_regstore.h).

        `-Priority 4` is NORMAL_PRIORITY_CLASS: a task's default, 7, is below normal,
        and the game inherits it -- every frame time measured on it would be a
        below-normal process's."""
        path, name = self.task_parts()
        argline = " ".join([TEST_TOKEN] + list(argv))
        self.run([
            f"$a = New-ScheduledTaskAction -Execute {ps_str(ntpath.join(self.folder, 'TotalA.exe'))} "
            f"-WorkingDirectory {ps_str(self.folder)} -Argument {ps_str(argline)}",
            f"$pr = New-ScheduledTaskPrincipal -UserId {ps_str(self.console_user)} "
            f"-LogonType Interactive",
            "$st = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) "
            "-AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew "
            "-Priority 4",
            f"Register-ScheduledTask -TaskPath {ps_str(path)} -TaskName {ps_str(name)} "
            f"-Action $a -Principal $pr -Settings $st -Force | Out-Null",
            f"Start-ScheduledTask -TaskPath {ps_str(path)} -TaskName {ps_str(name)}",
        ])

    def task_status(self):
        """(State, LastTaskResult) of this instance's task, or (None, None) when there is
        none. LastTaskResult is 0x41301 while the game runs, the game's exit code after
        it, and a Win32 or scheduler error (0x8007xxxx, 0x8004xxxx) when the task could
        not start it at all."""
        path, name = self.task_parts()
        out = self.run([f"$t = Get-ScheduledTask -TaskPath {ps_str(path)} -TaskName {ps_str(name)} "
                        f"-ErrorAction SilentlyContinue; if ($t) {{ $i = $t | Get-ScheduledTaskInfo; "
                        f"Write-Output ('' + $t.State + '|' + $i.LastTaskResult) }}"])
        if not out or "|" not in out[0]:
            return None, None
        state, _, result = out[0].partition("|")
        result = result.strip()
        return state.strip(), (int(result) & 0xFFFFFFFF if result.lstrip("-").isdigit() else None)

    def task_parts(self):
        path, _, name = self.task.rpartition("\\")
        return (path + "\\") if path else "\\", name

    def task_remove(self) -> str:
        """Remove this instance's task, then the task folder TASK_DIR when nothing is left
        in it (no task, hidden ones included, and no subfolder). The answer says what
        happened to the folder; a folder that could not be removed is reported, not
        fatal: the instance's own task is gone either way."""
        path, name = self.task_parts()
        folder = TASK_DIR.strip("\\")
        out = self.run([
            f"$t = Get-ScheduledTask -TaskPath {ps_str(path)} -TaskName {ps_str(name)} "
            f"-ErrorAction SilentlyContinue; if ($t) {{ Unregister-ScheduledTask "
            f"-TaskPath {ps_str(path)} -TaskName {ps_str(name)} -Confirm:$false }}",
            f"$__s = New-Object -ComObject Schedule.Service; $__s.Connect(); $__f = @($__s."
            f"GetFolder('\\').GetFolders(0) | Where-Object {{ $_.Name -eq {ps_str(folder)} }}); "
            f"if ($__f.Count -ne 1) {{ Write-Output 'absent' }} elseif ($__f[0].GetTasks(1).Count "
            f"-ne 0 -or $__f[0].GetFolders(0).Count -ne 0) {{ Write-Output 'kept' }} else {{ try "
            f"{{ $__s.GetFolder('\\').DeleteFolder({ps_str(folder)}, 0); Write-Output 'removed' "
            f"}} catch {{ Write-Output ('not removed: ' + $_.Exception.Message) }} }}"])
        return out[-1] if out else "absent"

    def priority(self, pid: int) -> str:
        """The priority class of a running process, as Windows names it."""
        out = self.run([f"Write-Output ('' + (Get-Process -Id {int(pid)}).PriorityClass)"])
        return out[0].strip() if out else ""

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

    # -- TA's registry: a file in the test folder (tagpu_regstore.h)
    def read_player_registry(self) -> "RegStore":
        """TA's registry as the player's user has it: every key under STORE_ROOT with its
        values, READ through `RegistryKey.OpenSubKey(name, $false)` -- read-only handles,
        through which nothing can be written. Names and string data are converted with
        Windows PowerShell's `Encoding.Default`, the ANSI code page the game's A
        functions use."""
        enc = "[Text.Encoding]::Default"
        out = self.run([
            "$__q = New-Object Collections.Queue; $__r = [Microsoft.Win32.Registry]::CurrentUser."
            f"OpenSubKey({ps_str(STORE_ROOT[5:])}, $false); if ($__r) {{ $__q.Enqueue($__r) }}; "
            "while ($__q.Count -gt 0) { $__k = $__q.Dequeue(); "
            f"$__kn = [Convert]::ToBase64String({enc}.GetBytes($__k.Name)); "
            "Write-Output ('K|' + $__kn); foreach ($__n in $__k.GetValueNames()) { "
            "$__t = [int]$__k.GetValueKind($__n); $__v = $__k.GetValue($__n, $null, "
            "'DoNotExpandEnvironmentNames'); "
            f"if ($__t -eq 1 -or $__t -eq 2) {{ $__b = {enc}.GetBytes([string]$__v + [char]0) }} "
            "elseif ($__t -eq 4) { $__b = [BitConverter]::GetBytes([int32]$__v) } "
            "elseif ($__t -eq 11) { $__b = [BitConverter]::GetBytes([int64]$__v) } "
            f"elseif ($__t -eq 7) {{ $__b = {enc}.GetBytes(([string[]]$__v -join [char]0) + "
            "[char]0 + [char]0) } else { $__b = [byte[]]$__v; if ($__t -lt 0) { $__t = 0 } }; "
            "if ($null -eq $__b) { $__b = [byte[]]@() }; "
            f"Write-Output ('V|' + $__kn + '|' + [Convert]::ToBase64String({enc}.GetBytes($__n)) "
            "+ '|' + $__t + '|' + [Convert]::ToBase64String($__b)) }; "
            "foreach ($__s in $__k.GetSubKeyNames()) { $__c = $__k.OpenSubKey($__s, $false); "
            "if ($__c) { $__q.Enqueue($__c) } }; $__k.Close() }",
        ], timeout=120.0)
        return RegStore.from_listing(out)

    def dll_has_test_mode(self) -> bool:
        """Whether the test folder's ddraw.dll is one tacli may launch: every line of
        TEST_MODE_MARKS is in its bytes (read on the far side, latin-1, so every byte is
        one character)."""
        dll = ps_str(ntpath.join(self.folder, "ddraw.dll"))
        has = " -and ".join(f"$__t.Contains({ps_str(m.decode('ascii'))})" for m in TEST_MODE_MARKS)
        out = self.run([f"if ([IO.File]::Exists({dll})) {{ $__t = [Text.Encoding]::GetEncoding(28591)."
                        f"GetString([IO.File]::ReadAllBytes({dll})); Write-Output ({has}) }} "
                        f"else {{ Write-Output 'False' }}"],
                       timeout=120.0)
        return out == ["True"]


TASK_DIR = "\\tacli\\"          # every remote instance's task lives in this folder
TASK_RUNNING = 0x41301          # SCHED_S_TASK_RUNNING


# ---------------------------------------------------------- the registry store
#
# TA's registry in a test launch is `tacli-state\registry.txt`: the DLL serves TotalA.exe's
# registry calls from it and writes the game's changes back to it (tagpu_regstore.h, which
# specifies the format and the limits; the two must agree byte for byte). A remote
# instance's is seeded by `remote add` reading the player's key, a local instance's by
# tacli reading the template prefix's user.reg; `launch` puts its test values in it, and
# tacli writes no value of TA's key into a registry.

STORE = "registry.txt"
STORE_ROOT = r"HKCU\Software\Cavedog Entertainment"
STORE_TA = STORE_ROOT + r"\Total Annihilation"
REG_SZ, REG_DWORD = 1, 4
# The token every launch whose DLL serves the store puts first on TotalA.exe's command
# line: the DLL's sign of a test launch, which the engine ignores (tagpu_regstore.c, RS_TOKEN).
TEST_TOKEN = "-xtacli-test"
# Lines only a DLL that fails closed in test mode and closes the -r switch logs: tacli
# starts no remote game with a DLL that lacks any of them, and runs a local game with such
# a DLL on the registry every prefix shares, saying so.
TEST_MODE_R_CLOSED = "the -r switch (DirectPlay registration through dsetup.dll) is ignored"
TEST_MODE_MARKS = (b"registry: TEST MODE, entered by", TEST_MODE_R_CLOSED.encode("ascii"))
# The DLL's own account of a test launch, the first `registry: ` line of its run: served
# (`... entered by <signal> -- <exe>'s registry is ...`) or refused (`... entered by
# <signal>, but <what>: the game is not run`), tagpu_regstore.c. A served run then logs the
# -r closure (TEST_MODE_R_CLOSED), or its own refusal, from tagpu_patches.c; every refusal
# of a test launch ends with TEST_MODE_REFUSED.
TEST_MODE_SERVED = "registry: TEST MODE, entered by "
TEST_MODE_REFUSED = ": the game is not run"
# The DLL's limits (tagpu_regstore.c): a store past any of them does not load whole, and
# the game is not run, so RegStore refuses to read or write one.
STORE_MAX_KEY = 511             # bytes of a key path
STORE_MAX_NAME = 1023           # bytes of a value name
STORE_MAX_DATA = 65536          # bytes of a value; an sz's text and its NUL
STORE_MAX_VALUES = 512          # values of one key
STORE_MAX_KEYS = 1024           # keys: the root and every key under it
STORE_MAX_FILE = 4 << 20        # bytes of the file
_HIVES = {"HKEY_CURRENT_USER": "HKCU"}


def _esc(b: bytes) -> str:
    return "".join(f"%{c:02X}" if c == 0x25 or c < 0x20 or c >= 0x7F else chr(c) for c in b)


def _unesc(s: str) -> bytes:
    out, i = bytearray(), 0
    while i < len(s):
        c = s[i]
        if not " " <= c <= "~":
            raise ValueError(f"a raw {c!r} where the format escapes it")
        if c == "%":
            h = s[i + 1:i + 3]
            if len(h) != 2 or any(x not in "0123456789abcdefABCDEF" for x in h):
                raise ValueError(f"a broken escape at {s[i:i + 3]!r}")
            if not int(h, 16):
                raise ValueError("an escaped NUL")
            out.append(int(h, 16))
            i += 3
        else:
            out.append(ord(c))
            i += 1
    return bytes(out)


def _b(s) -> bytes:
    return s.encode("ascii") if isinstance(s, str) else bytes(s)


class RegStore:
    """The keys and values of a registry store, in file order. Key paths and value names
    are the ANSI bytes the game's A functions see, matched without case as the registry
    matches them; a value is (name, type, data bytes). The DLL's limits (STORE_MAX_*)
    hold for every store this class holds: past one, ValueError."""

    def __init__(self):
        self._keys = {}         # lower path -> (path, {lower name: (name, type, data)})

    def _key(self, path: bytes):
        """The key, created with every missing key between it and STORE_ROOT."""
        root = STORE_ROOT.encode("ascii")
        low = path.lower()
        if not (low == root.lower() or low.startswith(root.lower() + b"\\")) \
                or b"\\\\" in path or path.endswith(b"\\"):
            raise ValueError(f"{path!r} is not a key under {STORE_ROOT}")
        if len(path) > STORE_MAX_KEY:
            raise ValueError(f"a key path of {len(path)} bytes (the DLL takes {STORE_MAX_KEY})")
        cur = path[:len(root)]
        parts = path[len(root) + 1:].split(b"\\") if len(path) > len(root) else []
        new = sum(1 for i in range(len(parts) + 1)
                  if (b"\\".join([cur] + parts[:i])).lower() not in self._keys)
        if len(self._keys) + new > STORE_MAX_KEYS:
            raise ValueError(f"more than {STORE_MAX_KEYS} keys (the DLL's limit)")
        self._keys.setdefault(cur.lower(), (cur, {}))
        for part in parts:
            cur += b"\\" + part
            self._keys.setdefault(cur.lower(), (cur, {}))
        return self._keys[low]

    def add_key(self, key):
        """The key, with every missing key between it and STORE_ROOT; one that holds no
        value is still a key the game can open."""
        self._key(_b(key))

    def set(self, key, name, kind: int, data: bytes):
        name, data = _b(name), bytes(data)
        if len(name) > STORE_MAX_NAME:
            raise ValueError(f"a value name of {len(name)} bytes (the DLL takes {STORE_MAX_NAME})")
        if len(data) > STORE_MAX_DATA:
            raise ValueError(f"a value of {len(data)} bytes (the DLL takes {STORE_MAX_DATA})")
        vals = self._key(_b(key))[1]
        if name.lower() not in vals and len(vals) >= STORE_MAX_VALUES:
            raise ValueError(f"more than {STORE_MAX_VALUES} values in one key (the DLL's limit)")
        vals[name.lower()] = (name, int(kind), data)

    def set_dword(self, key, name, value: int):
        self.set(key, name, REG_DWORD, (int(value) & 0xFFFFFFFF).to_bytes(4, "little"))

    def set_sz(self, key, name, text: str):
        self.set(key, name, REG_SZ, text.encode("ascii") + b"\0")

    def get(self, key, name):
        k = self._keys.get(_b(key).lower())
        return None if k is None else k[1].get(_b(name).lower())

    def keys(self):
        return [path for path, _ in self._keys.values()]

    def values(self, key):
        k = self._keys.get(_b(key).lower())
        return [] if k is None else list(k[1].values())

    def format(self) -> bytes:
        lines = ["# tacli registry store: <key> or <key>\\t<name>\\t<type>\\t<data> "
                 "(tagpu_regstore.h)"]
        for path, vals in self._keys.values():
            lines.append(_esc(path))
            for name, kind, data in vals.values():
                if kind == REG_DWORD and len(data) == 4:
                    spec = f"dword\t{int.from_bytes(data, 'little')}"
                elif kind == REG_SZ and data.endswith(b"\0") and b"\0" not in data[:-1]:
                    spec = "sz\t" + _esc(data[:-1])
                else:
                    spec = f"hex({kind})\t{data.hex()}"
                lines.append(f"{_esc(path)}\t{_esc(name)}\t{spec}")
        data = ("\r\n".join(lines) + "\r\n").encode("ascii")
        if len(data) > STORE_MAX_FILE:
            raise ValueError(f"a store file of {len(data)} bytes (the DLL takes {STORE_MAX_FILE})")
        return data

    @classmethod
    def parse(cls, data: bytes) -> "RegStore":
        """The store a file holds; ValueError names the first line that does not parse or
        the limit it passes, as the DLL refuses it (it then does not run the game)."""
        if len(data) > STORE_MAX_FILE:
            raise ValueError(f"a store file of {len(data)} bytes (the DLL takes {STORE_MAX_FILE})")
        store = cls()
        for n, line in enumerate(data.split(b"\n"), 1):
            line = line[:-1] if line.endswith(b"\r") else line
            if not line or line.startswith(b"#"):
                continue
            try:
                f = line.decode("ascii").split("\t")
                if len(f) not in (1, 4):
                    raise ValueError(f"{len(f)} fields")
                key = _unesc(f[0])
                store._key(key)
                if len(f) == 1:
                    continue
                name, kind, val = _unesc(f[1]), f[2], f[3]
                if kind == "dword":
                    if not re.fullmatch(r"[0-9]{1,10}", val) or int(val) > 0xFFFFFFFF:
                        raise ValueError(f"dword {val!r}")
                    store.set(key, name, REG_DWORD, int(val).to_bytes(4, "little"))
                elif kind == "sz":
                    store.set(key, name, REG_SZ, _unesc(val) + b"\0")
                elif re.fullmatch(r"hex\([0-9]{1,10}\)", kind) and int(kind[4:-1]) <= 0xFFFFFFFF:
                    if not re.fullmatch(r"(?:[0-9a-fA-F]{2})*", val):
                        raise ValueError(f"hex data {val[:20]!r}")
                    store.set(key, name, int(kind[4:-1]), bytes.fromhex(val))
                else:
                    raise ValueError(f"type {kind!r}")
            except (ValueError, UnicodeDecodeError) as e:
                raise ValueError(f"line {n}: {e}") from None
        return store

    @classmethod
    def from_listing(cls, lines) -> "RegStore":
        """The store `Remote.read_player_registry` read: `K|<key>` and
        `V|<key>|<name>|<type>|<data>` lines, each field base64."""
        store = cls()
        for line in lines:
            f = line.split("|")
            if f[0] not in ("K", "V"):
                continue
            hive, sep, rest = base64.b64decode(f[1]).partition(b"\\")
            key = _HIVES.get(hive.decode("ascii", "replace"), "?").encode("ascii") + sep + rest
            if f[0] == "K":
                store.add_key(key)
            else:
                store.set(key, base64.b64decode(f[2]), int(f[3]) & 0xFFFFFFFF,
                          base64.b64decode(f[4]))
        return store


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
    """Write `data` whole to `win`: under a temporary name, then put in its place --
    `[IO.File]::Replace` over a file that is there (never Move-Item -Force, which in
    Windows PowerShell 5.1 deletes the target and then moves, so every write had a moment
    with no file), `[IO.File]::Move` onto a name that is free. A reader sees the old file
    or the new one, whole.

    Replace keeps the old file as `.tacli-old`, deleted once the swap is done. Its one
    failure after the swap began, ReplaceFile's error 1177 (ERROR_UNABLE_TO_MOVE_REPLACEMENT_2),
    leaves the target renamed to `.tacli-old` and the new file under the temporary name:
    the statement fails with that error, and until the next write the name is missing
    (the registry store then reads as missing: `launch` refuses the test folder, naming
    the `.tacli-old`, and the DLL does not run the game). The next write through here
    first moves a `.tacli-old` back when the name is missing, or deletes a stale one."""
    tmp = ps_str(win + ".tacli-tmp")
    old = ps_str(win + ".tacli-old")
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
    dst = ps_str(win)
    stmts.append(f"if (-not [IO.File]::Exists({dst}) -and [IO.File]::Exists({old})) {{ "
                 f"[IO.File]::Move({old}, {dst}) }} elseif ([IO.File]::Exists({old})) {{ "
                 f"[IO.File]::Delete({old}) }}; if ([IO.File]::Exists({dst})) {{ "
                 f"[IO.File]::Replace({tmp}, {dst}, {old}); [IO.File]::Delete({old}) }} "
                 f"else {{ [IO.File]::Move({tmp}, {dst}) }}")
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
    rotation inside the listing (a file renamed between its enumeration and its
    open) or between the listing and a read shows up as a file that is gone or a
    header that no longer matches, and the sync starts again, `attempts` times."""
    logdir = ntpath.join(remote.folder, "log")
    mirror = Path(local_root) / "log"
    mirror.mkdir(parents=True, exist_ok=True)
    for _ in range(attempts):
        try:
            listing = remote.run([
                f"if ([IO.Directory]::Exists({ps_str(logdir)})) {{ Get-ChildItem -LiteralPath "
                f"{ps_str(logdir)} -Filter *.log -File | ForEach-Object {{ $f = [IO.File]::Open("
                f"$_.FullName, 'Open', 'Read', 'ReadWrite, Delete'); try {{ $n = [Math]::Min("
                f"{HEAD_BYTES}, $f.Length); $b = New-Object byte[] $n; $r = $f.Read($b, 0, $n); "
                f"Write-Output ({_B64_NAME} + '|' + $f.Length + '|' + [Convert]::ToBase64String("
                f"$b, 0, $r)) }} finally {{ $f.Close() }} }} }}"])
        except RemoteError as e:
            if e.kind not in ("FileNotFoundException", "DirectoryNotFoundException"):
                raise
            continue                        # a listed file was rotated away before its open
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
