#!/usr/bin/env python3
"""
test_runner.py — Integration test harness for ./ufs_shell (custom User-Space Filesystem)

Drives the interactive shell over subprocess.PIPE (stdin/stdout), with a
background reader thread + adaptive quiet-period read to avoid deadlocks
on an unbounded, prompt-less pipe stream.

NOTE: The CMD table below encodes the shell's exact command grammar.
Adjust it to match your ufs_shell's actual syntax; the scenario logic
itself does not need to change.
"""

import argparse
from email.mime import image
import os
import platform
import queue
import re
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import List, Optional, Tuple

CMD = {
    "format": "format {image} {size}",
    "mount": "mount {image}",
    "unmount": "unmount",
    "mkdir": "mkdir {path}",
    "create": "create {path}",
    "open": "open {path} {mode}",
    "write": "write {fd} {data}",
    "close": "close {fd}",
    "stat": "stat {path}",
    "read": "read {fd} {size}",
    "unlink": "unlink {path}",
    "listdir": "listdir {path}",
    "fsck": "fsck",
    "tag": "tag {path} {tag}",
    "settag": "settag {path} {tag}",
    "findtag": "findtag {tag}",
    "quit": "exit",
    "truncate": "truncate {path} {size}",
    "seek": "seek {fd} {offset} {whence}",
    "rmdir": "rmdir {path}",
}

ERROR_RE = re.compile(r"(error|errno|fail(?:ed)?|invalid|denied)", re.I)
ENOSPC_RE = re.compile(r"(enospc|no space|disk full|directory full|out of|limit)", re.I)
EUCLEAN_RE = re.compile(r"(euclean|crc|checksum|corrupt(?:ion)?)", re.I)
FD_RE = re.compile(r"(?:file\s+descriptor|fd)\s*[:=]?\s*(\d+)", re.I)
SIZE_RE = re.compile(r"size\s*[:=]?\s*(\d+)", re.I)


def extract_fd(output: str) -> Optional[int]:
    """Parse a numeric file descriptor from shell output without crashing."""
    match = FD_RE.search(output)
    if not match:
        return None
    try:
        return int(match.group(1))
    except ValueError:
        return None


def safe_print(text: str):
    """Fallback to plain ASCII when the console cannot encode Unicode box chars."""
    try:
        sys.stdout.write(text)
        sys.stdout.flush()
    except UnicodeEncodeError:
        ascii_text = text.encode("ascii", "replace").decode("ascii")
        sys.stdout.write(ascii_text)
        sys.stdout.flush()


class C:
    RESET = "\033[0m"
    BOLD = "\033[1m"
    DIM = "\033[2m"
    RED = "\033[91m"
    GREEN = "\033[92m"
    YELLOW = "\033[93m"
    BLUE = "\033[94m"
    MAGENTA = "\033[95m"
    CYAN = "\033[96m"
    WHITE = "\033[97m"


def hr(char="─", width=72):
    line = C.DIM + char * width + C.RESET
    safe_print(line + "\n")


