#!/usr/bin/env python3
"""Exercise the bare-Escape key path using a real pseudo-terminal."""
import os
import pty
import select
import signal
import sqlite3
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROGRAM = os.path.join(ROOT, "recode_all_music_in_folder")


def main():
    with tempfile.TemporaryDirectory(prefix="ramif-escape-tty-") as temp:
        data_home = os.path.join(temp, "data")
        music = os.path.join(temp, "music")
        output = os.path.join(temp, "out")
        os.makedirs(music)
        os.makedirs(output)
        with open(os.path.join(music, "TTY Song.mp3"), "wb"):
            pass
        pid, master = pty.fork()
        if pid == 0:
            os.environ["XDG_DATA_HOME"] = data_home
            os.execv(PROGRAM, [PROGRAM, "--target", music, "--output", output,
                               "--title", "--artist", "--no-clipboard"])

        output_bytes = bytearray()

        def pump(timeout=0.25):
            ready, _, _ = select.select([master], [], [], timeout)
            if ready:
                try:
                    chunk = os.read(master, 8192)
                except OSError:
                    chunk = b""
                if chunk:
                    output_bytes.extend(chunk)
                    return True
            return False

        def expect_count(token, count, timeout=10):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                if output_bytes.count(token) >= count:
                    return
                pump(0.1)
            raise AssertionError(f"timed out waiting for {token!r} x{count}; output={bytes(output_bytes)!r}")

        try:
            title_prompt = b"Song title for "
            expect_count(title_prompt, 1)
            os.write(master, b"\x1b")
            expect_count(title_prompt, 2)
            os.write(master, b"\x15TTY Title\r")
            expect_count(b"Artist for ", 1)
            os.write(master, b"TTY Artist\r")

            deadline = time.monotonic() + 15
            status = None
            while time.monotonic() < deadline:
                done, status = os.waitpid(pid, os.WNOHANG)
                if done == pid:
                    break
                pump(0.1)
            else:
                os.kill(pid, signal.SIGKILL)
                os.waitpid(pid, 0)
                raise AssertionError("RAMIF did not exit after receiving interactive metadata")
            if not os.WIFEXITED(status) or os.WEXITSTATUS(status) != 0:
                raise AssertionError(f"RAMIF exited unexpectedly: {status}; output={bytes(output_bytes)!r}")

            db_path = os.path.join(data_home, "recode_all_music_in_folder", "ramif_database.db")
            with sqlite3.connect(db_path) as conn:
                row = conn.execute("select title,artist from files").fetchone()
            assert row == ("TTY Title", "TTY Artist"), row
        finally:
            try:
                os.close(master)
            except OSError:
                pass
        print("RAMIF pseudo-terminal Escape test passed.")


if __name__ == "__main__":
    main()
