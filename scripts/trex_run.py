#!/usr/bin/env python3
"""Run a TRex traffic burst against the DUT with configurable pps, frame size
and duration. Can be launched on danger50 (the TRex host) or from any other host
(e.g. danger51): in that case it copies itself to danger50 and runs there over
ssh, printing the same result.

Starts the TRex server (unless one is already up), generates a single burst of
`pps * duration` packets of the requested shape and frame size, waits for it to
finish and prints TRex's tx/rx numbers as JSON. The server is stopped again
unless --keep-server is given.

Traffic shapes (--mode):
  esp   Ether/IPv4(proto 50)/ESP with an incrementing Sequence Number. Matches
        the DUT's --doca-mode esp_rr steering (same layout as trex_scripts/esp_rr.py).
  udp   Ether/IPv4/UDP, src port cycling over --flows values so RSS spreads it.
        For --doca-mode meta_rr or none.

--frame-size is bytes on the wire including the 4-byte FCS (64 = minimum, 92
and 1518 are the sizes used in MUSQ_TEST_RESULTS.md); the built frame content is
frame-size - 4.

Examples:
  scripts/trex_run.py --pps 1e6 --duration 10 --frame-size 92          # from danger51 or danger50
  scripts/trex_run.py --mode udp --flows 64 --pps 2e6 --duration 10 --frame-size 1518
  scripts/trex_run.py --server-only            # just (re)start the server and leave it up
  scripts/trex_run.py --stop-server
"""
import argparse
import json
import os
import shlex
import socket
import subprocess
import sys
import time
import warnings

# TRex's bundled scapy emits cryptography deprecation warnings on import.
warnings.filterwarnings("ignore", category=DeprecationWarning)
try:
    from cryptography.utils import CryptographyDeprecationWarning
    warnings.filterwarnings("ignore", category=CryptographyDeprecationWarning)
except Exception:
    pass

TREX_HOST = "danger50"
REMOTE_COPY = "/homes/ofekdg/trex_scripts/trex_run.py"
TREX_DIR = "/homes/ofekdg/trex-core/scripts"
TREX_SERVER_BIN = os.path.join(TREX_DIR, "t-rex-64")
SERVER_PROC_NAME = "_t-rex-64"          # t-rex-64 is a wrapper that execs this
SERVER_LOG = "/tmp/trex_server.log"
# NUMA node 0 of danger50, matching /etc/trex_cfg.yaml (NIC 0000:17:00.1 is on node 0).
CPU_PIN_LIST = "0,2,4,6,8,10,12,14,16,18,20,22"
TREX_DP_CORES = 2

# Same L2/L3 identities as trex_scripts/*.py (dst = DUT port as configured in trex_cfg.yaml).
SRC_MAC = "0c:42:a1:1d:3b:5e"
DST_MAC = "f4:20:4d:24:44:86"
SRC_IP = "1.1.1.1"
DST_IP = "2.2.2.2"
ESP_PROTO = 50
SPI_CONST = 0x00001000
UDP_SPORT_BASE = 1025
UDP_DPORT = 12345
MIN_FRAME_SIZE = 64


def sh(cmd, check=True, timeout=60):
    return subprocess.run(cmd, shell=True, check=check, timeout=timeout,
                          capture_output=True, text=True)


def server_running():
    return sh(f"pgrep -x {SERVER_PROC_NAME}", check=False).returncode == 0


def stop_server():
    sh(f"sudo -n pkill -x {SERVER_PROC_NAME} || true", check=False)
    for _ in range(20):
        if not server_running():
            return
        time.sleep(0.5)


def start_server():
    stop_server()
    cmd = (f"cd {shlex.quote(TREX_DIR)} && : > {SERVER_LOG} && "
           f"sudo -n taskset -c {CPU_PIN_LIST} nohup {shlex.quote(TREX_SERVER_BIN)} -i "
           f"--no-scapy-server --no-ofed-check --mlx5-so -c {TREX_DP_CORES} "
           f"> {SERVER_LOG} 2>&1 < /dev/null &")
    sh(cmd)
    for _ in range(30):
        time.sleep(1)
        r = sh(f"grep -E 'Starting  TRex|Global stats enabled|Total-Tx' {SERVER_LOG}", check=False)
        if r.returncode == 0:
            time.sleep(5)  # let the RPC server come up
            return
    tail = sh(f"tail -n 40 {SERVER_LOG}", check=False).stdout
    raise SystemExit(f"TRex server did not become ready; log tail:\n{tail}")