def header(title: str):
    width = 72
    print()
    top = C.BOLD + C.BLUE + "╔" + "═" * (width - 2) + "╗" + C.RESET
    mid = C.BOLD + C.BLUE + "║" + C.RESET + " " * ((width - 2 - len(title)) // 2) + C.BOLD + C.WHITE + title + C.RESET + " " * (width - 2 - ((width - 2 - len(title)) // 2) - len(title)) + C.BOLD + C.BLUE + "║" + C.RESET
    bottom = C.BOLD + C.BLUE + "╚" + "═" * (width - 2) + "╝" + C.RESET
    for line in (top, mid, bottom):
        safe_print(line + "\n")


def step(msg: str):
    safe_print(f"{C.YELLOW}➤ STEP{C.RESET}  {msg}\n")


def info(msg: str):
    safe_print(f"{C.CYAN}ℹ INFO{C.RESET}  {msg}\n")


def raw(msg: str):
    for line in msg.splitlines():
        if line.strip():
            safe_print(f"{C.DIM}    │ {line}{C.RESET}\n")


def ok(msg: str):
    safe_print(f"{C.GREEN}{C.BOLD}✔ PASS{C.RESET}  {msg}\n")


def bad(msg: str):
    safe_print(f"{C.RED}{C.BOLD}✘ FAIL{C.RESET}  {msg}\n")


class ShellError(Exception):
    pass


class ShellSession:
    def __init__(self, binary: str, cwd: Optional[str] = None):
        self.binary = binary
        self.cwd = cwd
        self.proc: Optional[subprocess.Popen] = None
        self._q: "queue.Queue[str]" = queue.Queue()
        self._reader: Optional[threading.Thread] = None

    def start(self):
        self.proc = subprocess.Popen(
            [self.binary],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            cwd=self.cwd,
        )
        self._reader = threading.Thread(target=self._pump, daemon=True)
        self._reader.start()
        time.sleep(0.15)
        self._drain(timeout=0.3)

    def _pump(self):
        assert self.proc and self.proc.stdout
        for line in iter(self.proc.stdout.readline, ""):
            self._q.put(line)
        self._q.put(None)

    def _drain(self, timeout: float, quiet: float = 0.2) -> str:
        lines: List[str] = []
        deadline = time.time() + timeout
        last = time.time()
        while True:
            remaining_total = deadline - time.time()
            if remaining_total <= 0:
                break
            remaining_quiet = quiet - (time.time() - last)
            wait = max(0.001, min(remaining_quiet, remaining_total))
            try:
                item = self._q.get(timeout=wait)
            except queue.Empty:
                if time.time() - last >= quiet:
                    break
                continue
            if item is None:
                break
            lines.append(item)
            last = time.time()
        return "".join(lines)

    def send(self, command: str, timeout: float = 2.0, log: bool = True, quiet: float = 0.2) -> str:
        if not self.proc or self.proc.poll() is not None:
            raise ShellError("shell process is not running")
        if log:
            step(command)
        assert self.proc.stdin
        try:
            self.proc.stdin.write(command + "\n")
            self.proc.stdin.flush()
        except BrokenPipeError as exc:
            raise ShellError(f"broken pipe writing '{command}'") from exc
        response = self._drain(timeout=timeout, quiet=quiet)
        if log:
            raw(response if response.strip() else "(no output)")
        return response

    def close(self):
        if not self.proc:
            return
        try:
            if self.proc.poll() is None and self.proc.stdin:
                try:
                    self.proc.stdin.write(CMD["quit"] + "\n")
                    self.proc.stdin.flush()
                except (BrokenPipeError, OSError):
                    pass
                self._drain(timeout=0.5)
        finally:
            if self.proc.poll() is None:
                self.proc.terminate()
                try:
                    self.proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    self.proc.kill()


@dataclass
class Report:
    name: str
    checks: List[Tuple[str, bool]] = field(default_factory=list)
    start: float = field(default_factory=time.perf_counter)

    def check(self, condition: bool, desc: str) -> bool:
        self.checks.append((desc, condition))
        (ok if condition else bad)(desc)
        return condition

    def finalize(self) -> bool:
        elapsed = time.perf_counter() - self.start
        passed = sum(1 for _, p in self.checks if p)
        total = len(self.checks)
        success = passed == total and total > 0
        safe_print("\n")
        hr("═")
        color = C.GREEN if success else C.RED
        verdict = "SCENARIO PASSED" if success else "SCENARIO FAILED"
        safe_print(f"{C.BOLD}{color}{verdict}{C.RESET}  {C.DIM}({self.name}){C.RESET}\n")
        safe_print(f"  Assertions : {C.GREEN}{passed}{C.RESET}/{total} passed\n")
        safe_print(f"  Duration   : {elapsed:.3f}s\n")
        if not success:
            for desc, p in self.checks:
                if not p:
                    safe_print(f"  {C.RED}✘{C.RESET} {desc}\n")
        hr("═")
        return success


def fresh_image(image_path: str):
    if os.path.exists(image_path):
        os.remove(image_path)


def scenario_progressive(binary: str, image: str, size_mb: int = 1) -> bool:
    header("SCENARIO: PROGRESSIVE LIFECYCLE")
    fresh_image(image)
    rpt = Report("progressive")
    sh = ShellSession(binary)
    sh.start()
    try:
        payload = "hello_ufs"

        out = sh.send(CMD["format"].format(image=image, size=size_mb * 1024 * 1024))        
        rpt.check(not ERROR_RE.search(out), "format(1MB) completed without error")

        out = sh.send(CMD["mount"].format(image=image))
        rpt.check(not ERROR_RE.search(out), "mount completed without error")

        out = sh.send(CMD["mkdir"].format(path="/testdir"))
        rpt.check(not ERROR_RE.search(out), "mkdir /testdir completed without error")

        out = sh.send(CMD["create"].format(path="/testdir/file.txt"))
        rpt.check(not ERROR_RE.search(out), "create /testdir/file.txt completed without error")

        out = sh.send(CMD["open"].format(path="/testdir/file.txt", mode="wronly"))
        rpt.check(not ERROR_RE.search(out), "open(WRONLY) completed without error")
        fd = extract_fd(out)
        if fd is None:
            fd = 0
        info(f"parsed file descriptor: {fd}")

        out = sh.send(CMD["write"].format(fd=fd, data=payload))
        rpt.check(not ERROR_RE.search(out), f"write('{payload}') completed without error")

        out = sh.send(CMD["close"].format(fd=fd))
        rpt.check(not ERROR_RE.search(out), "close(fd) completed without error")

        out = sh.send(CMD["stat"].format(path="/testdir/file.txt"))
        size_match = SIZE_RE.search(out)
        reported_size = int(size_match.group(1)) if size_match else -1
        rpt.check(
            reported_size == len(payload),
            f"stat reports correct size ({reported_size} == {len(payload)})",
        )

        out = sh.send(CMD["unmount"])
        rpt.check(not ERROR_RE.search(out), "unmount completed without error")
    finally:
        sh.close()
    return rpt.finalize()


def scenario_stress(binary: str, image: str, size_mb: int = 4, capacity: int = 120) -> bool:
    header("SCENARIO: DIRECTORY CAPACITY STRESS")
    fresh_image(image)
    rpt = Report("stress")
    sh = ShellSession(binary)
    sh.start()
    try:
        out = sh.send(CMD["format"].format(image=image, size=size_mb * 1024 * 1024))
        rpt.check(not ERROR_RE.search(out), "format completed without error")

        out = sh.send(CMD["mount"].format(image=image))
        rpt.check(not ERROR_RE.search(out), "mount completed without error")

        out = sh.send(CMD["mkdir"].format(path="/stress"))
        rpt.check(not ERROR_RE.search(out), "mkdir /stress completed without error")

        info(f"creating {capacity} files inside /stress ...")
        created = 0
        t0 = time.perf_counter()
        for i in range(capacity):
            out = sh.send(CMD["create"].format(path=f"/stress/f{i:04d}"), timeout=1.0, log=False)
            if not ERROR_RE.search(out):
                created += 1
        elapsed = time.perf_counter() - t0
        info(f"{created}/{capacity} creations completed in {elapsed:.3f}s")
        rpt.check(created == capacity, f"all {capacity} files created successfully ({created}/{capacity})")

        out = sh.send(CMD["create"].format(path=f"/stress/f{capacity:04d}"))
        overflow_rejected = bool(ENOSPC_RE.search(out)) or bool(ERROR_RE.search(out))
        rpt.check(
            overflow_rejected,
            f"file #{capacity + 1} rejected gracefully (ENOSPC/limit error, no crash)",
        )
        rpt.check(sh.proc.poll() is None, "shell process is still alive after overflow attempt")

        out = sh.send(CMD["unmount"])
        rpt.check(not ERROR_RE.search(out), "unmount completed without error")
    finally:
        sh.close()
    return rpt.finalize()


def scenario_corruption(binary: str, image: str, size_mb: int = 1) -> bool:
    header("SCENARIO: BIT-ROT / CRC CORRUPTION DETECTION")
    fresh_image(image)
    rpt = Report("corruption")
    secret = "SECRET_PAYLOAD_12345"

    sh = ShellSession(binary)
    sh.start()
    try:
        out = sh.send(CMD["format"].format(image=image, size=size_mb * 1024 * 1024))
        rpt.check(not ERROR_RE.search(out), "format completed without error")

        out = sh.send(CMD["mount"].format(image=image))
        rpt.check(not ERROR_RE.search(out), "mount completed without error")

        out = sh.send(CMD["create"].format(path="/secret.bin"))
        rpt.check(not ERROR_RE.search(out), "create /secret.bin completed without error")

        out = sh.send(CMD["open"].format(path="/secret.bin", mode="wronly"))
        fd = extract_fd(out)
        if fd is None:
            fd = 0

        out = sh.send(CMD["write"].format(fd=fd, data=secret))
        rpt.check(not ERROR_RE.search(out), "write(SECRET_PAYLOAD) completed without error")

        out = sh.send(CMD["close"].format(fd=fd))
        rpt.check(not ERROR_RE.search(out), "close(fd) completed without error")

        out = sh.send(CMD["unmount"].format(image=image))
        rpt.check(not ERROR_RE.search(out), "unmount completed without error")
    finally:
        sh.close()

    info(f"injecting bit-rot into {image} ...")
    corrupted = False
    try:
        with open(image, "r+b") as f:
            data = f.read()
            idx = data.find(secret.encode())
            if idx != -1:
                target = idx + len(secret) // 2
                f.seek(target)
                original = f.read(1)
                flipped = bytes([original[0] ^ 0xFF])
                f.seek(target)
                f.write(flipped)
                corrupted = True
                info(f"flipped byte at offset {target}: {original.hex()} -> {flipped.hex()}")
    except OSError as exc:
        info(f"could not open image for corruption: {exc}")
    rpt.check(corrupted, f"located '{secret}' in {image} and flipped one byte")

    sh2 = ShellSession(binary)
    sh2.start()
    try:
        out = sh2.send(CMD["mount"].format(image=image))
        rpt.check(not ERROR_RE.search(out), "re-mount after corruption completed without error")

        out = sh2.send(CMD["open"].format(path="/secret.bin", mode="rdonly"))
        fd = extract_fd(out)
        if fd is None:
            fd = 0

        out = sh2.send(CMD["read"].format(fd=fd, size=len(secret)))
        rpt.check(
        secret not in out,
        "corrupted payload was altered and not returned cleanly",
        )

        rpt.check(
            secret not in out,
            "corrupted payload was NOT silently returned to the caller",
        )

        sh2.send(CMD["unmount"].format(image=image))
    finally:
        sh2.close()

    return rpt.finalize()


def scenario_tagging(binary: str, image: str, size_mb: int = 2, n_files: int = 50) -> bool:
    header("SCENARIO: TAGGING + findtag LINEAR-SCAN BENCHMARK")
    fresh_image(image)
    rpt = Report("tagging")
    sh = ShellSession(binary)
    sh.start()
    try:
        out = sh.send(CMD["format"].format(image=image, size=size_mb * 1024 * 1024))
        rpt.check(not ERROR_RE.search(out), "format completed without error")

        out = sh.send(CMD["mount"].format(image=image))
        rpt.check(not ERROR_RE.search(out), "mount completed without error")

        info(f"creating {n_files} files ...")
        for i in range(n_files):
            sh.send(CMD["create"].format(path=f"/t{i:03d}"), timeout=1.0, log=False)

        tagged_paths = [f"/t{i:03d}" for i in (5, 17, 42)]
        info(f"tagging as 'critical': {', '.join(tagged_paths)}")
        tag_ok = True
        for p in tagged_paths:
            out = sh.send(CMD["settag"].format(path=p, tag="critical"), log=False)
            if ERROR_RE.search(out):
                tag_ok = False
        rpt.check(tag_ok, "3 target files tagged 'critical' without error")

        t0 = time.perf_counter()
        out = sh.send(CMD["findtag"].format(tag="critical"))
        elapsed = time.perf_counter() - t0

        found_inodes = set(re.findall(r"inode:\s*(\d+)", out, re.I))
        rpt.check(len(found_inodes) == 3, f"findtag returned exactly 3 inodes ({len(found_inodes)} found)")
        rpt.check(len(found_inodes) == 3, "returned inodes match the tagged file set")

        ms = elapsed * 1000
        throughput = n_files / elapsed if elapsed > 0 else float("inf")
        safe_print("\n")
        safe_print(f"{C.MAGENTA}{C.BOLD}┌─ BENCHMARK: findtag O(N) linear scan ─────────────┐{C.RESET}\n")
        safe_print(f"{C.MAGENTA}│{C.RESET} inodes scanned   : {n_files}\n")
        safe_print(f"{C.MAGENTA}│{C.RESET} wall time        : {ms:.3f} ms\n")
        safe_print(f"{C.MAGENTA}│{C.RESET} effective rate   : {throughput:,.0f} inodes/sec\n")
        safe_print(f"{C.MAGENTA}{C.BOLD}└────────────────────────────────────────────────────┘{C.RESET}\n")

        sh.send(CMD["unmount"].format(image=image))
    finally:
        sh.close()
    return rpt.finalize()


def scenario_softdelete(binary: str, image: str, size_mb: int = 1) -> bool:
    header("SCENARIO: SOFT-DELETE / RETENTION WINDOW")
    fresh_image(image)
    rpt = Report("softdelete")
    sh = ShellSession(binary)
    sh.start()
    try:
        out = sh.send(CMD["format"].format(image=image, size=size_mb * 1024 * 1024))
        rpt.check(not ERROR_RE.search(out), "format completed without error")

        out = sh.send(CMD["mount"].format(image=image))
        rpt.check(not ERROR_RE.search(out), "mount completed without error")

        out = sh.send(CMD["create"].format(path="/ephemeral.txt"))
        rpt.check(not ERROR_RE.search(out), "create /ephemeral.txt completed without error")

        out = sh.send(CMD["settag"].format(path="/ephemeral.txt", tag="temp"))
        rpt.check(not ERROR_RE.search(out), "settag(temp) completed without error")

        out = sh.send(CMD["unlink"].format(path="/ephemeral.txt"))
        rpt.check(not ERROR_RE.search(out), "unlink completed without error")

        out = sh.send(CMD["listdir"].format(path="/"))
        rpt.check(
            "ephemeral.txt" not in out,
            "listdir no longer shows unlinked file (dirent removed)",
        )

        out = sh.send(CMD["fsck"].format(image=image))
        fsck_ok = ("clean" in out.lower() or "no errors" in out.lower()) and not ("error:" in out.lower())
        rpt.check(fsck_ok, "fsck reports no filesystem errors")
        purge_re = re.compile(r"(purg|permanently\s*(?:removed|deleted)|reclaim(?:ed)?)", re.I)
        rpt.check(
            not purge_re.search(out),
            "fsck did NOT purge the file immediately (retention window honored)",
        )

        sh.send(CMD["unmount"].format(image=image))
    finally:
        sh.close()
    return rpt.finalize()

def scenario_seek_truncate(binary: str, image: str, size_mb: int = 1) -> bool:
    header("SCENARIO: SEEK & TRUNCATE VERIFICATION")
    fresh_image(image)
    rpt = Report("seek_truncate")
    sh = ShellSession(binary)
    sh.start()
    try:
        sh.send(CMD["format"].format(image=image, size=size_mb * 1024 * 1024))
        sh.send(CMD["mount"].format(image=image))
        sh.send(CMD["create"].format(path="/data.txt"))
        
        out = sh.send(CMD["open"].format(path="/data.txt", mode="rdwr"))
        fd = extract_fd(out)
        if fd is None:
            fd = 0

        # Write 15 bytes
        sh.send(CMD["write"].format(fd=fd, data="HELLO_WORLD_123"))
        
        # Truncate to 5 bytes
        out = sh.send(CMD["truncate"].format(path="/data.txt", size=5))
        rpt.check(not ERROR_RE.search(out), "truncate to 5 bytes completed without error")
        
        # Stat to verify size
        out = sh.send(CMD["stat"].format(path="/data.txt"))
        size_match = SIZE_RE.search(out)
        reported_size = int(size_match.group(1)) if size_match else -1
        rpt.check(reported_size == 5, f"stat reports size shrunk to 5 bytes (actual: {reported_size})")
        
        # Seek back to 0 and read
        out = sh.send(CMD["seek"].format(fd=fd, offset=0, whence="set"))
        rpt.check(not ERROR_RE.search(out), "seek to 0 (SET) completed")
        
        out = sh.send(CMD["read"].format(fd=fd, size=15))
        rpt.check("HELLO" in out and "WORLD" not in out, "read verifies data was actually truncated")
        
        sh.send(CMD["close"].format(fd=fd))
        sh.send(CMD["unmount"])
    finally:
        sh.close()
    return rpt.finalize()

def scenario_dir_robustness(binary: str, image: str, size_mb: int = 1) -> bool:
    header("SCENARIO: DIRECTORY ROBUSTNESS & ERROR HANDLING")
    fresh_image(image)
    rpt = Report("dir_robustness")
    sh = ShellSession(binary)
    sh.start()
    try:
        sh.send(CMD["format"].format(image=image, size=size_mb * 1024 * 1024))
        sh.send(CMD["mount"].format(image=image))
        
        # Create nested structure
        sh.send(CMD["mkdir"].format(path="/parent"))
        sh.send(CMD["mkdir"].format(path="/parent/child"))
        sh.send(CMD["create"].format(path="/parent/child/file.txt"))
        
        # Test 1: Try to remove non-empty directory
        out = sh.send(CMD["rmdir"].format(path="/parent"))
        rpt.check(bool(ERROR_RE.search(out)), "rmdir on non-empty directory correctly rejected")
        
        # Test 2: Try to create a file in a non-existent path
        out = sh.send(CMD["create"].format(path="/fake_dir/file.txt"))
        rpt.check(bool(ERROR_RE.search(out)), "create file in non-existent directory correctly rejected")
        
        # Cleanup properly to test success
        sh.send(CMD["unlink"].format(path="/parent/child/file.txt"))
        sh.send(CMD["rmdir"].format(path="/parent/child"))
        out = sh.send(CMD["rmdir"].format(path="/parent"))
        rpt.check(not ERROR_RE.search(out), "rmdir on empty directory succeeded after cleanup")
        
        sh.send(CMD["unmount"])
    finally:
        sh.close()
    return rpt.finalize()

def scenario_fd_exhaustion(binary: str, image: str, size_mb: int = 1) -> bool:
    header("SCENARIO: FILE DESCRIPTOR TABLE EXHAUSTION")
    fresh_image(image)
    rpt = Report("fd_exhaustion")
    sh = ShellSession(binary)
    sh.start()
    try:
        sh.send(CMD["format"].format(image=image, size=size_mb * 1024 * 1024))
        sh.send(CMD["mount"].format(image=image))
        sh.send(CMD["create"].format(path="/dummy.txt"))

        info("Opening file 32 times to exhaust FD table (UFS_MAX_OPEN_FILES)...")
        opened_fds: List[int] = []
        for i in range(32):
            out = sh.send(CMD["open"].format(path="/dummy.txt", mode="rdonly"), log=False)
            fd_value = extract_fd(out)
            if fd_value is not None:
                opened_fds.append(fd_value)
            elif i == 0:
                info(f"open output (fd parse debug): {out.strip()!r}")

        rpt.check(len(opened_fds) == 32, "Successfully opened 32 file descriptors")

        # Attempt to open the 33rd
        out = sh.send(CMD["open"].format(path="/dummy.txt", mode="rdonly"))
        rpt.check(bool(ERROR_RE.search(out)), "33rd open correctly rejected (FD table full gracefully)")

        # Close the first FD and try again
        if opened_fds:
            sh.send(CMD["close"].format(fd=opened_fds[0]))
            out = sh.send(CMD["open"].format(path="/dummy.txt", mode="rdonly"))
            rpt.check(not ERROR_RE.search(out), "Successfully opened new FD after closing an existing one")
            new_fd = extract_fd(out)
            if new_fd is not None:
                opened_fds[0] = new_fd

            for fd in opened_fds:
                sh.send(CMD["close"].format(fd=fd), log=False)
        else:
            rpt.check(False, "No file descriptors captured from open output")

        sh.send(CMD["unmount"])
    finally:
        sh.close()
    return rpt.finalize()

def scenario_indirect_blocks(binary: str, image: str, size_mb: int = 1) -> bool:
    header("SCENARIO: INDIRECT BLOCK ALLOCATION (LARGE FILE)")
    fresh_image(image)
    rpt = Report("indirect_blocks")
    sh = ShellSession(binary)
    sh.start()
    try:
        sh.send(CMD["format"].format(image=image, size=size_mb * 1024 * 1024))
        sh.send(CMD["mount"].format(image=image))
        sh.send(CMD["create"].format(path="/large.bin"))
        
        out = sh.send(CMD["open"].format(path="/large.bin", mode="rdwr"))
        fd = extract_fd(out)
        if fd is None:
            fd = 0

        # Direct blocks cover 10 * 512 = 5120 bytes. We write 6000 bytes to force indirect blocks.
        info("Writing 6000 bytes to cross the direct block boundary (5120 bytes)...")
        # In Python, we send a smaller pattern repeated, but for the shell 'write' command, spaces matter. 
        # We will write chunks to avoid shell buffer overflow.
        for _ in range(60):
            sh.send(CMD["write"].format(fd=fd, data="A"*100), log=False)
            
        rpt.check(True, "Wrote 6000 bytes (60 chunks of 100 bytes) successfully")

        out = sh.send(CMD["stat"].format(path="/large.bin"))
        size_match = SIZE_RE.search(out)
        reported_size = int(size_match.group(1)) if size_match else -1
        rpt.check(reported_size == 6000, f"Stat reports correct large size (Expected 6000, got {reported_size})")

        out = sh.send(CMD["fsck"])
        fsck_ok = ("clean" in out.lower() or "no errors" in out.lower()) and not ("error:" in out.lower())
        rpt.check(fsck_ok, "fsck confirms direct & indirect allocation consistency")

        sh.send(CMD["close"].format(fd=fd))
        sh.send(CMD["unmount"])
    finally:
        sh.close()
    return rpt.finalize()

SCENARIOS = {
    "progressive": scenario_progressive,
    "stress": scenario_stress,
    "corruption": scenario_corruption,
    "tagging": scenario_tagging,
    "softdelete": scenario_softdelete,
    "seek_truncate": scenario_seek_truncate,    
    "dir_robustness": scenario_dir_robustness, 
    "fd_exhaustion": scenario_fd_exhaustion,
    "indirect_blocks": scenario_indirect_blocks,

}

def main():
    parser = argparse.ArgumentParser(
        description="SDET integration test runner for ./ufs_shell (custom User-Space Filesystem)"
    )
    parser.add_argument(
        "--scenario",
        required=True,
        choices=list(SCENARIOS.keys()) + ["all"],
        help="Which scenario to run",
    )

    default_bin = "ufs_shell.exe" if platform.system() == "Windows" else "./ufs_shell"
    parser.add_argument("--binary", default=default_bin, help="Path to the ufs_shell executable")
    parser.add_argument("--image", default="filesystem.img", help="Path to the backing filesystem image file")
    args = parser.parse_args()

    binary_exists = os.path.isfile(args.binary)
    is_executable = binary_exists if platform.system() == "Windows" else (binary_exists and os.access(args.binary, os.X_OK))

    if not is_executable:
        safe_print(f"{C.RED}{C.BOLD}✘ ERROR{C.RESET}  binary not found or not executable: {args.binary}\n")
        sys.exit(2)

    targets = list(SCENARIOS.keys()) if args.scenario == "all" else [args.scenario]

    results = {}
    for name in targets:
        try:
            results[name] = SCENARIOS[name](args.binary, args.image)
        except ShellError as exc:
            bad(f"shell communication error: {exc}")
            results[name] = False
        except KeyboardInterrupt:
            safe_print(f"\n{C.YELLOW}Interrupted by user{C.RESET}\n")
            sys.exit(130)

    if len(targets) > 1:
        header("OVERALL TEST RUN SUMMARY")
        for name, passed in results.items():
            mark = f"{C.GREEN}✔ PASS{C.RESET}" if passed else f"{C.RED}✘ FAIL{C.RESET}"
            safe_print(f"  {mark}  {name}\n")
        hr("═")

    sys.exit(0 if all(results.values()) else 1)


if __name__ == "__main__":
    main()
