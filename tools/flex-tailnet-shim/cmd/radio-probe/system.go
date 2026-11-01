package main

import (
	"crypto/rand"
	"os"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"time"

	"golang.org/x/crypto/chacha20poly1305"
)

type SystemInfo struct {
	KernelRelease string           `json:"kernel_release"`
	KernelVersion string           `json:"kernel_version"`
	ProcVersion   string           `json:"proc_version"`
	Machine       string           `json:"machine"`
	OSRelease     string           `json:"os_release,omitempty"`
	CPUModel      string           `json:"cpu_model"`
	Cores         int              `json:"cores"`
	CPUInfo       string           `json:"cpuinfo"`
	Mem           map[string]int64 `json:"meminfo_kb"`
	LoadAvg       string           `json:"loadavg"`
	UptimeS       float64          `json:"uptime_s"`
	BootID        string           `json:"boot_id"`
	Clock         time.Time        `json:"clock_utc"`
}

func readTrim(path string, max int) string {
	b, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	s := strings.TrimSpace(string(b))
	if max > 0 && len(s) > max {
		s = s[:max] + "…"
	}
	return s
}

func systemInfo() SystemInfo {
	s := SystemInfo{
		KernelRelease: readTrim("/proc/sys/kernel/osrelease", 0),
		KernelVersion: readTrim("/proc/sys/kernel/version", 0),
		ProcVersion:   readTrim("/proc/version", 0),
		OSRelease:     readTrim("/etc/os-release", 2048),
		CPUInfo:       withoutSerial(readTrim("/proc/cpuinfo", 8192)),
		Cores:         runtime.NumCPU(),
		LoadAvg:       readTrim("/proc/loadavg", 0),
		BootID:        readTrim("/proc/sys/kernel/random/boot_id", 0),
		Clock:         time.Now().UTC(),
		Mem:           map[string]int64{},
	}
	s.Machine = machine()
	s.CPUModel = cpuModel(s.CPUInfo)
	for _, l := range strings.Split(readTrim("/proc/meminfo", 0), "\n") {
		k, v, ok := strings.Cut(l, ":")
		if !ok {
			continue
		}
		switch k {
		case "MemTotal", "MemFree", "MemAvailable", "Buffers", "Cached", "SwapTotal", "SwapFree", "CmaTotal", "CmaFree", "Shmem":
			n, _ := strconv.ParseInt(strings.TrimSuffix(strings.TrimSpace(v), " kB"), 10, 64)
			s.Mem[k] = n
		}
	}
	if f := strings.Fields(readTrim("/proc/uptime", 0)); len(f) > 0 {
		s.UptimeS, _ = strconv.ParseFloat(f[0], 64)
	}
	return s
}

// machine is uname -m, which the launcher passes in (Go has no portable
// uname without cgo or a per-arch Utsname layout).
func machine() string {
	if m := strings.TrimSpace(os.Getenv("PROBE_UNAME_M")); m != "" {
		return m
	}
	return runtime.GOARCH + " (GOARCH; no launcher uname)"
}

// withoutSerial drops cpuinfo's "Serial" line, which some ARM boards (the
// 8600's CM4 among them) fill with the board's unique serial.
func withoutSerial(cpuinfo string) string {
	var keep []string
	for _, l := range strings.Split(cpuinfo, "\n") {
		if k, _, _ := strings.Cut(l, ":"); strings.TrimSpace(k) == "Serial" {
			continue
		}
		keep = append(keep, l)
	}
	return strings.Join(keep, "\n")
}

// cpuModel picks the most descriptive cpuinfo line: ARM kernels vary in
// which of these they fill in.
func cpuModel(cpuinfo string) string {
	fields := map[string]string{}
	for _, l := range strings.Split(cpuinfo, "\n") {
		k, v, ok := strings.Cut(l, ":")
		if !ok {
			continue
		}
		k, v = strings.TrimSpace(k), strings.TrimSpace(v)
		if _, seen := fields[k]; !seen && v != "" {
			fields[k] = v
		}
	}
	var parts []string
	for _, k := range []string{"Hardware", "model name", "Processor", "CPU part", "CPU architecture", "BogoMIPS"} {
		if v, ok := fields[k]; ok {
			parts = append(parts, k+"="+v)
		}
	}
	return strings.Join(parts, "; ")
}

type LoadInfo struct {
	WindowS float64       `json:"window_s"`
	IdlePct float64       `json:"cpu_idle_pct"`
	Procs   int           `json:"processes"`
	Top     []ProcessLoad `json:"top_by_cpu"`
}

