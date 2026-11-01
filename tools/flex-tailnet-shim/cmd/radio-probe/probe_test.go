package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestCPUTotalsCountsIdleAndIowait(t *testing.T) {
	idle, total := cpuTotals("cpu  10 0 20 300 5 0 1 0 0 0\ncpu0 1 2 3 4 5\n")
	if idle != 305 || total != 336 {
		t.Fatalf("idle %d total %d, want 305 336", idle, total)
	}
	if i, tt := cpuTotals("intr 1 2 3"); i != 0 || tt != 0 {
		t.Fatalf("a non-cpu line gave %d %d", i, tt)
	}
}

func TestPidStatSurvivesParenthesesInComm(t *testing.T) {
	// comm "a) b (c" with fields after it; utime=7 stime=8 rss=99.
	line := "42 (a) b (c) S 1 42 42 0 -1 4194560 100 0 0 0 7 8 0 0 20 0 1 0 500 1000 99 18446744073709551615"
	comm, ticks, rss, ok := pidStat(line)
	if !ok || comm != "a) b (c" || ticks != 15 || rss != 99 {
		t.Fatalf("got %q %d %d %v", comm, ticks, rss, ok)
	}
	if _, _, _, ok := pidStat("42 (x) S 1"); ok {
		t.Fatal("a short line parsed")
	}
}

func TestParseProcAddr(t *testing.T) {
	for in, want := range map[string]string{
		"0100007F:1388":                         "127.0.0.1:5000",
		"00000000:BBA0":                         "0.0.0.0:48032",
		"00000000000000000000000001000000:0016": "[::1]:22",
		"0000000000000000FFFF00000100007F:0035": "127.0.0.1:53",
	} {
		ap, ok := parseProcAddr(in)
		if !ok || ap.String() != want {
			t.Errorf("%s: got %v %v, want %s", in, ap, ok, want)
		}
	}
	if _, ok := parseProcAddr("zz:1"); ok {
		t.Error("garbage parsed")
	}
}

func TestListeningKeepsOnlyListeningTCP(t *testing.T) {
	table := "  sl  local_address rem_address   st\n" +
		"   0: 00000000:1380 00000000:0000 0A 0\n" +
		"   1: 0100007F:1380 0100007F:9C40 01 0\n"
	got := listening("tcp", table)
	if len(got) != 1 || got[0] != "tcp 0.0.0.0:4992" {
		t.Fatalf("got %v", got)
	}
	if u := listening("udp", "hdr\n   0: 00000000:1393 00000000:0000 07 0\n"); len(u) != 1 || u[0] != "udp 0.0.0.0:5011" {
		t.Fatalf("udp got %v", u)
	}
}

func TestMarkerSurvivesOnlyAcrossBoots(t *testing.T) {
	now := time.Now()
	runs, survived := mergeMarker(nil, markerRun{BootID: "a", Time: now})
	if survived || len(runs) != 1 {
		t.Fatalf("first run: %v %v", runs, survived)
	}
	b, _ := json.Marshal(map[string]any{"runs": runs})
	runs, survived = mergeMarker(b, markerRun{BootID: "a", Time: now})
	if survived || len(runs) != 2 {
		t.Fatalf("same boot counted as surviving a reboot: %v", runs)
	}
	b, _ = json.Marshal(map[string]any{"runs": runs})
	runs, survived = mergeMarker(b, markerRun{BootID: "b", Time: now})
	if !survived || len(runs) != 3 {
		t.Fatalf("new boot: %v %v", runs, survived)
	}
	if _, survived := mergeMarker([]byte("not json"), markerRun{BootID: "b"}); survived {
		t.Fatal("a corrupt marker counted as surviving")
	}
}

func TestStorageWritesAMarkerAndCleanupRemovesIt(t *testing.T) {
	// Only the test's own directory: the real candidates include /tmp and
	// /dev/shm, which a test must never write to.
	dir := t.TempDir()
	missing := filepath.Join(dir, "missing")
	s := storageInfo("boot-1", []string{dir, missing})
	var found *DirCheck
	for i := range s.Dirs {
		if s.Dirs[i].Path == dir {
			found = &s.Dirs[i]
		}
	}
	if found == nil || !found.Writable || found.SurvivedReboot {
		t.Fatalf("HOME check: %+v", found)
	}
	if _, err := os.Stat(filepath.Join(dir, markerName)); err != nil {
		t.Fatal(err)
	}
	if s.Dirs[1].Exists || s.Dirs[1].Writable {
		t.Fatalf("a missing directory was checked as present: %+v", s.Dirs[1])
	}
	s = storageInfo("boot-2", []string{dir})
	for _, d := range s.Dirs {
		if d.Path == dir && !d.SurvivedReboot {
			t.Fatalf("second boot not seen: %+v", d)
		}
	}
	if removed := removeMarkers([]string{dir}); len(removed) != 1 {
		t.Fatalf("removed %v", removed)
	}
	// The probe's own temp marker must never be left behind.
	if _, err := os.Stat(filepath.Join(dir, markerName+".tmp")); !os.IsNotExist(err) {
		t.Fatalf("temp marker left: %v", err)
	}
}

