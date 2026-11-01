// Command radio-probe answers, on a radio we can't bench, the questions that
// decide whether flex-tailnet-shim can run there as a native (legacy)
// waveform: the kernel and CPU it gets, what it may write and whether that
// survives a reboot, whether it reaches the internet, and how the radio's
// API and VITA-49 streams look from inside the radio.
//
// It is installed as AetherRadioProbe.ssdr_waveform. It changes nothing on
// the radio: it reads /proc and its environment, writes one small marker
// file in each writable directory it checks (to see which survive a reboot),
// and connects to the radio API as a plain non-GUI client that sends only
// `client udpport` and `sub meter all`. It never transmits.
//
// The report is served on the LAN at http://<radio>:48994/ for 30 minutes
// after each start; POST /cleanup removes the markers and exits.
package main

import (
	"context"
	"encoding/json"
	"flag"
	"log"
	"net/http"
	"os"
	"runtime"
	"sync"
	"time"
)

const probeVersion = "0.1.2"

// Report is everything the probe found. It holds no secrets (environment
// values only for allowlisted names) and leaves out the hostname, which on a
// Flex is the radio's serial number. It does hold LAN and public addresses.
type Report struct {
	Summary   Summary        `json:"summary"`
	Probe     ProbeInfo      `json:"probe"`
	System    SystemInfo     `json:"system"`
	Identity  IdentityInfo   `json:"identity"`
	Storage   StorageInfo    `json:"storage"`
	Load      LoadInfo       `json:"load"`
	Bench     BenchInfo      `json:"bench"`
	Network   NetworkInfo    `json:"network"`
	RadioAPI  []RadioAPITest `json:"radio_api"`
	Durations map[string]int `json:"durations_ms"`
}

// Summary is the handful of answers the report exists for.
type Summary struct {
	Arch           string `json:"arch"`
	Kernel         string `json:"kernel"`
	CPUModel       string `json:"cpu_model"`
	Cores          int    `json:"cores"`
	MemTotalMB     int64  `json:"mem_total_mb"`
	MemAvailableMB int64  `json:"mem_available_mb"`
	// MemAvailableEstimated: the kernel predates MemAvailable (Linux 3.14),
	// so mem_available_mb is MemFree+Buffers+Cached, as free(1) did.
	MemAvailableEstimated bool     `json:"mem_available_estimated,omitempty"`
	CPUIdlePct            float64  `json:"cpu_idle_pct"`
	ChaChaMBps            float64  `json:"chacha20poly1305_mb_per_s"`
	UID                   int      `json:"uid"`
	WritableDirs          []string `json:"writable_dirs"`
	SurvivedReboot        []string `json:"dirs_whose_marker_survived_a_reboot"`
	BootsSeen             int      `json:"boots_seen_by_markers"`
	// RunsThisBoot counts starts in this boot from the markers: above 1,
	// the radio relaunches an exited waveform.
	RunsThisBoot        int               `json:"runs_this_boot"`
	InternetTCP         bool              `json:"internet_tcp_443"`
	InternetTLS         bool              `json:"internet_tls_verified"`
	InternetUDP         bool              `json:"internet_udp_stun"`
	RadioAPIAddr        string            `json:"radio_api_addr"`
	RadioVersion        string            `json:"radio_api_version"`
	VITASourceByAPIAddr map[string]string `json:"vita_source_by_api_addr"`
	ShimPortsFree       bool              `json:"shim_ports_free"`
}

type ProbeInfo struct {
	Version   string    `json:"version"`
	GoVersion string    `json:"go_version"`
	GOARCH    string    `json:"goarch"`
	Started   time.Time `json:"started"`
	Exe       string    `json:"exe"`
	Args      []string  `json:"args"`
}

func main() {
	listen := flag.String("listen", ":48994", "where to serve the report")
	lifetime := flag.Duration("lifetime", 30*time.Minute, "exit this long after starting")
	noAPI := flag.Bool("no-radio-api", false, "skip the radio API and VITA-49 checks")
	flag.Parse()
	log.SetFlags(0)
	log.SetPrefix("radio-probe: ")
	log.Printf("%s starting (%s/%s)", probeVersion, runtime.GOOS, runtime.GOARCH)

	// Serve at once, so a slow check can't hide the probe; the report fills
	// in when the checks finish.
	var mu sync.Mutex
	var rep *Report
	done := make(chan struct{})
	mux := http.NewServeMux()
	mux.HandleFunc("GET /{$}", func(w http.ResponseWriter, r *http.Request) {
		mu.Lock()
		cur := rep
		mu.Unlock()
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		if cur == nil {
			w.WriteHeader(http.StatusAccepted)
			w.Write([]byte(`{"status":"checks still running; reload in a minute"}` + "\n"))
			return
		}
		enc := json.NewEncoder(w)
		enc.SetIndent("", "  ")
		enc.Encode(cur)
	})
	mux.HandleFunc("POST /cleanup", func(w http.ResponseWriter, r *http.Request) {
		removed := removeMarkers(candidateDirs())
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(map[string]any{"removed": removed, "exiting": true})
		log.Printf("cleanup: removed %d markers; exiting", len(removed))
		go func() { time.Sleep(500 * time.Millisecond); close(done) }()
	})
	srv := &http.Server{Addr: *listen, Handler: mux, ReadHeaderTimeout: 5 * time.Second}
	go func() {
		if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			log.Printf("serve %s: %v", *listen, err)
		}
	}()

	r := collect(context.Background(), !*noAPI)
	mu.Lock()
	rep = r
	mu.Unlock()
	if b, err := json.Marshal(r.Summary); err == nil {
		log.Printf("summary %s", b) // reaches the radio's journal too
	}

	select {
	case <-done:
	case <-time.After(*lifetime):
		log.Printf("lifetime over; exiting")
	}
	srv.Close()
}

