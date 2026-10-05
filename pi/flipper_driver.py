"""flipper - command driver for the Flipper Zero, over the GPIO-header
expansion RPC link through flippi.

v2 adds storage writes (mkdir/put/rm) for installing files.
Still no RF transmit (IR/Sub-GHz/BadUSB) anywhere.
Invoke through the wrapper:  ~/flipper-rpc/flipper <command> [args]
"""

import argparse
import os
import sys

BASE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(BASE, "lib", "src"))
sys.path.insert(0, BASE)

from expansion_transport import ExpansionTransport, ExpansionError
from pyflipper.lib import rpc_codec
from pyflipper.lib.rpc_codec import RpcError
from pyflipper.lib.rpc_session import RpcSession
from pyflipper._proto import flipper_pb2, storage_pb2

# File data is sent to the Flipper in slices of one write request each. All
# slices share one command ID; every slice but the last carries has_next,
# which is how the firmware knows to keep the file open between them.
WRITE_CHUNK = 512


def _storage_write(session, remote_path, data):
    codec = session._codec
    transport = session._transport
    command_id = codec.current_command_id
    codec._command_id += 1

    slices = [data[i:i + WRITE_CHUNK]
              for i in range(0, len(data), WRITE_CHUNK)] or [b""]
    for index, piece in enumerate(slices):
        main = flipper_pb2.Main()
        main.command_id = command_id
        main.has_next = index < len(slices) - 1
        main.storage_write_request.path = remote_path
        if piece:
            main.storage_write_request.file.data = piece
        body = main.SerializeToString()
        transport.write(rpc_codec.encode_varint(len(body)) + body)

    reply = transport.read_message(
        lambda m: rpc_codec.is_response_to(m, command_id),
        what=f"writing {remote_path}")
    session._check(reply, f"writing {remote_path}")


def cmd_ping(session, args):
    reply = session.request("system_ping_request",
                            data=args.text.encode(), what="ping")
    print(reply.system_ping_response.data.decode(errors="replace"))


def _stream_kv(session, request_field, response_field, what):
    values = {}
    for reply in session.request_stream(request_field, what=what):
        part = getattr(reply, response_field)
        values[part.key] = part.value
    return values


def cmd_info(session, _args):
    info = _stream_kv(session, "system_device_info_request",
                      "system_device_info_response", "device info")
    for key in sorted(info):
        print(f"{key} = {info[key]}")


def cmd_power(session, _args):
    power = _stream_kv(session, "system_power_info_request",
                       "system_power_info_response", "power info")
    for key in sorted(power):
        print(f"{key} = {power[key]}")


def cmd_ls(session, args):
    seen = False
    for reply in session.request_stream("storage_list_request", path=args.path,
                                        what=f"listing {args.path}"):
        for entry in reply.storage_list_response.file:
            seen = True
            if entry.type == storage_pb2.File.DIR:
                print(f"[dir]  {entry.name}")
            else:
                print(f"{entry.name}  ({entry.size} bytes)")
    if not seen:
        print("(empty)")


def cmd_stat(session, args):
    reply = session.request("storage_stat_request", path=args.path,
                            what=f"stat of {args.path}")
    print(f"{args.path}: {reply.storage_stat_response.file.size} bytes")


def cmd_storage_info(session, args):
    reply = session.request("storage_info_request", path=args.path,
                            what=f"storage info for {args.path}")
    info = reply.storage_info_response
    gib = 1024 ** 3
    print(f"{args.path}: {info.free_space / gib:.2f} GiB free of "
          f"{info.total_space / gib:.2f} GiB")


def cmd_read(session, args):
    contents = bytearray()
    for reply in session.request_stream("storage_read_request", path=args.path,
                                        what=f"reading {args.path}"):
        contents.extend(reply.storage_read_response.file.data)
    out_dir = os.path.join(BASE, "downloads")
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, os.path.basename(args.path.rstrip("/")))
    with open(out_path, "wb") as handle:
        handle.write(contents)
    print(f"saved {len(contents)} bytes -> {out_path}")
    try:
        text = bytes(contents).decode("utf-8")
    except UnicodeDecodeError:
        return
    if text.strip():
        print("---- file contents ----")
        print(text[:4000])


def cmd_md5(session, args):
    reply = session.request("storage_md5sum_request", path=args.path,
                            what=f"md5 of {args.path}")
    print(reply.storage_md5sum_response.md5sum)


def cmd_mkdir(session, args):
    session.request("storage_mkdir_request", path=args.path,
                    what=f"mkdir {args.path}")
    print(f"created directory {args.path}")


def cmd_put(session, args):
    with open(args.local, "rb") as handle:
        data = handle.read()
    _storage_write(session, args.remote, data)
    print(f"wrote {len(data)} bytes -> {args.remote}")


def cmd_rm(session, args):
    session.request("storage_delete_request", path=args.path, recursive=False,
                    what=f"deleting {args.path}")
    print(f"deleted {args.path}")


def build_parser():
    parser = argparse.ArgumentParser(prog="flipper")
    sub = parser.add_subparsers(dest="command", required=True)

    ping = sub.add_parser("ping", help="RPC round-trip check")
    ping.add_argument("text", nargs="?", default="hello flipper")
    ping.set_defaults(func=cmd_ping)

    info = sub.add_parser("info", help="device info")
    info.set_defaults(func=cmd_info)

    power = sub.add_parser("power", help="battery / charger info")
    power.set_defaults(func=cmd_power)

    ls = sub.add_parser("ls", help="list a directory on the Flipper")
    ls.add_argument("path", nargs="?", default="/ext")
    ls.set_defaults(func=cmd_ls)

    stat = sub.add_parser("stat", help="size of one file")
    stat.add_argument("path")
    stat.set_defaults(func=cmd_stat)

    sinfo = sub.add_parser("storage-info", help="total/free space")
    sinfo.add_argument("path", nargs="?", default="/ext")
    sinfo.set_defaults(func=cmd_storage_info)

    read = sub.add_parser("read", help="download a file to the Pi")
    read.add_argument("path")
    read.set_defaults(func=cmd_read)

    md5 = sub.add_parser("md5", help="md5 of a file on the Flipper")
    md5.add_argument("path")
    md5.set_defaults(func=cmd_md5)

    mkdir = sub.add_parser("mkdir", help="create a directory on the Flipper")
    mkdir.add_argument("path")
    mkdir.set_defaults(func=cmd_mkdir)

    put = sub.add_parser("put", help="upload a file from the Pi")
    put.add_argument("local")
    put.add_argument("remote")
    put.set_defaults(func=cmd_put)

    rm = sub.add_parser("rm", help="delete a file on the Flipper")
    rm.add_argument("path")
    rm.set_defaults(func=cmd_rm)

    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    transport = ExpansionTransport("/dev/ttyAMA0")
    try:
        transport.connect()
        args.func(RpcSession(transport), args)
    except (ExpansionError, RpcError, TimeoutError) as error:
        print(f"flipper: {error}", file=sys.stderr)
        return 1
    finally:
        transport.disconnect()
    return 0


if __name__ == "__main__":
    sys.exit(main())