func TestMountOfPicksTheLongestPrefix(t *testing.T) {
	ms := parseMounts("/dev/root / ext4 ro 0 0\ntmpfs /var/lib tmpfs rw 0 0\n/dev/sda1 /var/lib/smartsdr ext4 rw 0 0\ntmpfs /var/lib/smartsdrx tmpfs rw 0 0\n")
	for path, want := range map[string]string{
		"/var/lib/smartsdr/waveforms": "/var/lib/smartsdr",
		"/var/lib/smartsdrx":          "/var/lib/smartsdrx",
		"/var/lib/other":              "/var/lib",
		"/tmp":                        "/",
	} {
		if m, ok := mountOf(ms, path); !ok || m.point != want {
			t.Errorf("%s: got %q, want %q", path, m.point, want)
		}
	}
}

func TestEnvValuesShownOnlyForAllowlistedNames(t *testing.T) {
	for name, want := range map[string]bool{
		"SSDR_RADIO_ADDRESS": true, "HOME": true, "PROBE_UNAME_M": true,
		"API_TOKEN": false, "SECRET": false, "SSH_AUTH_SOCK": false,
	} {
		if envValueShown(name) != want {
			t.Errorf("%s: shown=%v", name, !want)
		}
	}
}

func TestCPUModelPrefersTheDescriptiveFields(t *testing.T) {
	got := cpuModel("processor\t: 0\nmodel name\t: ARMv7 Processor rev 2 (v7l)\nBogoMIPS\t: 996.14\nHardware\t: ExampleSoC\nprocessor\t: 1\nmodel name\t: other\n")
	want := "Hardware=ExampleSoC; model name=ARMv7 Processor rev 2 (v7l); BogoMIPS=996.14"
	if got != want {
		t.Fatalf("got %q", got)
	}
}

func TestCPUInfoDropsTheBoardSerial(t *testing.T) {
	got := withoutSerial("Hardware\t: BCM2835\nRevision\t: b03141\nSerial\t\t: 10000000deadbeef\nModel\t: x")
	if got != "Hardware\t: BCM2835\nRevision\t: b03141\nModel\t: x" {
		t.Fatalf("got %q", got)
	}
}

func TestTheWaveformStoreIsNeverWritten(t *testing.T) {
	exe, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	store := filepath.Dir(filepath.Dir(exe))
	for _, d := range []string{store, "/var/lib/smartsdr", "/var/lib/smartsdr/waveforms"} {
		if !inspectOnly(d) {
			t.Errorf("%s would be written to", d)
		}
	}
	if inspectOnly(filepath.Dir(exe)) || inspectOnly("/tmp") {
		t.Error("the install directory or /tmp would not be written to")
	}
	before, _ := os.ReadDir(store)
	s := storageInfo("boot-1", []string{store})
	if !s.Dirs[0].InspectOnly {
		t.Fatalf("store check: %+v", s.Dirs[0])
	}
	if after, _ := os.ReadDir(store); len(after) != len(before) {
		t.Fatalf("the store gained a file: %d -> %d entries", len(before), len(after))
	}
}

func TestMemAvailableFallsBackOnOldKernels(t *testing.T) {
	// The FLEX-6300's 2.6.37 meminfo has no MemAvailable.
	if kb, est := memAvailableKB(map[string]int64{"MemTotal": 480436, "MemFree": 389920, "Buffers": 2636, "Cached": 48152}); kb != 440708 || !est {
		t.Fatalf("old kernel: %d %v", kb, est)
	}
	if kb, est := memAvailableKB(map[string]int64{"MemAvailable": 1494000, "MemFree": 10}); kb != 1494000 || est {
		t.Fatalf("new kernel: %d %v", kb, est)
	}
}

func TestStoreUsageSizesEachWaveform(t *testing.T) {
	dir := t.TempDir()
	os.MkdirAll(filepath.Join(dir, "A", "sub"), 0o755)
	os.WriteFile(filepath.Join(dir, "A", "x"), make([]byte, 100), 0o644)
	os.WriteFile(filepath.Join(dir, "A", "sub", "y"), make([]byte, 50), 0o644)
	os.MkdirAll(filepath.Join(dir, "B"), 0o755)
	got := storeUsage(dir)
	if len(got) != 2 || got[0].Name != "A" || got[0].Bytes != 150 || got[0].Files != 2 || got[1].Bytes != 0 {
		t.Fatalf("got %+v", got)
	}
	if e := storeUsage(filepath.Join(dir, "missing")); len(e) != 1 || e[0].Error == "" {
		t.Fatalf("missing store: %+v", e)
	}
}
