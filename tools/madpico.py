#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Manage the pages of a madpico node over USB serial.

Only the pages directory is touched; the node identity and everything else
in the Pico's flash stay untouched. Requires pyserial (pip install pyserial).

Examples:
  madpico.py ls
  madpico.py put mypage.mu                 # uploads as /page/mypage.mu
  madpico.py put local.mu sub/remote.mu    # uploads as /page/sub/remote.mu
  madpico.py get index.mu                  # downloads to ./index.mu
  madpico.py rm old.mu
  madpico.py sync ./pages --delete         # mirror a local directory
  madpico.py df
"""
import argparse
import glob
import os
import sys
import time
import zlib

try:
    import serial
except ImportError:
    sys.exit("pyserial is required: pip install pyserial")


class ProtocolError(Exception):
    pass


class Node:
    def __init__(self, port, verbose=False):
        self.ser = serial.Serial(port, 115200, timeout=0.2)
        self.ser.dtr = True
        self.verbose = verbose
        self.buf = b""
        time.sleep(0.2)
        self.ser.reset_input_buffer()
        # Terminate any partial command line on the node
        self.ser.write(b"\n")

    def close(self):
        self.ser.close()

    def _fill(self, deadline):
        data = self.ser.read(4096)
        if data:
            self.buf += data
        elif time.time() > deadline:
            raise ProtocolError("timeout waiting for the node")

    def read_line(self, timeout=10.0):
        """Returns the next protocol line (starting with '!'), skipping log output."""
        deadline = time.time() + timeout
        while True:
            nl = self.buf.find(b"\n")
            if nl < 0:
                self._fill(deadline)
                continue
            line = self.buf[:nl].rstrip(b"\r").decode("utf-8", "replace")
            self.buf = self.buf[nl + 1:]
            if line.startswith("!"):
                return line
            if self.verbose and line:
                print(f"  [node] {line}", file=sys.stderr)

    def read_exact(self, n, timeout=30.0):
        deadline = time.time() + timeout
        while len(self.buf) < n:
            self._fill(deadline)
        data, self.buf = self.buf[:n], self.buf[n:]
        return data

    def command(self, line):
        self.ser.write(("!" + line + "\n").encode())
        self.ser.flush()

    @staticmethod
    def check_ok(line):
        if line.startswith("!err"):
            raise ProtocolError(line[5:] or "error")
        if not line.startswith("!ok"):
            raise ProtocolError(f"unexpected response: {line}")
        return line[4:]

    # --- commands --------------------------------------------------------
    def ping(self):
        self.command("ping")
        return self.check_ok(self.read_line())

    def ls(self):
        self.command("ls")
        files = {}
        while True:
            line = self.read_line(timeout=30)
            if line.startswith("!file "):
                _, size, crc, path = line.split(" ", 3)
                files[path] = (int(size), int(crc, 16))
                continue
            self.check_ok(line)
            return files

    def df(self):
        self.command("df")
        used, total = self.check_ok(self.read_line()).split()
        return int(used), int(total)

    def put(self, remote, data):
        crc = zlib.crc32(data) & 0xFFFFFFFF
        self.command(f"put {remote} {len(data)} {crc:08x}")
        line = self.read_line()
        if line != "!ready":
            self.check_ok(line)
            raise ProtocolError(f"unexpected response: {line}")
        # Send in chunks; USB CDC applies flow control
        for i in range(0, len(data), 1024):
            self.ser.write(data[i:i + 1024])
        self.ser.flush()
        self.check_ok(self.read_line(timeout=30))

    def get(self, remote):
        self.command(f"get {remote}")
        line = self.read_line()
        if not line.startswith("!data "):
            self.check_ok(line)
            raise ProtocolError(f"unexpected response: {line}")
        _, size, crc = line.split()
        data = self.read_exact(int(size))
        self.check_ok(self.read_line())
        if zlib.crc32(data) & 0xFFFFFFFF != int(crc, 16):
            raise ProtocolError("crc mismatch")
        return data

    def rm(self, remote):
        self.command(f"rm {remote}")
        self.check_ok(self.read_line())


def local_files(directory):
    files = {}
    for path in sorted(glob.glob(os.path.join(directory, "**", "*"), recursive=True)):
        if not os.path.isfile(path):
            continue
        rel = os.path.relpath(path, directory).replace(os.sep, "/")
        if any(part.startswith(".") for part in rel.split("/")):
            continue
        files[rel] = path
    return files


def main():
    ap = argparse.ArgumentParser(description="Manage pages on a madpico node over USB serial.")
    ap.add_argument("--port", default=os.environ.get("MADPICO_PORT", "/dev/ttyACM0"),
                    help="serial port (default: $MADPICO_PORT or /dev/ttyACM0)")
    ap.add_argument("-v", "--verbose", action="store_true", help="show node log output")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("ls", help="list pages")
    sub.add_parser("df", help="show flash usage")
    sub.add_parser("ping", help="check the connection")
    p = sub.add_parser("put", help="upload a page")
    p.add_argument("local")
    p.add_argument("remote", nargs="?")
    p = sub.add_parser("get", help="download a page")
    p.add_argument("remote")
    p.add_argument("local", nargs="?")
    p = sub.add_parser("rm", help="delete a page")
    p.add_argument("remote")
    p = sub.add_parser("sync", help="upload changed pages from a local directory")
    p.add_argument("directory")
    p.add_argument("--delete", action="store_true", help="delete pages that don't exist locally")
    p.add_argument("-n", "--dry-run", action="store_true", help="only show what would happen")
    args = ap.parse_args()

    node = Node(args.port, verbose=args.verbose)
    try:
        if args.cmd == "ping":
            print(node.ping())
        elif args.cmd == "ls":
            files = node.ls()
            for path, (size, crc) in sorted(files.items()):
                print(f"{size:8d}  {crc:08x}  /page/{path}")
            print(f"{len(files)} page(s)")
        elif args.cmd == "df":
            used, total = node.df()
            print(f"{used} / {total} bytes used ({100 * used / total:.1f}%)")
        elif args.cmd == "put":
            remote = args.remote or os.path.basename(args.local)
            with open(args.local, "rb") as f:
                data = f.read()
            node.put(remote, data)
            print(f"uploaded {args.local} -> /page/{remote} ({len(data)} bytes)")
        elif args.cmd == "get":
            data = node.get(args.remote)
            local = args.local or os.path.basename(args.remote)
            with open(local, "wb") as f:
                f.write(data)
            print(f"downloaded /page/{args.remote} -> {local} ({len(data)} bytes)")
        elif args.cmd == "rm":
            node.rm(args.remote)
            print(f"deleted /page/{args.remote}")
        elif args.cmd == "sync":
            remote = node.ls()
            local = local_files(args.directory)
            uploaded = unchanged = deleted = 0
            for rel, path in local.items():
                with open(path, "rb") as f:
                    data = f.read()
                crc = zlib.crc32(data) & 0xFFFFFFFF
                if rel in remote and remote[rel] == (len(data), crc):
                    unchanged += 1
                    continue
                print(f"{'would upload' if args.dry_run else 'upload'} /page/{rel} ({len(data)} bytes)")
                if not args.dry_run:
                    node.put(rel, data)
                uploaded += 1
            if args.delete:
                for rel in sorted(set(remote) - set(local)):
                    print(f"{'would delete' if args.dry_run else 'delete'} /page/{rel}")
                    if not args.dry_run:
                        node.rm(rel)
                    deleted += 1
            print(f"{uploaded} uploaded, {unchanged} unchanged, {deleted} deleted")
    except ProtocolError as e:
        sys.exit(f"error: {e}")
    finally:
        node.close()


if __name__ == "__main__":
    main()
