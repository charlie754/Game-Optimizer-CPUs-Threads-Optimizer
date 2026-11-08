#!/usr/bin/env python3
import argparse
import gzip
import json
import os
from pathlib import Path
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote, urlsplit


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, format_string, *args):
        return

    def _log_request(self):
        entry = {
            "method": self.command,
            "path": self.path,
            "headers": dict(self.headers.items()),
        }
        line = json.dumps(entry, separators=(",", ":"), ensure_ascii=False) + "\n"
        with self.server.log_lock:
            with self.server.log_path.open("a", encoding="utf-8") as log_file:
                log_file.write(line)
                log_file.flush()

    def do_GET(self):
        self._serve(False)

    def do_HEAD(self):
        self._serve(True)

    def _safe_call(self, function, *args):
        try:
            function(*args)
            return True
        except (ConnectionError, OSError):
            return False

    def _send_common_headers(self, length, content_type=None, encoding=None):
        headers = [("Content-Length", str(length))]
        if content_type is not None:
            headers.append(("Content-Type", content_type))
        if encoding is not None:
            headers.append(("Content-Encoding", encoding))
        headers.append(("Connection", "close"))
        for name, value in headers:
            if not self._safe_call(self.send_header, name, value):
                return False
        return self._safe_call(self.end_headers)

    def _safe_file(self, path_text):
        decoded = unquote(path_text)
        relative = decoded.lstrip("/")
        root = self.server.root
        target = (root / relative).resolve()
        try:
            target.relative_to(root)
        except ValueError:
            return None
        if not target.is_file():
            return None
        return target

    def _serve(self, head):
        self._log_request()
        parsed = urlsplit(self.path)
        pieces = parsed.path.lstrip("/").split("/", 1)
        mode = pieces[0] if pieces and pieces[0] else ""
        rest = pieces[1] if len(pieces) == 2 else ""

        if mode == "redirect" or mode == "redirect-cross":
            if mode == "redirect":
                location = "/ok/" + rest
            else:
                location = f"http://localhost:{self.server.server_port}/ok/{rest}"
            if not self._safe_call(self.send_response, 302):
                return
            if not self._safe_call(self.send_header, "Location", location):
                return
            self._send_common_headers(0)
            return

        target = self._safe_file(rest)
        if mode == "status500":
            if not self._safe_call(self.send_response, 500):
                return
            self._send_common_headers(0)
            return
        if mode not in {"ok", "drip", "gzip", "bigheaders", "slowheaders"} or target is None:
            self.send_response(404)
            self._send_common_headers(0)
            return

        if mode == "slowheaders":
            time.sleep(15)

        data = target.read_bytes()
        encoding = None
        if mode == "gzip":
            data = gzip.compress(b"\0" * (2 * 1024 * 1024), mtime=0)
            encoding = "gzip"

        if not self._safe_call(self.send_response, 200):
            return
        if mode == "bigheaders":
            for index in range(21):
                if not self._safe_call(self.send_header, f"X-Extra-{index:02d}", "x" * 1000):
                    return
        if not self._send_common_headers(len(data), encoding=encoding):
            return
        if head:
            return

        if mode == "drip":
            for byte in data:
                if not self._safe_call(self.wfile.write, bytes((byte,))):
                    return
                if not self._safe_call(self.wfile.flush):
                    return
                time.sleep(3)
            return

        if not self._safe_call(self.wfile.write, data):
            return
        if not self._safe_call(self.wfile.flush):
            return


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    parser.add_argument("--port", required=True, type=int)
    parser.add_argument("--log", required=True)
    args = parser.parse_args()

    root = Path(args.root).resolve()
    log_path = Path(args.log).resolve()
    log_path.parent.mkdir(parents=True, exist_ok=True)
    server = Server(("127.0.0.1", args.port), Handler)
    server.root = root
    server.log_path = log_path
    server.log_lock = threading.Lock()
    print(server.server_port, flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