def build_stream(mode, frame_size, pps, count, flows):
    from trex_stl_lib.api import (Ether, IP, UDP, STLPktBuilder, STLScVmRaw, STLStream,
                                  STLTXSingleBurst, STLVmFlowVar, STLVmWrFlowVar)
    content_len = frame_size - 4
    if mode == "esp":
        base = Ether(src=SRC_MAC, dst=DST_MAC) / IP(src=SRC_IP, dst=DST_IP, proto=ESP_PROTO)
        hdr_len = len(base)                       # 34
        sn_offset = hdr_len + 4                   # after the 4-byte SPI
        seq_offset = sn_offset + 4                # esp_rr.py convention: SN copy at payload offset 42
        payload = SPI_CONST.to_bytes(4, "big") + b"\x00" * (content_len - hdr_len - 4)
        vm = STLScVmRaw([
            STLVmFlowVar(name="sn", min_value=0, max_value=0xFFFFFFFF, size=4, op="inc", step=1),
            STLVmWrFlowVar(fv_name="sn", pkt_offset=sn_offset),
            STLVmWrFlowVar(fv_name="sn", pkt_offset=seq_offset),
        ])
        pkt = STLPktBuilder(pkt=base / payload, vm=vm)
    else:
        base = Ether(src=SRC_MAC, dst=DST_MAC) / IP(src=SRC_IP, dst=DST_IP) / \
            UDP(sport=UDP_SPORT_BASE, dport=UDP_DPORT)
        hdr_len = len(base)                       # 42
        payload = b"X" * (content_len - hdr_len)
        vm = None
        if flows > 1:
            vm = STLScVmRaw([
                STLVmFlowVar(name="sp", min_value=UDP_SPORT_BASE, max_value=UDP_SPORT_BASE + flows - 1,
                             size=2, op="inc"),
                STLVmWrFlowVar(fv_name="sp", pkt_offset="UDP.sport"),
                # UDP checksum left as built for the base port: zero it so it is
                # not wrong after the port rewrite (0 = no checksum in IPv4).
            ])
        pkt_obj = base / payload
        if flows > 1:
            pkt_obj[UDP].chksum = 0
        pkt = STLPktBuilder(pkt=pkt_obj, vm=vm)
    assert len(pkt.pkt) == content_len, f"built {len(pkt.pkt)}B content, expected {content_len}"
    return STLStream(packet=pkt, mode=STLTXSingleBurst(pps=pps, total_pkts=count))


def run_burst(args):
    sys.path.insert(0, os.path.join(TREX_DIR, "automation", "trex_control_plane", "interactive"))
    from trex_stl_lib.api import STLClient, STLError

    count = int(args.pps * args.duration)
    c = STLClient(server="localhost")
    try:
        c.connect()
        c.reset(ports=[args.port])
        c.add_streams(build_stream(args.mode, args.frame_size, args.pps, count, args.flows),
                      ports=[args.port])
        c.clear_stats()
        t0 = time.time()
        c.start(ports=[args.port], force=True)
        c.wait_on_traffic(ports=[args.port], timeout=args.duration + 30)
        elapsed = max(time.time() - t0, 1e-6)
        s = c.get_stats()[args.port]
        tx, rx = int(s["opackets"]), int(s["ipackets"])
        return {
            "mode": args.mode, "frame_size": args.frame_size, "offered_pps": args.pps,
            "duration_s": args.duration, "elapsed_s": round(elapsed, 3),
            "tx_pkts": tx, "rx_pkts": rx, "lost_pkts": tx - rx,
            "loss_pct": round(100.0 * (tx - rx) / max(tx, 1), 4),
            "tx_pps": round(tx / elapsed), "rx_pps": round(rx / elapsed),
            "tx_gbps": round(int(s["obytes"]) * 8 / elapsed / 1e9, 3),
            "rx_gbps": round(int(s["ibytes"]) * 8 / elapsed / 1e9, 3),
        }
    except STLError as e:
        raise SystemExit(f"STLError: {e}")
    finally:
        try:
            c.disconnect()
        except Exception:
            pass


def run_remote(argv):
    """Copy this script to the TRex host and run it there with the same arguments."""
    me = os.path.abspath(__file__)
    subprocess.run(["scp", "-q", "-o", "BatchMode=yes", me, f"{TREX_HOST}:{REMOTE_COPY}"], check=True)
    cmd = "python3 " + shlex.quote(REMOTE_COPY) + " " + " ".join(shlex.quote(a) for a in argv)
    return subprocess.call(["ssh", "-o", "BatchMode=yes", TREX_HOST, cmd])


def main():
    if socket.gethostname().split(".")[0] != TREX_HOST:
        return run_remote(sys.argv[1:])
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mode", choices=["esp", "udp"], default="esp")
    ap.add_argument("--pps", type=float, default=1e6, help="offered rate, packets/s")
    ap.add_argument("--duration", type=float, default=10.0, help="burst length, seconds")
    ap.add_argument("--frame-size", type=int, default=92, help="bytes on the wire incl. FCS (>= 64)")
    ap.add_argument("--flows", type=int, default=1, help="udp: number of distinct UDP source ports")
    ap.add_argument("--port", type=int, default=0, help="TRex port index")
    ap.add_argument("--keep-server", action="store_true", help="leave the TRex server running afterwards")
    ap.add_argument("--server-only", action="store_true", help="(re)start the server and exit")
    ap.add_argument("--stop-server", action="store_true", help="stop the server and exit")
    args = ap.parse_args()

    if args.stop_server:
        stop_server()
        return 0
    if args.frame_size < MIN_FRAME_SIZE:
        ap.error(f"--frame-size must be >= {MIN_FRAME_SIZE}")
    if args.flows < 1 or args.flows > 60000:
        ap.error("--flows must be in 1..60000")

    started = False
    if args.server_only or not server_running():
        print("starting TRex server ...", file=sys.stderr)
        start_server()
        started = True
    if args.server_only:
        return 0
    try:
        result = run_burst(args)
        print(json.dumps(result, indent=2))
    finally:
        if started and not args.keep_server:
            stop_server()
    return 0


if __name__ == "__main__":
    sys.exit(main())
