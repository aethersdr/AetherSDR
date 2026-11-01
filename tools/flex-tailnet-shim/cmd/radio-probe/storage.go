package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"syscall"
	"time"
)

type IdentityInfo struct {
	UID        int               `json:"uid"`
	EUID       int               `json:"euid"`
	GID        int               `json:"gid"`
	Groups     []int             `json:"groups"`
	Cwd        string            `json:"cwd"`
	ParentComm string            `json:"parent_comm"`
	ParentCmd  string            `json:"parent_cmdline"`
	EnvNames   []string          `json:"env_names"`
	Env        map[string]string `json:"env"`
	Status     map[string]string `json:"process_status"`
	Cgroup     string            `json:"cgroup"`
	Limits     string            `json:"limits"`
}

// envValueShown is which environment values the report repeats. Everything
// else is listed by name only, in case the radio passes a secret.
func envValueShown(name string) bool {
	if strings.HasPrefix(name, "SSDR_") || strings.HasPrefix(name, "PROBE_") {
		return true
	}
	switch name {
	case "HOME", "PATH", "PWD", "USER", "LOGNAME", "SHELL", "LANG", "TZ", "TMPDIR":
		return true
	}
	return false
}

func identityInfo() IdentityInfo {
	id := IdentityInfo{
		UID: os.Getuid(), EUID: os.Geteuid(), GID: os.Getgid(),
		Env: map[string]string{}, Status: map[string]string{},
		Cgroup: readTrim("/proc/self/cgroup", 2048),
		Limits: readTrim("/proc/self/limits", 4096),
	}
	id.Groups, _ = os.Getgroups()
	id.Cwd, _ = os.Getwd()
	ppid := os.Getppid()
	id.ParentComm = readTrim("/proc/"+strconv.Itoa(ppid)+"/comm", 0)
	id.ParentCmd = strings.ReplaceAll(readTrim("/proc/"+strconv.Itoa(ppid)+"/cmdline", 512), "\x00", " ")
	for _, kv := range os.Environ() {
		k, v, _ := strings.Cut(kv, "=")
		id.EnvNames = append(id.EnvNames, k)
		if envValueShown(k) {
			id.Env[k] = v
		}
	}
	sort.Strings(id.EnvNames)
	for _, l := range strings.Split(readTrim("/proc/self/status", 0), "\n") {
		k, v, ok := strings.Cut(l, ":")
		if !ok {
			continue
		}
		if strings.HasPrefix(k, "Cap") || k == "NoNewPrivs" || k == "Seccomp" || k == "Uid" || k == "Gid" {
			id.Status[k] = strings.TrimSpace(v)
		}
	}
	return id
}

type StorageInfo struct {
	Mounts string     `json:"mounts"`
	Dirs   []DirCheck `json:"dirs"`
	// Store is what occupies the waveform store this probe was installed
	// in: the space a native shim has to share.
	Store []StoreEntry `json:"waveform_store"`
}

// StoreEntry is one waveform in the store and the bytes it holds.
type StoreEntry struct {
	Name  string `json:"name"`
	Bytes int64  `json:"bytes"`
	Files int    `json:"files"`
	Error string `json:"error,omitempty"`
}

// storeUsage sizes each entry of dir, read-only, stopping at a bounded
// number of files so a large store can't stall the report.
func storeUsage(dir string) []StoreEntry {
	ents, err := os.ReadDir(dir)
	if err != nil {
		return []StoreEntry{{Name: dir, Error: err.Error()}}
	}
	var out []StoreEntry
	budget := 20000
	for _, e := range ents {
		se := StoreEntry{Name: e.Name()}
		err := filepath.WalkDir(filepath.Join(dir, e.Name()), func(_ string, d os.DirEntry, err error) error {
			if err != nil {
				return nil // unreadable: count what we can
			}
			if budget--; budget < 0 {
				return filepath.SkipAll
			}
			if d.Type().IsRegular() {
				if fi, err := d.Info(); err == nil {
					se.Bytes += fi.Size()
					se.Files++
				}
			}
			return nil
		})
		if err != nil {
			se.Error = err.Error()
		}
		out = append(out, se)
	}
	return out
}

type DirCheck struct {
	Path     string `json:"path"`
	Exists   bool   `json:"exists"`
	Writable bool   `json:"writable"`
	// InspectOnly marks a directory checked with access(2), not a write.
	InspectOnly    bool        `json:"inspect_only,omitempty"`
	WriteError     string      `json:"write_error,omitempty"`
	Mount          string      `json:"mount,omitempty"`
	FSType         string      `json:"fstype,omitempty"`
	MountOpts      string      `json:"mount_opts,omitempty"`
	FreeMB         int64       `json:"free_mb"`
	TotalMB        int64       `json:"total_mb"`
	MarkerRuns     []markerRun `json:"marker_runs,omitempty"`
	SurvivedReboot bool        `json:"marker_survived_reboot"`
}

// markerName is the one file the probe leaves in a writable directory. It
// lists every run that found the directory writable, by boot, so a run
// after a reboot shows which directories kept it.
const markerName = ".aether-radio-probe-marker"

type markerRun struct {
	BootID string    `json:"boot_id"`
	Time   time.Time `json:"time"`
	Exe    string    `json:"exe"`
}

