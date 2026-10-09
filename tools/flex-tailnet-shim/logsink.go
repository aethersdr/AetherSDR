package main

import (
	"bytes"
	"io"
	"log"
	"net/http"
	"os"
	"sync"
	"time"
)

// The radio gives containers no log channel. For development, SHIM_LOG_URL
// makes the shim POST its recent log lines to a listener every 15 s.
type logRing struct {
	mu    sync.Mutex
	lines [][]byte
}

func (r *logRing) Write(p []byte) (int, error) {
	r.mu.Lock()
	r.lines = append(r.lines, append([]byte{}, p...))
	if len(r.lines) > 400 {
		r.lines = r.lines[len(r.lines)-400:]
	}
	r.mu.Unlock()
	return len(p), nil
}

func (r *logRing) drain() []byte {
	r.mu.Lock()
	defer r.mu.Unlock()
	b := bytes.Join(r.lines, nil)
	r.lines = nil
	return b
}

func startLogSink() {
	url := os.Getenv("SHIM_LOG_URL")
	if url == "" {
		return
	}
	ring := &logRing{}
	log.SetOutput(io.MultiWriter(os.Stderr, ring))
	go func() {
		c := &http.Client{Timeout: 10 * time.Second}
		for {
			time.Sleep(15 * time.Second)
			if b := ring.drain(); len(b) > 0 {
				if resp, err := c.Post(url, "text/plain", bytes.NewReader(b)); err == nil {
					resp.Body.Close()
				}
			}
		}
	}()
}
