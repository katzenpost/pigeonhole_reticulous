# SPDX-License-Identifier: AGPL-3.0-only
"""Carries the simulator's Channel frames to the nearest Pigeonhole replica that serves Channel
mode, over Reticulum, as rnsd does on the device. On the unix socket each frame is a u16 big
endian length and the frame; a zero length from here says the Channel is up. The socket closes
when the Link does."""

import argparse
import os
import socket
import struct
import threading
import time

import RNS
from pigeonhole_rns import APP_NAME, ASPECT, rnsio
from pigeonhole_rns.channel import Frame


def recv_exact(conn, n):
    b = b""
    while len(b) < n:
        c = conn.recv(n - len(b))
        if not c:
            return None
        b += c
    return b


def nearest(replicas):
    cands = rnsio.reachable(list(replicas))
    return min(cands, key=rnsio.hops) if cands else None


def serve(conn, replicas):
    dest = None
    while dest is None:
        dest = nearest(replicas)
        time.sleep(0.5)
    identity = RNS.Identity.recall(dest)
    link = RNS.Link(RNS.Destination(identity, RNS.Destination.OUT, RNS.Destination.SINGLE, APP_NAME, ASPECT))
    deadline = time.time() + 30
    while link.status != RNS.Link.ACTIVE and time.time() < deadline and link.status != RNS.Link.CLOSED:
        time.sleep(0.1)
    if link.status != RNS.Link.ACTIVE:
        print("relay: no link to %s" % RNS.prettyhexrep(dest), flush=True)
        link.teardown()
        return
    print("relay: channel up to %s" % RNS.prettyhexrep(dest), flush=True)
    ch = link.get_channel()
    ch.register_message_type(Frame)
    lock = threading.Lock()

    def on_frame(msg):
        if not isinstance(msg, Frame):
            return False
        raw = msg.pack()
        with lock:
            try:
                conn.sendall(struct.pack(">H", len(raw)) + raw)
            except OSError:
                pass
        return True

    ch.add_message_handler(on_frame)
    link.set_link_closed_callback(lambda _l: conn.shutdown(socket.SHUT_RDWR))
    with lock:
        conn.sendall(b"\0\0")
    while True:
        h = recv_exact(conn, 2)
        if h is None:
            break
        raw = recv_exact(conn, struct.unpack(">H", h)[0])
        if raw is None:
            break
        f = Frame()
        f.unpack(raw)
        while not ch.is_ready_to_send() and link.status == RNS.Link.ACTIVE:
            time.sleep(0.01)
        if link.status != RNS.Link.ACTIVE:
            break
        ch.send(f)
    link.teardown()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", required=True, help="Reticulum config directory")
    ap.add_argument("--socket", required=True)
    a = ap.parse_args()
    RNS.Reticulum(configdir=a.config, loglevel=RNS.LOG_NOTICE)
    replicas = set()
    RNS.Transport.register_announce_handler(
        rnsio.AnnounceHandler(lambda dest, _i, _b, channel: channel and replicas.add(dest)))
    if os.path.exists(a.socket):
        os.unlink(a.socket)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(a.socket)
    srv.listen(1)
    while True:
        conn, _ = srv.accept()
        try:
            serve(conn, replicas)
        except OSError as e:
            print("relay: %s" % e, flush=True)
        conn.close()


if __name__ == "__main__":
    main()
