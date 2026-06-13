package main

import (
	"net"
	"testing"
	"time"
)

func ua(ip string, port int) *net.UDPAddr {
	return &net.UDPAddr{IP: net.ParseIP(ip), Port: port}
}

// route must learn slot A then B and forward to the opposite peer, dropping
// any 3rd distinct source -- identical to hub.py RelaySession.route().
func TestRouteTwoSlotsThenDrop(t *testing.T) {
	s := &session{}
	a, b, c := ua("1.1.1.1", 100), ua("2.2.2.2", 200), ua("3.3.3.3", 300)

	if dst := s.route(a); dst != nil {
		t.Fatalf("first packet (only A known) should have no dst, got %v", dst)
	}
	if dst := s.route(b); dst == nil || !udpEqual(dst, a) {
		t.Fatalf("B should route to A, got %v", dst)
	}
	if dst := s.route(a); dst == nil || !udpEqual(dst, b) {
		t.Fatalf("A should route to B, got %v", dst)
	}
	if dst := s.route(c); dst != nil {
		t.Fatalf("3rd distinct source must drop, got %v", dst)
	}
}

// Authorize sets a future expiry; Sweep removes expired ones.
func TestAuthorizeAndSweep(t *testing.T) {
	f := NewForwarder(nil, 2048, time.Hour, 0)
	var id sessionID
	id[0] = 0xAB

	if _, ok := f.authorized[id]; ok {
		t.Fatal("must start unauthorized")
	}
	f.Authorize(id, time.Minute)
	if exp, ok := f.authorized[id]; !ok || !time.Now().Before(exp) {
		t.Fatal("Authorize must set a future expiry")
	}
	f.authorized[id] = time.Now().Add(-time.Second) // force-expire
	f.Sweep()
	if _, ok := f.authorized[id]; ok {
		t.Fatal("expired authorization must be swept")
	}
}

// Token bucket must throttle once a session exceeds its byte rate.
func TestRateLimit(t *testing.T) {
	f := NewForwarder(nil, 2048, time.Hour, 8) // 8 kbps = 1000 bytes/s
	s := &session{lastRefill: time.Now(), tokens: 1000}
	now := time.Now()
	if !f.allow(s, 900, now) {
		t.Fatal("first 900B under the 1000B burst should pass")
	}
	if f.allow(s, 900, now) {
		t.Fatal("second 900B same instant should be throttled")
	}
}
