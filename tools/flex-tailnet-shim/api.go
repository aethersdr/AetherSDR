package main

import (
	"encoding/json"
	"errors"
	"io"
	"log"
	"net/http"
	"net/netip"
	"regexp"
	"strings"
	"time"
)

// ProvisionPort is where AetherSDR reaches the shim on the radio's LAN
// address. It is outside every port SmartSDR uses (4991-4994, 42607, 22,
// 123, 2947, 5000, 30001).
const ProvisionPort = 48992

// The provisioning API speaks plain HTTP on the LAN. The radio's own API and
// image upload are unauthenticated on the LAN, so first use is open, matching
// that trust. After that every change needs the admin token issued on first
// use. The auth key crosses the LAN in the clear but is single-use and spent
// within seconds by the tailnet login; it is never stored or logged.

var (
	authKeyRe  = regexp.MustCompile(`^tskey-[A-Za-z0-9_-]{8,200}$`)
	hostnameRe = regexp.MustCompile(`^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?$`)
	allowRe    = regexp.MustCompile(`^(tag:[a-z0-9-]{1,63}|[A-Za-z0-9._%+-]{1,64}@[A-Za-z0-9.-]{1,190}|[A-Za-z0-9._-]{1,64}@github)$`)
)

type provisionRequest struct {
	AuthKey  string   `json:"auth_key"`
	Hostname string   `json:"hostname"`
	Allow    []string `json:"allow"`
	Routes   []string `json:"routes"`
	// Optional; absent means share discovered devices (the default).
	ShareDiscovered *bool `json:"share_discovered"`
}

type routesRequest struct {
	Routes          []string `json:"routes"`
	ShareDiscovered *bool    `json:"share_discovered"`
}

func validRoutes(routes []string) error {
	if len(routes) > 16 {
		return errors.New("at most 16 shared LAN devices")
	}
	for _, r := range routes {
		if _, err := parseRoute(r); err != nil {
			return err
		}
	}
	return nil
}

type allowRequest struct {
	Allow []string `json:"allow"`
}

type apiError struct {
	Error string `json:"error"`
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}

func validAllow(allow []string) bool {
	if len(allow) > 32 {
		return false
	}
	for _, a := range allow {
		if !allowRe.MatchString(a) {
			return false
		}
	}
	return true
}

// lanOnly admits callers on private or link-local addresses, excluding the
// radio's internal container network (172.30.1.0/24), so another container
// cannot provision the shim. The tailnet side never reaches this server: it
// lives in tsnet's own network stack.
func lanOnly(next http.Handler) http.Handler {
	internal := netip.MustParsePrefix("172.30.0.0/16")
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		ap, err := netip.ParseAddrPort(r.RemoteAddr)
		if err != nil {
			writeJSON(w, http.StatusForbidden, apiError{"unknown caller"})
			return
		}
		ip := ap.Addr().Unmap()
		if internal.Contains(ip) || !(ip.IsPrivate() || ip.IsLinkLocalUnicast()) {
			log.Printf("api: refused %s %s from %s", r.Method, r.URL.Path, ip)
			writeJSON(w, http.StatusForbidden, apiError{"only LAN callers may use this API"})
			return
		}
		next.ServeHTTP(w, r)
	})
}

func bearer(r *http.Request) string {
	h := r.Header.Get("Authorization")
	if t, ok := strings.CutPrefix(h, "Bearer "); ok {
		return strings.TrimSpace(t)
	}
	return ""
}

func decode(r *http.Request, v any) error {
	dec := json.NewDecoder(io.LimitReader(r.Body, 4096))
	dec.DisallowUnknownFields()
	return dec.Decode(v)
}

