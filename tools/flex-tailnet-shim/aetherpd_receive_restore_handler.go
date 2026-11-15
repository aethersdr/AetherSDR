package main

import (
	"context"
	"errors"
	"log"
	"net/http"
	"time"
)

func aetherPDReceiveRestoreHandler(n *Node) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		token := bearer(r)
		if !n.checkToken(token) {
			time.Sleep(500 * time.Millisecond)
			writeJSON(w, http.StatusUnauthorized, apiError{errUnauthorized.Error()})
			return
		}
		if r.URL.RawQuery != "" || r.ContentLength != 0 {
			writeJSON(w, http.StatusBadRequest, apiError{"this endpoint accepts no parameters or body"})
			return
		}
		n.mu.Lock()
		process := n.pdProcess
		n.mu.Unlock()
		if process == nil {
			writeJSON(w, http.StatusConflict, map[string]string{"state": "receive_restore_unavailable"})
			return
		}
		ctx, cancel := context.WithTimeout(r.Context(), 6*time.Second)
		defer cancel()
		err := process.restoreReceive(ctx, func() bool { return n.checkToken(token) })
		if errors.Is(err, errUnauthorized) || !n.checkToken(token) {
			time.Sleep(500 * time.Millisecond)
			writeJSON(w, http.StatusUnauthorized, apiError{errUnauthorized.Error()})
			return
		}
		if err != nil {
			log.Printf("AetherPD: receive restoration refused: %v", err)
			writeJSON(w, http.StatusConflict, map[string]string{"state": "receive_restore_unavailable"})
			return
		}
		writeJSON(w, http.StatusOK, map[string]string{"state": "receive_restored"})
	}
}
