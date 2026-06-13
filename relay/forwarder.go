package main

import (
	"net"
	"sync"
	"time"
)

// Wire format (identical to hub.py _RelayProto / RelaySession, frozen):
//   0xCF 0x01 [16-byte session_id] [original UDP payload bytes...]
// Sessions are 2-slot self-learning: the first packet from each peer fills
// a slot (learning the peer's actual relay-facing NAT mapping, which for a
// symmetric NAT differs from its STUN-learned port); packets are forwarded
// as-is to the OTHER slot. A 3rd distinct source for the same session is
// dropped. The ONLY difference from the in-process hub relay: a federated
// relay forwards a session only after the hub has authorized its id over
// the control channel (control.go), which is what keeps it from being an
// open relay.
const (
	relayMagic   = 0xCF
	relayTagData = 0x01
	sessionIDLen = 16
	headerLen    = 2 + sessionIDLen // 18
)

type sessionID [sessionIDLen]byte

type session struct {
	slotA, slotB *net.UDPAddr
	createdAt    time.Time
	lastSeen     time.Time
	tokens       float64 // token-bucket bytes
	lastRefill   time.Time
}

// route mirrors hub.py RelaySession.route(): A then B, 3rd source dropped.
func (s *session) route(src *net.UDPAddr) *net.UDPAddr {
	switch {
	case s.slotA != nil && udpEqual(s.slotA, src):
		return s.slotB
	case s.slotB != nil && udpEqual(s.slotB, src):
		return s.slotA
	case s.slotA == nil:
		s.slotA = src
		return s.slotB
	case s.slotB == nil:
		s.slotB = src
		return s.slotA
	default:
		return nil
	}
}

func udpEqual(a, b *net.UDPAddr) bool { return a.Port == b.Port && a.IP.Equal(b.IP) }

// Forwarder owns the UDP socket and the session/authorization state.
type Forwarder struct {
	conn       *net.UDPConn
	maxPkt     int
	sessionTTL time.Duration
	ratePerSec float64 // bytes/sec per session; <=0 means unlimited

	mu         sync.Mutex
	sessions   map[sessionID]*session
	authorized map[sessionID]time.Time // value = expiry
}

func NewForwarder(conn *net.UDPConn, maxPkt int, ttl time.Duration, rateKbps int) *Forwarder {
	return &Forwarder{
		conn:       conn,
		maxPkt:     maxPkt,
		sessionTTL: ttl,
		ratePerSec: float64(rateKbps) * 1000.0 / 8.0,
		sessions:   make(map[sessionID]*session),
		authorized: make(map[sessionID]time.Time),
	}
}

// Authorize marks a session forwardable until now+ttl. Called from the
// control client when the hub pushes relay_authorize_session.
func (f *Forwarder) Authorize(id sessionID, ttl time.Duration) {
	f.mu.Lock()
	f.authorized[id] = time.Now().Add(ttl)
	f.mu.Unlock()
}

// Run is the receive/forward loop; blocks until the socket closes.
func (f *Forwarder) Run() {
	buf := make([]byte, 65536)
	for {
		n, src, err := f.conn.ReadFromUDP(buf)
		if err != nil {
			return
		}
		if n < headerLen || n > f.maxPkt {
			continue
		}
		if buf[0] != relayMagic || buf[1] != relayTagData {
			continue
		}
		var id sessionID
		copy(id[:], buf[2:2+sessionIDLen])

		now := time.Now()
		f.mu.Lock()
		exp, ok := f.authorized[id]
		if !ok || !now.Before(exp) {
			f.mu.Unlock()
			continue // unauthorized session -> not an open relay
		}
		sess := f.sessions[id]
		if sess == nil {
			sess = &session{createdAt: now, lastSeen: now, lastRefill: now, tokens: f.burst()}
			f.sessions[id] = sess
		}
		sess.lastSeen = now
		if !f.allow(sess, n, now) {
			f.mu.Unlock()
			continue
		}
		dst := sess.route(cloneUDP(src))
		f.mu.Unlock()

		if dst == nil {
			continue
		}
		// Forward as-is: the peer strips [0xCF 0x01 session_id] itself.
		_, _ = f.conn.WriteToUDP(buf[:n], dst)
	}
}

func (f *Forwarder) burst() float64 {
	if f.ratePerSec <= 0 {
		return 0
	}
	return f.ratePerSec // 1s of burst
}

// allow applies a per-session token-bucket byte-rate cap. Caller holds mu.
func (f *Forwarder) allow(s *session, n int, now time.Time) bool {
	if f.ratePerSec <= 0 {
		return true
	}
	s.tokens += now.Sub(s.lastRefill).Seconds() * f.ratePerSec
	s.lastRefill = now
	if s.tokens > f.ratePerSec {
		s.tokens = f.ratePerSec
	}
	if s.tokens < float64(n) {
		return false
	}
	s.tokens -= float64(n)
	return true
}

// Sweep drops expired authorizations and idle sessions. Run periodically.
func (f *Forwarder) Sweep() {
	now := time.Now()
	f.mu.Lock()
	for id, exp := range f.authorized {
		if now.After(exp) {
			delete(f.authorized, id)
		}
	}
	for id, s := range f.sessions {
		if now.Sub(s.lastSeen) > f.sessionTTL {
			delete(f.sessions, id)
		}
	}
	f.mu.Unlock()
}

func cloneUDP(a *net.UDPAddr) *net.UDPAddr {
	ip := make(net.IP, len(a.IP))
	copy(ip, a.IP)
	return &net.UDPAddr{IP: ip, Port: a.Port, Zone: a.Zone}
}
