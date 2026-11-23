#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Test client: fetches pages from a NomadNet node using Python RNS.

Usage:
  fetch_page.py --config <reticulum config dir> [--hash <destination>] [--name madpico]
                [--path /page/index.mu --path /page/status.mu] [--timeout 120]

Without --hash, the client waits for an announce with a matching name.
Exit code 0 if all pages were fetched successfully.
"""
import argparse
import sys
import time

import RNS

ap = argparse.ArgumentParser()
ap.add_argument("--config", default=None, help="Reticulum config directory")
ap.add_argument("--hash", default=None, help="destination hash of the node")
ap.add_argument("--name", default="madpico", help="node name to wait for (without --hash)")
ap.add_argument("--path", action="append", default=[], help="page path, can be repeated")
ap.add_argument("--timeout", type=float, default=120)
ap.add_argument("--loglevel", type=int, default=RNS.LOG_WARNING)
args = ap.parse_args()
paths = args.path or ["/page/index.mu", "/page/status.mu"]

found = {"hash": bytes.fromhex(args.hash) if args.hash else None}


class AnnounceHandler:
    aspect_filter = "nomadnetwork.node"

    def received_announce(self, destination_hash, announced_identity, app_data):
        name = app_data.decode("utf-8", "replace") if app_data else ""
        print(f"[announce] {RNS.prettyhexrep(destination_hash)} \"{name}\"", flush=True)
        if found["hash"] is None and name == args.name:
            found["hash"] = destination_hash


reticulum = RNS.Reticulum(args.config, loglevel=args.loglevel)
RNS.Transport.register_announce_handler(AnnounceHandler())

deadline = time.time() + args.timeout
print("Waiting for node ...", flush=True)
while found["hash"] is None and time.time() < deadline:
    time.sleep(0.2)
if found["hash"] is None:
    print("ERROR: no announce received")
    sys.exit(2)

dest_hash = found["hash"]
if not RNS.Transport.has_path(dest_hash):
    print("No path known, sending path request ...", flush=True)
    RNS.Transport.request_path(dest_hash)
    while not RNS.Transport.has_path(dest_hash) and time.time() < deadline:
        time.sleep(0.2)
    if not RNS.Transport.has_path(dest_hash):
        print("ERROR: no path to node")
        sys.exit(3)

identity = RNS.Identity.recall(dest_hash)
destination = RNS.Destination(identity, RNS.Destination.OUT, RNS.Destination.SINGLE,
                              "nomadnetwork", "node")

state = {"established": False, "closed": False}
link = RNS.Link(destination)
t_link = time.time()


def established(l):
    state["established"] = True
    rtt = l.rtt * 1000 if l.rtt else 0
    print(f"Link established in {(time.time() - t_link) * 1000:.0f} ms, RTT {rtt:.0f} ms", flush=True)


def closed(l):
    state["closed"] = True


link.set_link_established_callback(established)
link.set_link_closed_callback(closed)

while not state["established"] and not state["closed"] and time.time() < deadline:
    time.sleep(0.1)
if not state["established"]:
    print("ERROR: link could not be established")
    sys.exit(4)

all_ok = True
for path in paths:
    result = {"done": False, "ok": False}
    t0 = time.time()

    def on_response(receipt, path=path, t0=t0, result=result):
        data = receipt.response
        if isinstance(data, bytes):
            data = data.decode("utf-8", "replace")
        print(f"\n----- {path} ({(time.time() - t0) * 1000:.0f} ms) -----\n{data}----- end -----", flush=True)
        result["ok"] = True
        result["done"] = True

    def on_failed(receipt, path=path, result=result):
        print(f"ERROR: request {path} failed", flush=True)
        result["done"] = True

    link.request(path, data=None, response_callback=on_response, failed_callback=on_failed, timeout=30)
    while not result["done"] and time.time() < deadline:
        time.sleep(0.1)
    all_ok = all_ok and result["ok"]

link.teardown()
time.sleep(0.5)
sys.exit(0 if all_ok else 1)
