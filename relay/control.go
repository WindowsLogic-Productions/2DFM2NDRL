package main

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"log"
	"time"

	"github.com/gorilla/websocket"
)

// controlClient maintains the relay's long-lived WSS link to the hub. It
// authenticates with HMAC over a shared registration key (NOT the hub source,
// NOT the lobby OAuth handshake -- just one revocable symmetric secret), then
// receives relay_authorize_session pushes that gate which sessions the
// forwarder will relay. Reconnects forever with capped backoff.
type controlClient struct {
	hubURL  string
	key     []byte
	region  string
	udpPort int
	public  string
	fwd     *Forwarder
}

// relay -> hub. MAC = HMAC-SHA256(key, "relay_register|region|udp_port|ts|nonce").
// Hub verifies |now-ts| is small (anti-replay) and the MAC matches.
type registerMsg struct {
	Type    string `json:"type"` // "relay_register"
	Region  string `json:"region"`
	UDPPort int    `json:"udp_port"`
	Public  string `json:"public,omitempty"`
	TS      int64  `json:"ts"`
	Nonce   string `json:"nonce"`
	MAC     string `json:"mac"`
}

// hub -> relay.
type inboundMsg struct {
	Type      string `json:"type"`
	SessionID string `json:"session_id"` // hex, 32 chars = 16 bytes
	TTLSec    int    `json:"ttl_s"`
	Reason    string `json:"reason"`
}

func (c *controlClient) macFor(ts int64, nonce string) string {
	canon := fmt.Sprintf("relay_register|%s|%d|%d|%s", c.region, c.udpPort, ts, nonce)
	m := hmac.New(sha256.New, c.key)
	m.Write([]byte(canon))
	return hex.EncodeToString(m.Sum(nil))
}

func (c *controlClient) Run() {
	backoff := time.Second
	for {
		if err := c.session(); err != nil {
			log.Printf("control: %v (retry in %s)", err, backoff)
		}
		time.Sleep(backoff)
		if backoff < 30*time.Second {
			backoff *= 2
		}
	}
}

func (c *controlClient) session() error {
	ws, _, err := websocket.DefaultDialer.Dial(c.hubURL, nil)
	if err != nil {
		return fmt.Errorf("dial: %w", err)
	}
	defer ws.Close()

	ts := time.Now().Unix()
	nonce := randHex(8)
	reg := registerMsg{
		Type: "relay_register", Region: c.region, UDPPort: c.udpPort,
		Public: c.public, TS: ts, Nonce: nonce, MAC: c.macFor(ts, nonce),
	}
	if err := ws.WriteJSON(reg); err != nil {
		return fmt.Errorf("register: %w", err)
	}
	log.Printf("control: sent registration region=%s udp_port=%d", c.region, c.udpPort)

	// Heartbeat goroutine. gorilla allows one concurrent reader + one writer;
	// the main loop below only reads, this is the only writer post-register.
	done := make(chan struct{})
	go func() {
		t := time.NewTicker(15 * time.Second)
		defer t.Stop()
		for {
			select {
			case <-done:
				return
			case <-t.C:
				if err := ws.WriteJSON(map[string]any{
					"type": "relay_heartbeat", "ts": time.Now().Unix(),
				}); err != nil {
					ws.Close()
					return
				}
			}
		}
	}()
	defer close(done)

	for {
		var in inboundMsg
		if err := ws.ReadJSON(&in); err != nil {
			return fmt.Errorf("read: %w", err)
		}
		switch in.Type {
		case "relay_authorize_session":
			raw, err := hex.DecodeString(in.SessionID)
			if err != nil || len(raw) != sessionIDLen {
				log.Printf("control: bad authorize session_id %q", in.SessionID)
				continue
			}
			var id sessionID
			copy(id[:], raw)
			ttl := time.Duration(in.TTLSec) * time.Second
			if ttl <= 0 {
				ttl = 6 * time.Hour
			}
			c.fwd.Authorize(id, ttl)
			log.Printf("control: authorized session %s (ttl %s)", short(in.SessionID), ttl)
		case "relay_register_ok":
			log.Printf("control: hub accepted registration")
		case "error":
			return fmt.Errorf("hub rejected relay: %s", in.Reason)
		}
	}
}

func short(s string) string {
	if len(s) > 8 {
		return s[:8]
	}
	return s
}