func collect(ctx context.Context, radioAPI bool) *Report {
	r := &Report{Durations: map[string]int{}}
	exe, _ := os.Executable()
	r.Probe = ProbeInfo{probeVersion, runtime.Version(), runtime.GOARCH, time.Now().UTC(), exe, os.Args}
	step := func(name string, f func()) {
		t := time.Now()
		f()
		r.Durations[name] = int(time.Since(t).Milliseconds())
	}
	step("system", func() { r.System = systemInfo() })
	step("identity", func() { r.Identity = identityInfo() })
	step("storage", func() { r.Storage = storageInfo(r.System.BootID, candidateDirs()) })
	step("load", func() { r.Load = loadInfo(3 * time.Second) })
	step("bench", func() { r.Bench = bench(time.Second) })
	step("network", func() { r.Network = networkInfo(ctx) })
	if radioAPI {
		step("radio_api", func() { r.RadioAPI = radioAPITests(ctx, r.Network.LocalIPv4) })
	}
	r.Summary = summarize(r)
	return r
}

func summarize(r *Report) Summary {
	s := Summary{
		Arch:          r.System.Machine,
		Kernel:        r.System.KernelRelease,
		CPUModel:      r.System.CPUModel,
		Cores:         r.System.Cores,
		MemTotalMB:    r.System.Mem["MemTotal"] / 1024,
		CPUIdlePct:    r.Load.IdlePct,
		ChaChaMBps:    r.Bench.ChaCha20Poly1305MBps,
		UID:           r.Identity.UID,
		InternetTCP:   r.Network.ControlTCP.OK,
		InternetTLS:   r.Network.ControlTLS.OK,
		ShimPortsFree: true,
		// Empty, not null, so a reader sees "none" rather than "unknown".
		WritableDirs:   []string{},
		SurvivedReboot: []string{},
	}
	s.MemAvailableMB, s.MemAvailableEstimated = memAvailableKB(r.System.Mem)
	s.MemAvailableMB /= 1024
	bootSet := map[string]bool{}
	for _, d := range r.Storage.Dirs {
		if d.Writable {
			s.WritableDirs = append(s.WritableDirs, d.Path)
		}
		if d.SurvivedReboot {
			s.SurvivedReboot = append(s.SurvivedReboot, d.Path)
		}
		for _, run := range d.MarkerRuns {
			bootSet[run.BootID] = true
		}
	}
	s.BootsSeen = len(bootSet)
	// Any writable marker counts this boot's starts; the install directory
	// may not be writable (it isn't on the 8600).
	for _, d := range r.Storage.Dirs {
		n := 0
		for _, run := range d.MarkerRuns {
			if run.BootID == r.System.BootID {
				n++
			}
		}
		s.RunsThisBoot = max(s.RunsThisBoot, n)
	}
	for _, st := range r.Network.STUN {
		if st.OK {
			s.InternetUDP = true
		}
	}
	for _, b := range r.Network.ShimPortBinds {
		if !b.OK {
			s.ShimPortsFree = false
		}
	}
	s.VITASourceByAPIAddr = map[string]string{}
	for _, t := range r.RadioAPI {
		if t.Connected && s.RadioAPIAddr == "" {
			s.RadioAPIAddr, s.RadioVersion = t.Addr, t.VersionLine
		}
		if t.VITA != nil {
			src := t.VITA.FirstFrom
			if src == "" {
				src = "none received"
			}
			s.VITASourceByAPIAddr[t.Addr] = src
		}
	}
	return s
}

// memAvailableKB is MemAvailable, or on a kernel older than 3.14 (the
// FLEX-6300 runs 2.6.37) the older free(1) estimate.
func memAvailableKB(mem map[string]int64) (int64, bool) {
	if v, ok := mem["MemAvailable"]; ok {
		return v, false
	}
	return mem["MemFree"] + mem["Buffers"] + mem["Cached"], true
}