// candidateDirs is where the shim might keep its tailnet identity: its own
// install directory first, then the usual writable places.
func candidateDirs() []string {
	var dirs []string
	if exe, err := os.Executable(); err == nil {
		d := filepath.Dir(exe)
		dirs = append(dirs, d, filepath.Dir(d))
	}
	if h := os.Getenv("HOME"); h != "" {
		dirs = append(dirs, h)
	}
	dirs = append(dirs, "/var/lib/smartsdr", "/var/lib/smartsdr/waveforms", "/var/lib",
		"/home", "/data", "/storage", "/opt", "/mnt", "/media", "/root",
		"/var/tmp", "/tmp", "/dev/shm", "/run")
	seen := map[string]bool{}
	var out []string
	for _, d := range dirs {
		d = filepath.Clean(d)
		if !seen[d] {
			seen[d] = true
			out = append(out, d)
		}
	}
	return out
}

// inspectOnly is the radio's own waveform store and its parents, which
// SmartSDR enumerates: the probe asks the kernel whether it could write
// there but leaves no file, since how the loader treats a stray one is
// unknown. The probe's own install directory below them is written.
func inspectOnly(dir string) bool {
	if exe, err := os.Executable(); err == nil && dir == filepath.Dir(filepath.Dir(exe)) {
		return true
	}
	return dir == "/var/lib/smartsdr" || dir == "/var/lib/smartsdr/waveforms"
}

type mountEntry struct{ point, fstype, opts string }

func parseMounts(text string) []mountEntry {
	var out []mountEntry
	for _, l := range strings.Split(text, "\n") {
		f := strings.Fields(l)
		if len(f) >= 4 {
			out = append(out, mountEntry{unescapeMount(f[1]), f[2], f[3]})
		}
	}
	return out
}

func unescapeMount(s string) string {
	return strings.NewReplacer(`\040`, " ", `\011`, "\t", `\012`, "\n", `\134`, `\`).Replace(s)
}

// mountOf is the longest mount point containing path.
func mountOf(mounts []mountEntry, path string) (mountEntry, bool) {
	var best mountEntry
	found := false
	for _, m := range mounts {
		p := m.point
		if p == path || p == "/" || strings.HasPrefix(path, strings.TrimSuffix(p, "/")+"/") {
			if !found || len(p) >= len(best.point) {
				best, found = m, true
			}
		}
	}
	return best, found
}

// mergeMarker adds this run to a marker's runs and reports whether any
// earlier run came from another boot.
func mergeMarker(old []byte, run markerRun) (runs []markerRun, survived bool) {
	var prev struct {
		Runs []markerRun `json:"runs"`
	}
	if len(old) > 0 && json.Unmarshal(old, &prev) == nil {
		runs = prev.Runs
	}
	for _, r := range runs {
		if r.BootID != "" && r.BootID != run.BootID {
			survived = true
		}
	}
	runs = append(runs, run)
	if len(runs) > 20 {
		runs = runs[len(runs)-20:]
	}
	return runs, survived
}

func storageInfo(bootID string, dirs []string) StorageInfo {
	mountsText := readTrim("/proc/self/mounts", 16384)
	mounts := parseMounts(mountsText)
	s := StorageInfo{Mounts: mountsText}
	if exe, err := os.Executable(); err == nil {
		s.Store = storeUsage(filepath.Dir(filepath.Dir(exe)))
	}
	exe, _ := os.Executable()
	run := markerRun{BootID: bootID, Time: time.Now().UTC(), Exe: exe}
	for _, d := range dirs {
		c := DirCheck{Path: d}
		if fi, err := os.Stat(d); err == nil && fi.IsDir() {
			c.Exists = true
		}
		if m, ok := mountOf(mounts, d); ok && c.Exists {
			c.Mount, c.FSType, c.MountOpts = m.point, m.fstype, m.opts
		}
		var st syscall.Statfs_t
		if c.Exists && syscall.Statfs(d, &st) == nil {
			c.FreeMB = int64(st.Bavail) * int64(st.Bsize) / (1 << 20)
			c.TotalMB = int64(st.Blocks) * int64(st.Bsize) / (1 << 20)
		}
		if c.Exists && inspectOnly(d) {
			c.InspectOnly = true
			if err := syscall.Access(d, 2 /* W_OK */); err != nil {
				c.WriteError = err.Error()
			} else {
				c.Writable = true
			}
		} else if c.Exists {
			marker := filepath.Join(d, markerName)
			old, _ := os.ReadFile(marker)
			runs, survived := mergeMarker(old, run)
			b, _ := json.Marshal(map[string]any{"runs": runs})
			if err := writeSynced(marker, b); err != nil {
				c.WriteError = err.Error()
				// A marker from an earlier run still tells us what survived.
				if len(old) > 0 {
					c.MarkerRuns, c.SurvivedReboot = runs[:len(runs)-1], survived
				}
			} else {
				c.Writable, c.MarkerRuns, c.SurvivedReboot = true, runs, survived
			}
		}
		s.Dirs = append(s.Dirs, c)
	}
	return s
}

// writeSynced replaces path with b durably, as the shim's state would be.
func writeSynced(path string, b []byte) error {
	tmp := path + ".tmp"
	f, err := os.OpenFile(tmp, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0o600)
	if err != nil {
		return err
	}
	if _, err := f.Write(b); err != nil {
		f.Close()
		os.Remove(tmp)
		return err
	}
	if err := f.Sync(); err != nil {
		f.Close()
		os.Remove(tmp)
		return err
	}
	f.Close()
	if err := os.Rename(tmp, path); err != nil {
		os.Remove(tmp)
		return err
	}
	if dir, err := os.Open(filepath.Dir(path)); err == nil {
		dir.Sync()
		dir.Close()
	}
	return nil
}

func removeMarkers(dirs []string) []string {
	var removed []string
	for _, d := range dirs {
		p := filepath.Join(d, markerName)
		if os.Remove(p) == nil {
			removed = append(removed, p)
		}
	}
	return removed
}
