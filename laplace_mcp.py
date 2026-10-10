#!/usr/bin/env python3
"""Laplace-MCP. Admit a file by its recipe and return the entity id. Fetch that id for the record."""

import ctypes
import json
import os
import sys

# liblpm.so from the build: LAPLACE_MCP_LIB names it; else the release build under LAPLACE_BUILD.
_lib = ctypes.CDLL(os.environ.get("LAPLACE_MCP_LIB") or os.path.join(os.environ.get("LAPLACE_BUILD", "/repos/build"), "Laplace-MCP", "icx-release", "liblpm.so"))


class Rec(ctypes.Structure):
    _fields_ = [
        ("id", ctypes.c_uint8 * 16),
        ("m", ctypes.c_int64 * 4),
        ("hilbert", ctypes.c_uint64),
        ("seen", ctypes.c_uint32),
        ("tier", ctypes.c_uint8),
        ("found", ctypes.c_uint8),
    ]


class Hit(ctypes.Structure):
    _fields_ = [
        ("id", ctypes.c_uint8 * 16),
        ("m", ctypes.c_int64 * 4),
        ("hilbert", ctypes.c_uint64),
        ("line0", ctypes.c_uint32),
        ("line1", ctypes.c_uint32),
        ("shared", ctypes.c_uint32),
        ("nbytes", ctypes.c_uint32),
        ("path", ctypes.c_char * 512),
    ]


_lib.lpm_new.restype = ctypes.c_void_p
_lib.lpm_new.argtypes = [ctypes.c_char_p]
_lib.lpm_free.argtypes = [ctypes.c_void_p]
_lib.lpm_index_path.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
_lib.lpm_index_path.restype = ctypes.c_int
_lib.lpm_last.argtypes = [ctypes.c_void_p, ctypes.POINTER(Hit)]
_lib.lpm_last.restype = ctypes.c_int
_lib.lpm_fetch.argtypes = [
    ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint8), ctypes.POINTER(Rec),
    ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32),
]
_lib.lpm_fetch.restype = ctypes.c_int

IX = _lib.lpm_new(None)
if not IX:
    sys.stderr.write("tier 0 did not map\n")
    sys.exit(1)


def hx(b):
    return bytes(b).hex()


def admit(path):
    if _lib.lpm_index_path(IX, path.encode()) != 0:
        raise RuntimeError(path)
    hit = Hit()
    if _lib.lpm_last(IX, ctypes.byref(hit)) != 0:
        raise RuntimeError("no record")
    rec = Rec()
    nk = ctypes.c_uint32()
    _lib.lpm_fetch(IX, hit.id, ctypes.byref(rec), None, 0, ctypes.byref(nk))
    return {"id": hx(hit.id), "tier": rec.tier, "children": nk.value,
            "m": [rec.m[i] for i in range(4)], "hilbert": rec.hilbert, "path": path}


def fetch(idhex):
    raw = bytes.fromhex(idhex)
    if len(raw) != 16:
        raise RuntimeError("id is 32 hex characters")
    buf = (ctypes.c_uint8 * 16).from_buffer_copy(raw)
    rec = Rec()
    kids = (ctypes.c_uint8 * (16 * 8))()
    nk = ctypes.c_uint32()
    if _lib.lpm_fetch(IX, buf, ctypes.byref(rec), kids, 8, ctypes.byref(nk)) != 0:
        raise RuntimeError("no such id")
    shown = min(nk.value, 8)
    return {"id": hx(rec.id), "tier": rec.tier, "children": nk.value,
            "m": [rec.m[i] for i in range(4)], "hilbert": rec.hilbert,
            "child": [bytes(kids[i * 16:(i + 1) * 16]).hex() for i in range(shown)]}


TOOLS = [
    {"name": "admit", "description": "Admit a file by its recipe. Returns the entity id. Fetch that id for the record.",
     "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]}},
    {"name": "fetch", "description": "The record for an entity id: physicality and constituent ids.",
     "inputSchema": {"type": "object", "properties": {"id": {"type": "string"}}, "required": ["id"]}},
]


def reply(obj):
    body = json.dumps(obj).encode()
    sys.stdout.buffer.write(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
    sys.stdout.buffer.flush()


def handle(msg):
    method, i = msg.get("method"), msg.get("id")
    if method == "notifications/initialized" or method and method.startswith("notifications/"):
        return
    if method == "initialize":
        ver = (msg.get("params") or {}).get("protocolVersion", "2024-11-05")
        reply({"jsonrpc": "2.0", "id": i, "result": {
            "protocolVersion": ver, "capabilities": {"tools": {"listChanged": False}},
            "serverInfo": {"name": "laplace-mcp", "version": "0.1.0"}}})
        return
    if method == "tools/list":
        reply({"jsonrpc": "2.0", "id": i, "result": {"tools": TOOLS}})
        return
    if method == "tools/call":
        p = msg.get("params") or {}
        name, args = p.get("name"), p.get("arguments") or {}
        try:
            if name == "admit":
                out = admit(args["path"])
            elif name == "fetch":
                out = fetch(args["id"])
            else:
                raise RuntimeError(name)
            reply({"jsonrpc": "2.0", "id": i, "result": {"content": [{"type": "text", "text": json.dumps(out)}], "isError": False}})
        except Exception as e:
            reply({"jsonrpc": "2.0", "id": i, "result": {"content": [{"type": "text", "text": str(e)}], "isError": True}})
        return
    if i is not None:
        reply({"jsonrpc": "2.0", "id": i, "error": {"code": -32601, "message": method or ""}})


def serve():
    buf = sys.stdin.buffer
    while True:
        headers = {}
        while True:
            line = buf.readline()
            if not line:
                return
            if line in (b"\r\n", b"\n"):
                break
            k, _, v = line.decode().partition(":")
            headers[k.strip().lower()] = v.strip()
        n = int(headers.get("content-length", "0"))
        if not n:
            continue
        handle(json.loads(buf.read(n)))


if __name__ == "__main__":
    serve()
