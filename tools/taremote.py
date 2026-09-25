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
  * every statement it emits is wrapped so that it must print a DONE marker, and
    `Session.run` fails when one is missing -- a line PowerShell did not execute is
    an error here, not an empty answer;
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
MARKER = "tacli-test-folder.txt"          # written by `remote add`; `rm` requires it
REG_EXPORT = "registry-before.reg"        # the export taken before a launch
REG_CHECK = "registry-after.reg"          # the export taken after a restore, to compare
COPIED = "copied.txt"                     # every file `remote add` copied, relative

# NO FILE OF THE PLAYER'S COPY IS REPLACED OR DELETED WITHOUT ITS ORIGINAL BESIDE IT
# (`RemotePath._guard`). Two tiers:
#   * PROTECTED, the files a launch replaces: `remote add` records each one's original
#     state up front -- a copy (`.tacli-original`) or, when the player's folder had
#     none, an empty `.tacli-absent` -- and nothing touches one while that record is
#     missing, since a copy made later could be of a file tacli already changed;
#   * every other file listed in COPIED: the first replace or delete makes the
#     `.tacli-original` itself and checks its hash first. tacli has not written such
#     a file before that moment, so what it copies is still the player's original.
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


def ps_script(statements, token: str, batch: int) -> str:
    """THE one place a PowerShell script is made: one statement a line, a blank line
    at the end.

    Each statement is checked (`ps_check_statement`) and wrapped so that it runs only
    while every statement before it succeeded, reports a failure as an ERR line
    carrying the .NET exception's type and its message in base64 (stdout is in the
    console's code page, and the message is in the machine's language), and prints
    `DONE <i>` whether it
    succeeded or not. A line PowerShell skipped therefore shows up as a missing DONE,
    which `Session.run` turns into an error. The wrapper is checked too: the rule is
    about what goes down the pipe, not about what the caller meant.

    `token` makes the markers unforgeable by anything a statement prints; `batch`
    numbers the END marker so a late line from an earlier batch is never read as
    this one's.
    """
    if not re.fullmatch(r"[0-9a-f]{8,32}", token or ""):
        raise PSScriptError("the marker token must be 8-32 lowercase hex digits")
    tag = f"@@{token}"
    lines = ["$__ok = $true"]
    for i, s in enumerate(statements):
        ps_check_statement(s)
        lines.append(
            f"if ($__ok) {{ try {{ {s} }} catch {{ $__ok = $false; "
            f"Write-Output ('{tag} ERR {i} ' + $_.Exception.GetType().Name + ' ' + "
            f"[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($_.Exception.Message))) }} }}; "
            f"Write-Output '{tag} DONE {i}'")
    lines.append(f"Write-Output '{tag} END {int(batch)}'")
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
        self._send(SESSION_INIT, timeout=60.0)

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
            self.proc.stdin.write(script.encode("ascii"))
            self.proc.stdin.flush()
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
            word, _, rest = line[len(tag):].partition(" ")
            if word == "END":
                if rest.strip() == str(self.batch):
                    break
                continue                        # a stale batch's marker
            if word == "DONE":
                done.add(int(rest))
            elif word == "ERR":
                idx, _, rest = rest.partition(" ")
                kind, _, msg = rest.partition(" ")
                msg = base64.b64decode(msg.strip() or "").decode("utf-8", "replace")
                msg = " ".join(msg.split())
                raise_after = RemoteError(f"{msg} [{kind}] (in: "
                                          f"{statements[int(idx)][:120]})", kind)
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
                out = self._run([
                    f"$f = [IO.File]::Open({p}, 'Open', 'Read', 'ReadWrite, Delete'); try {{ "
                    f"[void]$f.Seek({pos}, 'Begin'); $b = New-Object byte[] {want}; $t = 0; "
                    f"while ($t -lt {want}) {{ $r = $f.Read($b, $t, {want} - $t); "
                    f"if ($r -le 0) {{ break }}; $t += $r }}; "
                    f"Write-Output ([Convert]::ToBase64String($b, 0, $t)) }} "
                    f"finally {{ $f.Close() }}"], timeout=120.0)
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
                         f"{ps_str(pattern)} -File | ForEach-Object {{ Write-Output $_.Name }} }}"])
        return [self / name for name in out if name]

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
        return [f"if ([IO.File]::Exists({p}) -and -not [IO.File]::Exists({b}) -and "
                f"[IO.File]::Exists({listed}) -and ([IO.File]::ReadAllLines({listed}) "
                f"-contains {ps_str(chr(92).join(self.parts))})) {{ Copy-Item -LiteralPath {p} "
                f"-Destination {b}; if ((Get-FileHash -LiteralPath {p}).Hash -ne (Get-FileHash "
                f"-LiteralPath {b}).Hash) {{ throw ('the backup of ' + {p} + ' differs from "
                f"it') }} }}"]

    def write_bytes(self, data: bytes):
        """Replace the file whole: written under a temporary name, then moved over
        the target, so a reader never sees half of it."""
        tmp = ps_str(self.win + ".tacli-tmp")
        stmts = self._guard()
        first = True
        view = memoryview(data)
        for off in range(0, max(len(data), 1), CHUNK):
            b64 = base64.b64encode(view[off:off + CHUNK]).decode("ascii")
            if first:
                stmts.append(f"[IO.File]::WriteAllBytes({tmp}, [Convert]::FromBase64String('{b64}'))")
                first = False
            else:
                stmts.append(f"$s = [IO.File]::Open({tmp}, 'Append', 'Write'); try {{ "
                             f"$c = [Convert]::FromBase64String('{b64}'); "
                             f"$s.Write($c, 0, $c.Length) }} finally {{ $s.Close() }}")
        stmts.append(f"Move-Item -LiteralPath {tmp} -Destination {ps_str(self.win)} -Force")
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
                        "Write-Output ('' + $_.Id + '|' + $p) }"])
        rows = []
        for line in out:
            pid, _, path = line.partition("|")
            if pid.strip().isdigit():
                rows.append((int(pid), path))
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
        a check that it is the SSH login's user: TA reads HKCU, so the registry this
        session exports must be the hive the game will use."""
        out = self.run([
            "Write-Output ('' + (Get-CimInstance Win32_ComputerSystem).UserName)",
            "Write-Output ([Security.Principal.WindowsIdentity]::GetCurrent().Name)"])
        console = (out[0] if out else "").strip()
        me = (out[1] if len(out) > 1 else "").strip()
        if not console:
            raise RemoteError("nobody is logged on at the remote console, so a game "
                              "launched there has no desktop to open on")
        if console.lower() != me.lower():
            raise RemoteError(f"the console user is {console} but SSH logs in as {me}: "
                              f"the game would read another user's registry than the one "
                              f"tacli saves and restores")
        return console

    # -- the test folder
    def check_copy(self) -> dict:
        """What `copy_player` needs to be true first: the player's folder is a game
        folder, the test folder does not exist yet, and its drive has the room."""
        src, dst = ps_str(self.player), ps_str(self.folder)
        out = self.run([
            f"if (-not [IO.File]::Exists({ps_str(ntpath.join(self.player, 'TotalA.exe'))})) "
            f"{{ throw 'the player''s folder has no TotalA.exe' }}",
            f"if (Test-Path -LiteralPath {dst}) {{ throw 'the test folder already exists' }}",
            f"$m = Get-ChildItem -LiteralPath {src} -Recurse -File -Force | Measure-Object "
            f"-Property Length -Sum; Write-Output ('' + $m.Count + ' ' + $m.Sum)",
            f"$d = (Get-Item -LiteralPath ([IO.Path]::GetPathRoot({dst}))).PSDrive; "
            f"Write-Output ('' + $d.Free)",
        ], timeout=300.0)
        files, size = (int(x) for x in out[0].split())
        free = int(out[1]) if len(out) > 1 and out[1].strip().isdigit() else None
        if free is not None and free < size + (256 << 20):
            raise RemoteError(f"the test folder's drive has {free >> 20} MB free and the "
                              f"copy needs {size >> 20} MB")
        return {"files": files, "bytes": size}

    def copy_player(self, instance: str) -> dict:
        """Copy the player's folder, once, into the test folder, then record the
        original of every PROTECTED file and leave the marker `rm` requires. Reads
        the player's folder and writes nothing into it."""
        src, dst = ps_str(self.player), ps_str(self.folder)
        self.run([
            f"[void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName({dst}))",
            f"$ErrorActionPreference = 'Continue'; & robocopy.exe {src} {dst} /E /COPY:DAT "
            f"/DCOPY:T /R:1 /W:1 /NP /NFL /NDL /NJH /NJS 2>$null | Out-Null; "
            f"$rc = $LASTEXITCODE; $ErrorActionPreference = 'Stop'; "
            f"if ($rc -ge 8) {{ throw ('robocopy exited ' + $rc) }}",
        ], timeout=1800.0)
        out = self.run([
            f"$m = Get-ChildItem -LiteralPath {p} -Recurse -File -Force | Measure-Object "
            f"-Property Length -Sum; Write-Output ('' + $m.Count + ' ' + $m.Sum)"
            for p in (src, dst)], timeout=300.0)
        if out[0] != out[1]:
            raise RemoteError(f"the copy holds {out[1]} (files bytes) and the player's "
                              f"folder {out[0]}: the test folder is incomplete")
        # the list `_guard` reads, taken before tacli adds a file of its own
        self.run([f"$n = {len(self.folder) + 1}; $l = @(Get-ChildItem -LiteralPath {dst} -Recurse "
                  f"-File -Force | ForEach-Object {{ $_.FullName.Substring($n) }}); "
                  f"[void][IO.Directory]::CreateDirectory({ps_str(self.state(''))}); "
                  f"[IO.File]::WriteAllLines({ps_str(self.state(COPIED))}, [string[]]$l)"],
                 timeout=300.0)
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
        mark = f"instance={instance}\nplayer={self.player}\n"
        stmts.append(f"[void][IO.Directory]::CreateDirectory({ps_str(self.state(''))})")
        stmts.append(f"[IO.File]::WriteAllText({ps_str(ntpath.join(self.folder, MARKER))}, "
                     f"{ps_str(mark)})")
        backups = self.run(stmts)
        files, size = (int(x) for x in out[0].split())
        return {"files": files, "bytes": size, "protected": backups}

    def remove_folder(self, instance: str):
        """Delete the test folder -- only when its marker names this instance, so a
        metadata file pointing anywhere else deletes nothing."""
        marker = ps_str(ntpath.join(self.folder, MARKER))
        self.run([
            f"if (-not [IO.File]::Exists({marker})) {{ throw 'no tacli marker in the test "
            f"folder; nothing was deleted' }}",
            f"$t = [IO.File]::ReadAllText({marker}); if (-not $t.StartsWith("
            f"{ps_str('instance=' + instance + chr(10))})) {{ throw ('the marker names '"
            f" + $t.Split([char]10)[0] + '; nothing was deleted') }}",
            f"Remove-Item -LiteralPath {ps_str(self.folder)} -Recurse -Force",
        ], timeout=900.0)

    # -- the scheduled task
    def task_start(self, args=()):
        """Launch TotalA.exe on the console user's desktop. A process started from
        the SSH session runs where nobody can see it; a scheduled task with an
        interactive principal runs on the desktop, and needs no password."""
        folder, task = ps_str(self.folder), self.task_parts()
        exe = ps_str(ntpath.join(self.folder, "TotalA.exe"))
        argline = " ".join(args)
        action = (f"$a = New-ScheduledTaskAction -Execute {exe} -WorkingDirectory {folder}"
                  + (f" -Argument {ps_str(argline)}" if argline else ""))
        self.run([
            action,
            f"$pr = New-ScheduledTaskPrincipal -UserId {ps_str(self.console_user)} "
            f"-LogonType Interactive",
            "$st = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) "
            "-AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew",
            f"Register-ScheduledTask -TaskPath {ps_str(task[0])} -TaskName {ps_str(task[1])} "
            f"-Action $a -Principal $pr -Settings $st -Force | Out-Null",
            f"Start-ScheduledTask -TaskPath {ps_str(task[0])} -TaskName {ps_str(task[1])}",
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

    # -- the registry
    def state(self, name: str) -> str:
        return ntpath.join(self.folder, STATE_DIR, name)

    def reg_save(self) -> dict:
        """Export TA's key into the test folder's state directory. Returns what the
        restore needs: whether the key existed, and the export's SHA-256."""
        exp = ps_str(self.state(REG_EXPORT))
        out = self.run([
            f"[void][IO.Directory]::CreateDirectory({ps_str(self.state(''))})",
            f"if (Test-Path -LiteralPath {ps_str(REG_PSPATH)}) {{ "
            f"$ErrorActionPreference = 'Continue'; & reg.exe export {ps_str(REG_KEY)} {exp} /y "
            f"2>$null | Out-Null; $rc = $LASTEXITCODE; $ErrorActionPreference = 'Stop'; "
            f"if ($rc -ne 0) {{ throw ('reg export exited ' + $rc) }}; "
            f"Write-Output ('present ' + (Get-FileHash -LiteralPath {exp} -Algorithm SHA256).Hash) }} "
            f"else {{ Write-Output 'absent' }}"])
        word, _, digest = (out[-1] if out else "").partition(" ")
        if word == "present":
            return {"present": True, "sha256": digest.strip(), "file": self.state(REG_EXPORT)}
        if word == "absent":
            return {"present": False, "sha256": None, "file": None}
        raise RemoteError(f"the registry export said {out!r}")

    def reg_digest(self) -> "str | None":
        """SHA-256 of an export of TA's key as it is now, or None when it is absent."""
        chk = ps_str(self.state(REG_CHECK))
        out = self.run([
            f"if (Test-Path -LiteralPath {ps_str(REG_PSPATH)}) {{ "
            f"$ErrorActionPreference = 'Continue'; & reg.exe export {ps_str(REG_KEY)} {chk} /y "
            f"2>$null | Out-Null; $rc = $LASTEXITCODE; $ErrorActionPreference = 'Stop'; "
            f"if ($rc -ne 0) {{ throw ('reg export exited ' + $rc) }}; "
            f"Write-Output (Get-FileHash -LiteralPath {chk} -Algorithm SHA256).Hash; "
            f"Remove-Item -LiteralPath {chk} -Force }} else {{ Write-Output 'absent' }}"])
        word = (out[-1] if out else "").strip()
        return None if word == "absent" else word

    def reg_restore(self, saved: dict) -> str:
        """Put TA's key back as `reg_save` found it: delete it, import the export, and
        prove it by exporting again and comparing the hashes. Refuses to delete the
        key unless the export it would import is there and unchanged."""
        exp = ps_str(self.state(REG_EXPORT))
        key, pspath = ps_str(REG_KEY), ps_str(REG_PSPATH)
        delete = (f"if (Test-Path -LiteralPath {pspath}) {{ $ErrorActionPreference = 'Continue'; "
                  f"& reg.exe delete {key} /f 2>$null | Out-Null; $rc = $LASTEXITCODE; "
                  f"$ErrorActionPreference = 'Stop'; if ($rc -ne 0) {{ throw ('reg delete "
                  f"exited ' + $rc) }} }}")
        if saved.get("present"):
            want = saved["sha256"]
            self.run([
                f"if (-not [IO.File]::Exists({exp})) {{ throw 'the export taken at launch is "
                f"missing; the key was left as it is' }}",
                f"$h = (Get-FileHash -LiteralPath {exp} -Algorithm SHA256).Hash; if ($h -ne "
                f"{ps_str(want)}) {{ throw ('the export taken at launch has changed (' + $h + "
                f"'); the key was left as it is') }}",
                delete,
                f"$ErrorActionPreference = 'Continue'; & reg.exe import {exp} 2>$null | Out-Null; "
                f"$rc = $LASTEXITCODE; $ErrorActionPreference = 'Stop'; if ($rc -ne 0) {{ "
                f"throw ('reg import exited ' + $rc + '; the export is still at ' + {exp}) }}",
            ])
            got = self.reg_digest()
            if got != want:
                raise RemoteError(f"TA's registry key reads {got} after the restore, not the "
                                  f"{want} exported at launch (the export is kept at "
                                  f"{self.state(REG_EXPORT)})")
            return f"restored TA's registry key ({want[:12]}…, verified by a fresh export)"
        self.run([delete])
        if self.reg_digest() is not None:
            raise RemoteError("TA's registry key is still there after deleting it")
        return "restored TA's registry key: it did not exist before the launch, and it is gone"

    def reg_set(self, values):
        """Write (subkey, name, kind, data) values under TA's key -- only ever after
        `reg_save`, whose restore puts every one of them back."""
        stmts = []
        for sub, name, kind, data in values:
            path = REG_PSPATH + (("\\" + sub) if sub else "")
            ptype = {"REG_DWORD": "DWord", "REG_SZ": "String"}[kind]
            val = str(int(data)) if kind == "REG_DWORD" else ps_str(data)
            stmts.append(f"if (-not (Test-Path -LiteralPath {ps_str(path)})) {{ "
                         f"New-Item -Path {ps_str(path)} -Force | Out-Null }}")
            stmts.append(f"New-ItemProperty -LiteralPath {ps_str(path)} -Name {ps_str(name)} "
                         f"-PropertyType {ptype} -Value {val} -Force | Out-Null")
        if stmts:
            self.run(stmts)


def user_profile(ssh: str, key=None) -> str:
    """The SSH user's profile folder on the remote machine: where `remote add` puts a
    test folder when it is not told where."""
    out = session_for(ssh, key).run(["Write-Output $env:USERPROFILE"])
    if not out or not out[0].strip():
        raise RemoteError("the remote machine reported no user profile folder")
    return out[0].strip()


def check_folders(player: str, folder: str):
    """The test folder is neither the player's folder, nor inside it, nor around it:
    nothing tacli writes may land in what the player plays from."""
    for p in (player, folder):
        if not re.fullmatch(r"[A-Za-z]:\\.+", p or ""):
            raise ValueError(f"{p!r} is not an absolute Windows path (C:\\...)")
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


def sync_logs(remote: Remote, local_root: Path, attempts: int = 4) -> Path:
    """Mirror the current run of every log stream under `<test folder>\\log` into
    `<local_root>/log`, so talog reads it exactly as it reads a local gamedir.

    A file is known by its header (`run/part`), as talog knows it: after a rotation
    the file that was `tagpu.log` is `tagpu.1.log`, and its bytes are reused from
    the mirror, so only what was appended since the last sync crosses the link.
    Files of older runs are dropped from the mirror (every verb reads the current
    run). A rotation between the listing and a read shows up as a header that no
    longer matches, and the sync starts again."""
    logdir = ntpath.join(remote.folder, "log")
    mirror = Path(local_root) / "log"
    mirror.mkdir(parents=True, exist_ok=True)
    for _ in range(attempts):
        listing = remote.run([
            f"if ([IO.Directory]::Exists({ps_str(logdir)})) {{ Get-ChildItem -LiteralPath "
            f"{ps_str(logdir)} -Filter *.log -File | ForEach-Object {{ $f = [IO.File]::Open("
            f"$_.FullName, 'Open', 'Read', 'ReadWrite, Delete'); try {{ $n = [Math]::Min("
            f"{HEAD_BYTES}, $f.Length); $b = New-Object byte[] $n; $r = $f.Read($b, 0, $n); "
            f"Write-Output ($_.Name + '|' + $f.Length + '|' + [Convert]::ToBase64String($b, 0, $r)) }} "
            f"finally {{ $f.Close() }} }} }}"])
        remote_files = []
        for line in listing:
            name, length, head = line.split("|", 2)
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
        staged, moved = {}, False
        for name, length, head in keep:
            k = _key(head)
            src = local.get(k) if k else None
            have = src.read_bytes() if src is not None else b""
            if len(have) > length:
                have = b""
            data = _read_keyed(remote, ntpath.join(logdir, name), k, len(have), length)
            if data is None:
                moved = True
                break
            staged[name] = have + data
        if moved:
            continue
        for name, data in staged.items():
            tmp = mirror / (name + ".sync")
            tmp.write_bytes(data)
        for p in mirror.glob("*.log"):
            if p.name not in staged:
                p.unlink()
        for name in staged:
            os.replace(mirror / (name + ".sync"), mirror / name)
        return Path(local_root)
    raise RemoteError(f"the remote log kept rotating under the sync ({attempts} tries)")
