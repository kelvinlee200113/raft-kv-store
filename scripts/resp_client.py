#!/usr/bin/env python3
"""Small, dependency-free RESP2 client used by cluster validation."""

from __future__ import annotations

import argparse
import json
import socket
import sys
from typing import BinaryIO, List, Optional, Sequence, Union


MAX_LINE_BYTES = 1024 * 1024
MAX_BULK_BYTES = 64 * 1024 * 1024

RespValue = Union[str, int, bytes, None, List["RespValue"]]


class RespError(Exception):
    """An error reply returned by the server."""


class ProtocolError(Exception):
    """A malformed or incomplete RESP reply."""


def encode_command(parts: Sequence[str]) -> bytes:
    encoded = [part.encode("utf-8") for part in parts]
    frame = bytearray(f"*{len(encoded)}\r\n".encode("ascii"))
    for part in encoded:
        frame.extend(f"${len(part)}\r\n".encode("ascii"))
        frame.extend(part)
        frame.extend(b"\r\n")
    return bytes(frame)


def read_exact(stream: BinaryIO, size: int) -> bytes:
    chunks = bytearray()
    while len(chunks) < size:
        chunk = stream.read(size - len(chunks))
        if not chunk:
            raise ProtocolError("connection closed in the middle of a reply")
        chunks.extend(chunk)
    return bytes(chunks)


def read_line(stream: BinaryIO) -> bytes:
    line = stream.readline(MAX_LINE_BYTES + 1)
    if not line:
        raise ProtocolError("connection closed before a reply arrived")
    if len(line) > MAX_LINE_BYTES:
        raise ProtocolError("reply line exceeds the client limit")
    if not line.endswith(b"\r\n"):
        raise ProtocolError("reply line is missing CRLF")
    return line[:-2]


def parse_integer(raw: bytes, field: str) -> int:
    try:
        return int(raw)
    except ValueError as exc:
        raise ProtocolError(f"invalid {field}: {raw!r}") from exc


def read_response(stream: BinaryIO) -> RespValue:
    prefix = read_exact(stream, 1)

    if prefix == b"+":
        return read_line(stream).decode("utf-8", errors="replace")
    if prefix == b"-":
        raise RespError(read_line(stream).decode("utf-8", errors="replace"))
    if prefix == b":":
        return parse_integer(read_line(stream), "integer reply")
    if prefix == b"$":
        size = parse_integer(read_line(stream), "bulk length")
        if size == -1:
            return None
        if size < 0 or size > MAX_BULK_BYTES:
            raise ProtocolError(f"bulk length is outside the client limit: {size}")
        payload = read_exact(stream, size)
        if read_exact(stream, 2) != b"\r\n":
            raise ProtocolError("bulk reply is missing its trailing CRLF")
        return payload
    if prefix == b"*":
        count = parse_integer(read_line(stream), "array length")
        if count == -1:
            return None
        if count < 0 or count > 1024:
            raise ProtocolError(f"array length is outside the client limit: {count}")
        return [read_response(stream) for _ in range(count)]

    raise ProtocolError(f"unknown RESP type byte: {prefix!r}")


def printable(value: RespValue) -> str:
    if value is None:
        return "(nil)"
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="backslashreplace")
    if isinstance(value, list):
        normalized = [printable(item) for item in value]
        return json.dumps(normalized)
    return str(value)


def port_number(raw: str) -> int:
    value = int(raw)
    if value < 1 or value > 65535:
        raise argparse.ArgumentTypeError("port must be between 1 and 65535")
    return value


def positive_timeout(raw: str) -> float:
    value = float(raw)
    if value <= 0:
        raise argparse.ArgumentTypeError("timeout must be greater than zero")
    return value


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Send one RESP2 command to Raft KV")
    parser.add_argument("--host", default="127.0.0.1", help="numeric server address")
    parser.add_argument("--port", required=True, type=port_number)
    parser.add_argument("--timeout", default=2.0, type=positive_timeout, help="seconds")
    parser.add_argument("command", help="PING, GET, SET, or DEL")
    parser.add_argument("arguments", nargs="*", help="command arguments")
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    request = encode_command([args.command, *args.arguments])

    try:
        with socket.create_connection((args.host, args.port), timeout=args.timeout) as sock:
            sock.settimeout(args.timeout)
            sock.sendall(request)
            with sock.makefile("rb") as stream:
                response = read_response(stream)
    except RespError as exc:
        print(str(exc), file=sys.stderr)
        return 2
    except (OSError, ProtocolError) as exc:
        print(f"CLIENT_ERROR: {exc}", file=sys.stderr)
        return 3

    print(printable(response))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
