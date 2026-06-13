// fm2k-relay -- a standalone, volunteer-hostable UDP relay for FM2K rollback
// netplay. It is NOT the hub: it carries no lobby, matchmaking, auth, DB, or
// user data. It only forwards 0xCF-wrapped UDP between the two peers of a
// match the hub has authorized over a WSS control channel. See
// docs/dev/nat_reachability_plan.md "Phase 4" in the wanwan repo.
//
// Build:  cd relay && go build -o fm2k-relay .
// Run:    ./fm2k-relay -hub wss://hub.2dfm.org/ -key <shared-hex-key> \
//                      -region ap-southeast -public relay.example.com:7712
// Open the UDP -listen port (default 7712) inbound. One static binary, no
// runtime deps; cross-compile e.g. GOOS=linux GOARCH=amd64 go build -o ...
package main

import (
	"crypto/rand"
	"encoding/hex"
	"flag"
	"log"
	"net"
	"os"
	"time"
)

func envOr(k, d string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return d
}

func randHex(n int) string {
	b := make([]byte, n)
	_, _ = rand.Read(b)
	return hex.EncodeToString(b)
}

func main() {
	listen := flag.String("listen", envOr("FM2K_RELAY_LISTEN", ":7712"), "UDP listen address host:port")
	region := flag.String("region", envOr("FM2K_RELAY_REGION", "unknown"), "region tag, e.g. ap-southeast (used by hub geo-selection)")
	hub := flag.String("hub", envOr("FM2K_RELAY_HUB", ""), "hub control WSS URL, e.g. wss://hub.2dfm.org/")
	public := flag.String("public", envOr("FM2K_RELAY_PUBLIC", ""), "public host:port clients dial (default: hub uses WS source IP + listen port)")
	keyStr := flag.String("key", envOr("FM2K_RELAY_REGISTER_KEY", ""), "shared HMAC registration key (hex or raw)")
	maxPkt := flag.Int("maxpkt", 2048, "max forwarded packet size in bytes (gekko packets are small)")
	ttlSec := flag.Int("session-ttl", 21600, "idle-session eviction seconds")
	rateKbps := flag.Int("rate-kbps", 512, "per-session forward rate cap in kbps (0 = unlimited)")
	flag.Parse()

	if *hub == "" || *keyStr == "" {
		log.Fatal("fm2k-relay: -hub and -key are required (see -h)")
	}
	key, err := hex.DecodeString(*keyStr)
	if err != nil {
		key = []byte(*keyStr) // not hex -> use raw bytes; hub must match
	}

	udpAddr, err := net.ResolveUDPAddr("udp", *listen)
	if err != nil {
		log.Fatalf("resolve %s: %v", *listen, err)
	}
	conn, err := net.ListenUDP("udp", udpAddr)
	if err != nil {
		log.Fatalf("listen %s: %v", *listen, err)
	}
	defer conn.Close()
	log.Printf("fm2k-relay: forwarding on %s region=%s -> hub %s", conn.LocalAddr(), *region, *hub)

	fwd := NewForwarder(conn, *maxPkt, time.Duration(*ttlSec)*time.Second, *rateKbps)
	go func() {
		t := time.NewTicker(60 * time.Second)
		for range t.C {
			fwd.Sweep()
		}
	}()

	cc := &controlClient{
		hubURL: *hub, key: key, region: *region,
		udpPort: udpAddr.Port, public: *public, fwd: fwd,
	}
	go cc.Run()

	fwd.Run() // blocks until the socket closes
}