// NewAPI returns the provisioning HTTP handler for n.
func NewAPI(n *Node) http.Handler {
	mux := http.NewServeMux()
	requireToken := func(w http.ResponseWriter, r *http.Request) bool {
		if n.checkToken(bearer(r)) {
			return true
		}
		time.Sleep(500 * time.Millisecond)
		writeJSON(w, http.StatusUnauthorized, apiError{"admin token required"})
		return false
	}

	mux.HandleFunc("GET /v1/status", func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, http.StatusOK, n.Status())
	})

	mux.HandleFunc("POST /v1/provision", func(w http.ResponseWriter, r *http.Request) {
		if n.Status().Provisioned && !requireToken(w, r) {
			return
		}
		var req provisionRequest
		if err := decode(r, &req); err != nil {
			writeJSON(w, http.StatusBadRequest, apiError{"malformed request"})
			return
		}
		req.AuthKey = strings.TrimSpace(req.AuthKey)
		if !authKeyRe.MatchString(req.AuthKey) {
			writeJSON(w, http.StatusBadRequest, apiError{"that doesn't look like a Tailscale auth key (tskey-…)"})
			return
		}
		if req.Hostname == "" {
			req.Hostname = "flex-radio"
		}
		if !hostnameRe.MatchString(req.Hostname) {
			writeJSON(w, http.StatusBadRequest, apiError{"hostname must be lowercase letters, digits and dashes"})
			return
		}
		if !validAllow(req.Allow) {
			writeJSON(w, http.StatusBadRequest, apiError{"allowlist entries must be tailnet logins or tag:names"})
			return
		}
		if err := validRoutes(req.Routes); err != nil {
			writeJSON(w, http.StatusBadRequest, apiError{"shared LAN devices: " + err.Error()})
			return
		}
		log.Printf("api: provisioning requested by %s (hostname %q)", r.RemoteAddr, req.Hostname)
		token, err := n.Provision(req.AuthKey, req.Hostname, req.Allow, req.Routes, req.ShareDiscovered)
		if err != nil {
			writeJSON(w, http.StatusBadGateway, apiError{"could not join the tailnet: " + err.Error()})
			return
		}
		writeJSON(w, http.StatusOK, struct {
			AdminToken string `json:"admin_token"`
			Status     Status `json:"status"`
		}{token, n.Status()})
	})

	mux.HandleFunc("PUT /v1/allow", func(w http.ResponseWriter, r *http.Request) {
		if !requireToken(w, r) {
			return
		}
		var req allowRequest
		if err := decode(r, &req); err != nil || !validAllow(req.Allow) {
			writeJSON(w, http.StatusBadRequest, apiError{"allowlist entries must be tailnet logins or tag:names"})
			return
		}
		if err := n.SetAllow(req.Allow); err != nil {
			writeJSON(w, http.StatusInternalServerError, apiError{err.Error()})
			return
		}
		writeJSON(w, http.StatusOK, n.Status())
	})

	mux.HandleFunc("PUT /v1/routes", func(w http.ResponseWriter, r *http.Request) {
		if !requireToken(w, r) {
			return
		}
		var req routesRequest
		if err := decode(r, &req); err != nil {
			writeJSON(w, http.StatusBadRequest, apiError{"malformed request"})
			return
		}
		if err := validRoutes(req.Routes); err != nil {
			writeJSON(w, http.StatusBadRequest, apiError{"shared LAN devices: " + err.Error()})
			return
		}
		if err := n.SetSharing(req.Routes, req.ShareDiscovered); err != nil {
			writeJSON(w, http.StatusInternalServerError, apiError{err.Error()})
			return
		}
		writeJSON(w, http.StatusOK, n.Status())
	})

	mux.HandleFunc("POST /v1/signout", func(w http.ResponseWriter, r *http.Request) {
		if !requireToken(w, r) {
			return
		}
		log.Printf("api: sign-out requested by %s", r.RemoteAddr)
		n.SignOut()
		writeJSON(w, http.StatusOK, n.Status())
	})
	return lanOnly(mux)
}

// ServeAPI runs the provisioning API. It starts before, and independently
// of, the tailnet, so a radio with no internet yet can still be provisioned.
func ServeAPI(n *Node, addr string) {
	srv := &http.Server{
		Addr:              addr,
		Handler:           NewAPI(n),
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       10 * time.Second,
		WriteTimeout:      120 * time.Second, // provisioning waits for the tailnet login
		MaxHeaderBytes:    8 << 10,
	}
	for {
		err := srv.ListenAndServe()
		if errors.Is(err, http.ErrServerClosed) {
			return
		}
		log.Printf("api: %v; retrying", err)
		time.Sleep(5 * time.Second)
	}
}
