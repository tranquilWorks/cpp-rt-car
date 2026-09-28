#!/usr/bin/env python3
"""Finite supervision of actual external CIL processes; no shell invocation."""
import argparse
from pathlib import Path
import subprocess
import threading
import uuid


class Peer:
    def __init__(self, executable, key, generation, timeout=5000, mode=None):
        args = [str(executable), key, str(generation), str(timeout)]
        if mode:
            args.append(mode)
        self.process = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        text=True, bufsize=1)
        self.lines = []
        self.ready = threading.Event()
        self.stopped = threading.Event()
        self.overflow = False
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        for line in iter(lambda: self.process.stdout.readline(4096), ""):
            if len(self.lines) >= 64 or len(line) >= 4096:
                self.overflow = True
            else:
                self.lines.append(line)
            if line.startswith("ready "):
                self.ready.set()
            if line == "stopped old controller\n":
                self.stopped.set()
        self.process.stdout.close()

    def wait_ready(self):
        if not self.ready.wait(10):
            raise RuntimeError(f"process did not become ready: {self.lines}")

    def finish(self, expected=0):
        code = self.process.wait(timeout=15)
        self.reader.join(timeout=2)
        output = "".join(self.lines)
        if self.reader.is_alive() or self.overflow or code != expected:
            raise RuntimeError(f"process exit={code}, expected={expected}, output={output!r}")
        return output

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=2)
        self.reader.join(timeout=2)


def key():
    return "test_" + uuid.uuid4().hex


def normal_output(plant, controller, generation):
    expected_plant = (f"ready plant\nplant status=ok generation={generation} applied=8 position=255 ack=1 "
                      "history=128,192,224,240,248,252,254,255\n")
    expected_controller = "ready controller\ncontroller status=ok sent=8 efforts=128,64,32,16,8,4,2,1\n"
    if plant != expected_plant or controller != expected_controller:
        raise RuntimeError(f"feedback oracle mismatch: plant={plant!r}, controller={controller!r}")


def run_suite(plant_path, controller_path, normal_only=False):
    peers = []

    def spawn(exe, name, generation, timeout=5000, mode=None):
        peer = Peer(exe, name, generation, timeout, mode)
        peers.append(peer)
        return peer

    def pair(generation, mode="normal", timeout=5000):
        name = key()
        host = spawn(plant_path, name, generation, timeout)
        host.wait_ready()
        controller = spawn(controller_path, name, generation, timeout, mode)
        return host, controller

    try:
        host, controller = pair(11)
        plant_output, controller_output = host.finish(), controller.finish()
        normal_output(plant_output, controller_output, 11)
        if normal_only:
            print(plant_output + controller_output, end="", flush=True)
            return
        print("PASS actual feedback: 8 commands, every state and exact shutdown", flush=True)
        for mode, result, applied in [("schema", "schema", 0), ("generation", "generation", 0),
                                      ("sequence", "sequence", 0), ("future", "future", 0),
                                      ("expired", "expired", 0), ("correlation", "correlation", 0),
                                      ("replay", "sequence", 1)]:
            host, controller = pair(20, mode, 1000)
            output = host.finish(2)
            controller.finish()
            position = 128 if applied else 0
            if f"status={result} generation=20 applied={applied} position={position}" not in output:
                raise RuntimeError(f"incorrect rejected-input/hold result: {mode}: {output}")
        print("PASS schema/generation/sequence/future/expiry/correlation/replay rejection and hold", flush=True)
        for mode, code in [("crash", 77), ("partial", 78)]:
            host, controller = pair(30, mode, 250)
            controller.finish(code)
            output = host.finish(2)
            if "status=timeout generation=30 applied=0 position=0 ack=0" not in output:
                raise RuntimeError(f"dead peer/partial record was applied: {output}")
        host, controller = pair(31, "no_ack", 500)
        controller.finish()
        if "status=no_ack generation=31 applied=8 position=255 ack=0" not in host.finish(2):
            raise RuntimeError("missing shutdown acknowledgement was accepted")
        print("PASS peer death, unpublished partial payload and missing stop acknowledgement", flush=True)
        alone = spawn(plant_path, key(), 40, 150)
        if "status=timeout generation=40 applied=0 position=0" not in alone.finish(2):
            raise RuntimeError("missing controller did not select hold")
        absent = spawn(controller_path, key(), 40, 150)
        if "controller setup=not_found" not in absent.finish(2):
            raise RuntimeError("missing mapping did not fail")
        name = key()
        host = spawn(plant_path, name, 41, 500)
        host.wait_ready()
        wrong = spawn(controller_path, name, 42, 500)
        if "controller setup=generation" not in wrong.finish(2):
            raise RuntimeError("wrong generation attached")
        host.finish(2)
        name = key()
        host = spawn(plant_path, name, 43)
        host.wait_ready()
        duplicate = spawn(plant_path, name, 43)
        if "plant setup=exists" not in duplicate.finish(2):
            raise RuntimeError("exclusive creation failed")
        controller = spawn(controller_path, name, 43)
        normal_output(host.finish(), controller.finish(), 43)
        name = key()
        host = spawn(plant_path, name, 44, 750)
        host.wait_ready()
        first = spawn(controller_path, name, 44, 750, "hold")
        first.wait_ready()
        second = spawn(controller_path, name, 44, 750)
        if "controller setup=attached" not in second.finish(2):
            raise RuntimeError("second writer acquired an occupied session")
        host.finish(2)
        first.finish()
        print("PASS missing mapping/controller, generation mismatch and exclusive creator/writer", flush=True)
        old_name = key()
        old_host = spawn(plant_path, old_name, 50, 250)
        old_host.wait_ready()
        old = spawn(controller_path, old_name, 50, 250, "linger")
        old.wait_ready()
        if not old.stopped.wait(5):
            raise RuntimeError("old session did not stop")
        old_host.finish(2)
        if old.process.poll() is not None:
            raise RuntimeError("old view was not alive during reconnect")
        host, controller = pair(51)
        normal_output(host.finish(), controller.finish(), 51)
        old.finish()
        # Old name must not admit another controller after both sides detach.
        stale = spawn(controller_path, old_name, 50, 150)
        if "controller setup=not_found" not in stale.finish(2):
            raise RuntimeError("old mapping name leaked after cleanup")
        print("PASS fresh-generation reconnect while old mapped peer writes", flush=True)
        a_key, b_key = key(), key()
        a = spawn(plant_path, a_key, 61)
        b = spawn(plant_path, b_key, 62)
        a.wait_ready()
        b.wait_ready()  # Both Runtime instances are live before either controller starts.
        ac = spawn(controller_path, a_key, 61)
        bc = spawn(controller_path, b_key, 62)
        normal_output(a.finish(), ac.finish(), 61)
        normal_output(b.finish(), bc.finish(), 62)
        print("PASS concurrent isolated sessions", flush=True)
    finally:
        for peer in reversed(peers):
            peer.close()
    print("PASS CIL process suite", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--plant", required=True, type=Path)
    parser.add_argument("--controller", required=True, type=Path)
    parser.add_argument("--normal-only", action="store_true")
    args = parser.parse_args()
    run_suite(args.plant.resolve(), args.controller.resolve(), args.normal_only)