type ProcessLoad struct {
	PID    int     `json:"pid"`
	Comm   string  `json:"comm"`
	CPUPct float64 `json:"cpu_pct_of_one_core"`
	RSSKB  int64   `json:"rss_kb"`
}

// cpuTotals returns idle (idle+iowait) and total jiffies from /proc/stat's
// first line.
func cpuTotals(stat string) (idle, total uint64) {
	line, _, _ := strings.Cut(stat, "\n")
	f := strings.Fields(line)
	if len(f) < 5 || f[0] != "cpu" {
		return 0, 0
	}
	for i, x := range f[1:] {
		n, _ := strconv.ParseUint(x, 10, 64)
		total += n
		if i == 3 || i == 4 {
			idle += n
		}
	}
	return idle, total
}

// pidStat returns comm, utime+stime ticks and RSS pages from a
// /proc/<pid>/stat line. comm may hold spaces and parentheses.
func pidStat(line string) (comm string, ticks uint64, rssPages int64, ok bool) {
	open, close := strings.IndexByte(line, '('), strings.LastIndexByte(line, ')')
	if open < 0 || close < open {
		return "", 0, 0, false
	}
	comm = line[open+1 : close]
	f := strings.Fields(line[close+1:])
	if len(f) < 22 {
		return "", 0, 0, false
	}
	ut, _ := strconv.ParseUint(f[11], 10, 64)
	st, _ := strconv.ParseUint(f[12], 10, 64)
	rssPages, _ = strconv.ParseInt(f[21], 10, 64)
	return comm, ut + st, rssPages, true
}

func procTicks() map[int]ProcessLoad {
	out := map[int]ProcessLoad{}
	ents, _ := os.ReadDir("/proc")
	page := int64(os.Getpagesize())
	for _, e := range ents {
		pid, err := strconv.Atoi(e.Name())
		if err != nil {
			continue
		}
		b, err := os.ReadFile("/proc/" + e.Name() + "/stat")
		if err != nil {
			continue
		}
		comm, ticks, rss, ok := pidStat(string(b))
		if !ok {
			continue
		}
		out[pid] = ProcessLoad{PID: pid, Comm: comm, CPUPct: float64(ticks), RSSKB: rss * page / 1024}
	}
	return out
}

func loadInfo(window time.Duration) LoadInfo {
	idle0, total0 := cpuTotals(readTrim("/proc/stat", 0))
	p0 := procTicks()
	start := time.Now()
	time.Sleep(window)
	idle1, total1 := cpuTotals(readTrim("/proc/stat", 0))
	p1 := procTicks()
	secs := time.Since(start).Seconds()
	l := LoadInfo{WindowS: secs, Procs: len(p1)}
	if total1 > total0 {
		l.IdlePct = float64(idle1-idle0) / float64(total1-total0) * 100
	}
	for pid, b := range p1 {
		a, ok := p0[pid]
		if !ok || b.CPUPct < a.CPUPct {
			continue
		}
		// USER_HZ is 100 on Linux.
		b.CPUPct = (b.CPUPct - a.CPUPct) / 100 / secs * 100
		l.Top = append(l.Top, b)
	}
	sort.Slice(l.Top, func(i, j int) bool {
		if l.Top[i].CPUPct != l.Top[j].CPUPct {
			return l.Top[i].CPUPct > l.Top[j].CPUPct
		}
		return l.Top[i].RSSKB > l.Top[j].RSSKB
	})
	if len(l.Top) > 15 {
		l.Top = l.Top[:15]
	}
	return l
}

type BenchInfo struct {
	// WireGuard's cipher over 1420-byte packets on one core: the shim's
	// per-byte cost for every relayed stream.
	ChaCha20Poly1305MBps float64 `json:"chacha20poly1305_mb_per_s"`
	Packets              int     `json:"packets"`
}

func bench(d time.Duration) BenchInfo {
	key := make([]byte, chacha20poly1305.KeySize)
	rand.Read(key)
	aead, err := chacha20poly1305.New(key)
	if err != nil {
		return BenchInfo{}
	}
	nonce := make([]byte, aead.NonceSize())
	pt := make([]byte, 1420)
	buf := make([]byte, 0, len(pt)+aead.Overhead())
	n := 0
	start := time.Now()
	for time.Since(start) < d {
		for i := 0; i < 64; i++ {
			nonce[0]++
			buf = aead.Seal(buf[:0], nonce, pt, nil)
			n++
		}
	}
	return BenchInfo{
		ChaCha20Poly1305MBps: float64(n*len(pt)) / time.Since(start).Seconds() / 1e6,
		Packets:              n,
	}
}
