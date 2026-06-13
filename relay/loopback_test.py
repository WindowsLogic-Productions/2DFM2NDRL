# End-to-end loopback test for fm2k-relay: a stub hub authorizes one session,
# the real relay binary forwards 0xCF packets between two UDP peers both ways,
# and an unauthorized session must be dropped. Requires python `websockets`.
#
#   cd relay && go build -o fm2k-relay .
#   FM2K_RELAY_BIN=./fm2k-relay python3 loopback_test.py
import asyncio, json, hmac, hashlib, os, socket, subprocess, time
import websockets

KEY    = b"loopback-test-key"        # non-hex -> relay uses raw bytes (matches)
REGION = "AS"
TOKEN  = "00112233445566778899aabbccddeeff"  # 16-byte session_id, hex
WS_PORT, UDP_PORT = 8771, 7799
RELAY  = os.environ.get("FM2K_RELAY_BIN", "./fm2k-relay")
ENV    = bytes([0xCF, 0x01]) + bytes.fromhex(TOKEN)   # 0xCF envelope prefix

authorized = asyncio.Event()

async def hub(ws):
    raw = await ws.recv()
    m = json.loads(raw)
    assert m.get("type") == "relay_register", m
    canon = f"relay_register|{m['region']}|{m['udp_port']}|{m['ts']}|{m['nonce']}".encode()
    expect = hmac.new(KEY, canon, hashlib.sha256).hexdigest()
    assert hmac.compare_digest(expect, m["mac"]), "register HMAC rejected"
    await ws.send(json.dumps({"type": "relay_register_ok"}))
    await ws.send(json.dumps({"type": "relay_authorize_session", "session_id": TOKEN, "ttl_s": 3600}))
    authorized.set()
    async for _ in ws:   # drain heartbeats
        pass

def recv_ok(sock, expect):
    try:
        data, _ = sock.recvfrom(2048)
        return data == expect
    except socket.timeout:
        return False

async def main():
    async with websockets.serve(hub, "127.0.0.1", WS_PORT):
        proc = subprocess.Popen(
            [RELAY, "-hub", f"ws://127.0.0.1:{WS_PORT}/", "-key", KEY.decode(),
             "-region", REGION, "-listen", f"127.0.0.1:{UDP_PORT}", "-rate-kbps", "0"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            await asyncio.wait_for(authorized.wait(), timeout=10)
            await asyncio.sleep(1.0)  # let the relay apply the authorize push
            relay = ("127.0.0.1", UDP_PORT)
            a = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); a.bind(("127.0.0.1", 0)); a.settimeout(2)
            b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); b.bind(("127.0.0.1", 0)); b.settimeout(2)

            a.sendto(ENV + b"A1", relay)          # learns slot A (no dst yet)
            time.sleep(0.2)
            b.sendto(ENV + b"B1", relay)          # learns slot B -> forwards to A
            got_BtoA = recv_ok(a, ENV + b"B1")
            a.sendto(ENV + b"A2", relay)          # A -> B
            got_AtoB = recv_ok(b, ENV + b"A2")

            bad = bytes([0xCF, 0x01]) + bytes(16) + b"X"   # unauthorized session
            a.sendto(bad, relay)
            leaked = recv_ok(b, bad)

            print(f"forward A->B:          {'PASS' if got_AtoB else 'FAIL'}")
            print(f"forward B->A:          {'PASS' if got_BtoA else 'FAIL'}")
            print(f"unauthorized dropped:  {'PASS' if not leaked else 'FAIL'}")
            ok = got_AtoB and got_BtoA and not leaked
            print("RESULT:", "ALL PASS" if ok else "FAILURE")
            raise SystemExit(0 if ok else 1)
        finally:
            proc.terminate()

asyncio.run(main())
