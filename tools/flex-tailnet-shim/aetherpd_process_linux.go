//go:build linux

package main

import (
	"context"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"sync"
	"syscall"
	"time"
	"unsafe"
)

// A trusted launcher supplies only already-prepared, unprivileged session
// descriptors. This does not grant capture privileges or create an IQ stream.
// The guardian must be armed before this handoff and retain its API lease
// independently until the DSP exits and the radio can safely return to USB.
type aetherPDProcess struct {
	captureBatches            pdCaptureBatchStore
	testUnkeys                pdTestUnkeys
	captures                  pdCaptureStore
	calibrationIntents        pdCalibrationIntents
	receiveJournal            *pdReceiveJournal
	owner                     *os.File
	setupOwner                *os.File
	done                      chan struct{}
	once                      sync.Once
	err                       error
	receiveRecoveryIncomplete bool // Published with err by closing done.
	pid                       int
	status                    *net.UnixConn
	statusReads               chan struct{}
	statusNonce               uint64
	unkey                     *aetherPDUnkey
	lease                     *aetherPDLeaseGuard
	calibration               *net.UnixConn
	calibrationReads          chan struct{}
	calibrationNonce          uint64
}

func (p *aetherPDProcess) Stop() {
	if p != nil {
		p.testUnkeys.close()
		p.captures.close()
		p.captureBatches.close()
		p.calibrationIntents.close()
		p.once.Do(func() { _ = p.owner.Close() })
	}
}
func (p *aetherPDProcess) Wait(ctx context.Context) error {
	select {
	case <-p.done:
		return p.err
	case <-ctx.Done():
		return ctx.Err()
	}
}

func inheritedPDDescriptor(key string) (int, error) {
	text := os.Getenv(key)
	if len(text) == 0 || len(text) > 4 {
		return 0, fmt.Errorf("invalid %s descriptor", key)
	}
	for _, c := range text {
		if c < '0' || c > '9' {
			return 0, fmt.Errorf("invalid %s descriptor", key)
		}
	}
	value, err := strconv.Atoi(text)
	if err != nil || value < 3 || value > 1023 {
		return 0, fmt.Errorf("invalid %s descriptor", key)
	}
	return value, nil
}

// syscall.Getsockname normalizes both unnamed and empty abstract Unix names
// to "@". The kernel address length preserves the distinction we require.
func privatePDUnixEndpoint(fd int, peer bool) bool {
	var address syscall.RawSockaddrUnix
	size := uint32(unsafe.Sizeof(address))
	operation := uintptr(syscall.SYS_GETSOCKNAME)
	if peer {
		operation = uintptr(syscall.SYS_GETPEERNAME)
	}
	_, _, errno := syscall.Syscall(operation, uintptr(fd), uintptr(unsafe.Pointer(&address)), uintptr(unsafe.Pointer(&size)))
	return errno == 0 && address.Family == syscall.AF_UNIX && size == uint32(unsafe.Offsetof(address.Path))
}

func validatePDDescriptors(iq, api, guardian, setup int) error {
	if iq == api || iq == guardian || api == guardian || setup == iq || setup == api || setup == guardian {
		return errors.New("session descriptors must be distinct")
	}
	if typ, err := syscall.GetsockoptInt(iq, syscall.SOL_SOCKET, syscall.SO_TYPE); err != nil || typ != syscall.SOCK_DGRAM {
		return errors.New("IQ descriptor must be a datagram socket")
	}
	if !privatePDUnixEndpoint(iq, false) || !privatePDUnixEndpoint(iq, true) {
		return errors.New("IQ endpoint and peer must be unnamed private sockets")
	}
	if typ, err := syscall.GetsockoptInt(api, syscall.SOL_SOCKET, syscall.SO_TYPE); err != nil || typ != syscall.SOCK_STREAM {
		return errors.New("API descriptor must be a stream socket")
	}
	if protocol, err := syscall.GetsockoptInt(api, syscall.SOL_SOCKET, syscall.SO_PROTOCOL); err != nil || protocol != syscall.IPPROTO_TCP {
		return errors.New("API lease must use TCP")
	}
	remote, err := syscall.Getpeername(api)
	if err != nil {
		return err
	}
	radio, ok := remote.(*syscall.SockaddrInet4)
	if !ok || radio.Addr != [4]byte{127, 0, 0, 1} || radio.Port != 4992 {
		return errors.New("API lease must connect to the local radio")
	}
	for _, fd := range []int{guardian, setup} {
		var st syscall.Stat_t
		flags, _, errno := syscall.Syscall(syscall.SYS_FCNTL, uintptr(fd), syscall.F_GETFL, 0)
		if errno != 0 || int(flags)&syscall.O_ACCMODE != syscall.O_WRONLY || syscall.Fstat(fd, &st) != nil || st.Mode&syscall.S_IFMT != syscall.S_IFIFO {
			return errors.New("lifetime descriptor must be a write-only pipe")
		}
	}

	return nil
}

func startInheritedAetherPD() (*aetherPDProcess, error) {
	preparedSpec, err := parsePDPrepared(os.Getenv)
	if err != nil {
		return nil, err
	}
	setupSpec, err := parsePDSetupChildren(os.Getenv, preparedSpec.pid)
	if err != nil {
		return nil, err
	}
	if setupSpec.present && !preparedSpec.present {
		return nil, errors.New("setup children require a prepared DSP session")
	}
	ownLease, err := inheritedPDLeaseOwnership(os.Getenv("SHIM_PD_OWN_LEASE_GUARD"))
	if err != nil {
		return nil, err
	}
	transportCache, err := inheritedPDTransportCache(os.Getenv("SHIM_PD_TRANSPORT_CACHE"))
	if err != nil {
		return nil, err
	}
	keys := []string{"SHIM_PD_IQ_FD", "SHIM_PD_API_FD", "SHIM_PD_GUARD_FD", "SHIM_PD_SETUP_FD", "SHIM_PD_UNKEY_API_FD", "SHIM_PD_GUARD_READ_FD", "SHIM_PD_UNKEY_CLIENT_HANDLE", "SHIM_PD_GUI_HANDLE"}
	present := 0
	for _, key := range keys {
		if os.Getenv(key) != "" {
			present++
		}
	}
	if present == 0 {
		if transportCache || ownLease || preparedSpec.present {
			return nil, errors.New("AetherPD recovery or lease ownership requires a prepared session")
		}
		return nil, nil
	}
	if present != len(keys) {
		return nil, errors.New("incomplete AetherPD session handoff")
	}
	fd := make([]int, 6)
	for i, key := range keys[:6] {
		var err error
		fd[i], err = inheritedPDDescriptor(key)
		if err != nil {
			return nil, err
		}
	}
	if err := validatePDDescriptors(fd[0], fd[1], fd[2], fd[3]); err != nil {
		return nil, err
	}
	client, err := pdClientHandle(os.Getenv(keys[6]))
	if err != nil {
		return nil, err
	}
	gui, err := pdClientHandle(os.Getenv(keys[7]))
	if err != nil || client == gui {
		return nil, errors.New("invalid prepared GUI identity")
	}
	if err = validatePDUnkeyDescriptors(fd); err != nil {
		return nil, err
	}
	if _, _, errno := syscall.Syscall6(syscall.SYS_PRCTL, 4, 0, 0, 0, 0, 0); errno != 0 {
		return nil, fmt.Errorf("protect prepared session descriptors: %w", errno)
	}
	executable, err := os.Executable()
	if err != nil {
		return nil, err
	}
	for _, descriptor := range fd {
		syscall.CloseOnExec(descriptor)
	}
	var prepared *pdPreparedChild
	if preparedSpec.present {
		if !ownLease || !transportCache {
			return nil, errors.New("prepared DSP requires owned guardians and fixed recovery profile")
		}
		prepared, err = claimPDPreparedChild(preparedSpec, fd)
		if err != nil {
			return nil, err
		}
		defer func() {
			if prepared != nil {
				if cleanupErr := prepared.close(); cleanupErr != nil {
					log.Printf("AetherPD: prepared startup cleanup: %v", cleanupErr)
				}
			}
		}()
	}
	files := []*os.File{os.NewFile(uintptr(fd[0]), "pd-iq"), os.NewFile(uintptr(fd[1]), "pd-api"), os.NewFile(uintptr(fd[2]), "pd-guardian")}
	for _, file := range files {
		defer file.Close()
	}
	setup := os.NewFile(uintptr(fd[3]), "pd-setup-lifetime")
	setupChildren, err := claimPDSetupChildren(setupSpec)
	defer func() {
		if setupChildren != nil {
			setupChildren.expectStop()
			setup.Close()
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			defer cancel()
			if cleanupErr := setupChildren.wait(ctx); cleanupErr != nil {
				log.Printf("AetherPD: setup startup cleanup: %v", cleanupErr)
			}
		}
	}()
	if err != nil {
		return nil, err
	}
	control := os.NewFile(uintptr(fd[4]), "pd-unkey-api")
	life := os.NewFile(uintptr(fd[5]), "pd-unkey-lifetime")
	defer control.Close()
	defer life.Close()
	guard, err := startAetherPDUnkey(filepath.Join(filepath.Dir(executable), "pd-unkey-guard"), control, life, client, gui)
	if err != nil {
		setup.Close()
		return nil, err
	}
	// Capture radio-authoritative receive state before registering/selecting PD.
	// This is session memory, not persisted radio settings or permission to restore.
	baseline, err := observePDReceive(context.Background(), gui, 0, -1)
	if err == nil && (!baseline.view.ready || !baseline.view.txAllowed || baseline.view.txOwner != 0 || baseline.view.source != "" || baseline.view.reason != "" || time.Since(baseline.view.observedAt) > time.Second) {
		err = errors.New("fresh idle receive baseline required before AetherPD startup")
	}
	var process *aetherPDProcess
	var lease *aetherPDLeaseGuard
	if err == nil {
		log.Print("AetherPD: captured owned receive baseline before DSP startup")
		if ownLease {
			lease, err = startAetherPDLeaseGuard(filepath.Join(filepath.Dir(executable), "pd-lease-guard"), files[1], life, baseline.original)
			if err == nil {
				lease.receive, err = startPDReceiveGuardian(executable, life, baseline.original)
				if err == nil {
					log.Print("AetherPD: owned API lease guardian armed before DSP startup")
					log.Print("AetherPD: owned receive guardian armed before DSP startup")
				}
			}
		}
		if err == nil {
			if prepared != nil {
				process, err = attachPreparedAetherPD(prepared, setup, guard, baseline.original, lease, setupChildren)
				if err == nil {
					prepared = nil
					setupChildren = nil
				}
			} else {
				process, err = launchAetherPDWithSession(filepath.Join(filepath.Dir(executable), "aether-pd"), files, setup, transportCache, guard, baseline.original, lease)
			}
		}
	}
	if err != nil {
		if prepared != nil {
			err = errors.Join(err, prepared.close())
			prepared = nil
		}
		setup.Close()
		// Without a DSP, this is the last guardian writer in the shim. The trusted
		// launcher must close its copies after handing the session over.
		files[2].Close()
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		_ = guard.wait(ctx)
		if lease != nil {
			_ = lease.wait(ctx)
			if lease.receive != nil {
				_ = lease.receive.wait(ctx)
			}
		}
		cancel()
	}
	return process, err
}

func inheritedPDTransportCache(value string) (bool, error) {
	switch value {
	case "", "0":
		return false, nil
	case "1":
		return true, nil
	default:
		return false, errors.New("SHIM_PD_TRANSPORT_CACHE must be 0 or 1")
	}
}

func launchAetherPD(program string, files []*os.File, setup *os.File, transportCache bool, guard *aetherPDUnkey) (*aetherPDProcess, error) {
	return launchAetherPDWithBaseline(program, files, setup, transportCache, guard, pdReceiveOriginal{})
}

func launchAetherPDWithBaseline(program string, files []*os.File, setup *os.File, transportCache bool, guard *aetherPDUnkey, original pdReceiveOriginal) (*aetherPDProcess, error) {
	return launchAetherPDWithSession(program, files, setup, transportCache, guard, original, nil)
}

func launchAetherPDWithSession(program string, files []*os.File, setup *os.File, transportCache bool, guard *aetherPDUnkey, original pdReceiveOriginal, lease *aetherPDLeaseGuard) (*aetherPDProcess, error) {
	if len(files) != 3 || setup == nil || guard == nil {
		return nil, errors.New("incomplete prepared session")
	}
	// The native runtime currently builds a USB analytic signal. Receive
	// recovery supports more modes, but that does not authorize their RF use.
	if original.complete && original.settings.mode != "USB" {
		return nil, errors.New("AetherPD runtime currently supports USB only")
	}
	st, err := os.Stat(program)
	if err != nil {
		return nil, err
	}
	if !st.Mode().IsRegular() || st.Mode()&(os.ModeSetuid|os.ModeSetgid) != 0 {
		return nil, errors.New("invalid AetherPD executable")
	}
	read, write, err := os.Pipe()
	if err != nil {
		return nil, err
	}
	defer read.Close()
	pair, err := syscall.Socketpair(syscall.AF_UNIX, syscall.SOCK_DGRAM|syscall.SOCK_CLOEXEC, 0)
	if err != nil {
		write.Close()
		return nil, err
	}
	parentStatus := os.NewFile(uintptr(pair[0]), "pd-status-parent")
	childStatus := os.NewFile(uintptr(pair[1]), "pd-status-child")
	defer parentStatus.Close()
	defer childStatus.Close()
	connection, err := net.FileConn(parentStatus)
	if err != nil {
		write.Close()
		return nil, err
	}
	status, ok := connection.(*net.UnixConn)
	if !ok {
		connection.Close()
		write.Close()
		return nil, errors.New("invalid private status connection")
	}
	calibration, calibrationChild, err := newPDCalibrationChannel()
	if err != nil {
		write.Close()
		status.Close()
		return nil, err
	}
	defer calibrationChild.Close()
	cmd := exec.Command(program)
	cmd.Env = []string{"PATH=/usr/bin:/bin", "AETHER_PD_RADIO=127.0.0.1", "AETHER_PD_FEEDBACK=local", "AETHER_PD_IQ_FD=3", "AETHER_PD_API_FD=4", "AETHER_PD_OWNER_FD=6", "AETHER_PD_STATUS_FD=7", "AETHER_PD_CALIBRATION_FD=8", "AETHER_PD_TX_DEADLINE_MS=12"}
	if transportCache {
		cmd.Env = append(cmd.Env, "AETHER_PD_TRANSPORT_CACHE=1")
	}
	cmd.Env = append(cmd.Env, fmt.Sprintf("AETHER_PD_GUI_HANDLE=0x%08x", guard.owner))
	cmd.ExtraFiles = append(append([]*os.File{}, files...), read, childStatus, calibrationChild)
	// The container shim may start as root. The DSP always runs without it;
	// managed native startup additionally rejects root and retained capabilities.
	if os.Geteuid() == 0 {
		cmd.SysProcAttr = &syscall.SysProcAttr{Credential: &syscall.Credential{Uid: 78666, Gid: 20, Groups: []uint32{}}}
	}
	cmd.Stdout = io.Discard
	cmd.Stderr = io.Discard
	if err = validateAetherPDGuardians(guard, lease); err != nil {
		write.Close()
		status.Close()
		calibration.Close()
		return nil, err
	}
	if err = cmd.Start(); err != nil {
		write.Close()
		status.Close()
		calibration.Close()
		return nil, err
	}
	return monitorAetherPDProcess(cmd.Process.Pid, cmd.Wait, write, status, calibration, setup, guard, original, lease), nil
}

func validateAetherPDGuardians(guard *aetherPDUnkey, lease *aetherPDLeaseGuard) error {
	if guard == nil {
		return errors.New("unkey guardian unavailable")
	}
	select {
	case <-guard.done:
		return errors.Join(errors.New("unkey guardian ended before DSP startup"), guard.err)
	default:
	}
	if lease != nil {
		select {
		case <-lease.done:
			return errors.Join(errors.New("API lease guardian ended before DSP startup"), lease.err)
		default:
		}
		if lease.receive != nil {
			select {
			case <-lease.receive.done:
				return errors.Join(errors.New("receive guardian ended before DSP startup"), lease.receive.err)
			default:
			}
		}
	}
	return nil
}

func monitorAetherPDProcess(pid int, wait func() error, write *os.File, status, calibration *net.UnixConn, setup *os.File, guard *aetherPDUnkey, original pdReceiveOriginal, lease *aetherPDLeaseGuard) *aetherPDProcess {
	return monitorAetherPDProcessWithSetup(pid, wait, write, status, calibration, setup, guard, original, lease, nil)
}

func monitorAetherPDProcessWithSetup(pid int, wait func() error, write *os.File, status, calibration *net.UnixConn, setup *os.File, guard *aetherPDUnkey, original pdReceiveOriginal, lease *aetherPDLeaseGuard, setupChildren *pdSetupChildren) *aetherPDProcess {
	var setupFailed <-chan struct{}
	if setupChildren != nil {
		setupFailed = setupChildren.failed
	}
	var leaseDone, receiveDone <-chan struct{}
	if lease != nil {
		leaseDone = lease.done
		if lease.receive != nil {
			receiveDone = lease.receive.done
		}
	}
	p := &aetherPDProcess{calibration: calibration, calibrationReads: make(chan struct{}, 1), unkey: guard, lease: lease, owner: write, setupOwner: setup, done: make(chan struct{}), pid: pid, status: status, statusReads: make(chan struct{}, 1)}
	if original.complete {
		p.receiveJournal = &pdReceiveJournal{original: original}
	}
	dspDone := make(chan struct{})
	go func() {
		select {
		case <-setupFailed:
			p.Stop()
		case <-guard.done:
			p.Stop()
		case <-leaseDone:
			p.Stop()
		case <-receiveDone:
			p.Stop()
		case <-dspDone:
		}
	}()
	go func() {
		dspErr := wait()
		if dspErr != nil {
			log.Printf("AetherPD: owned DSP exited: %v (API lease failure: %t)", dspErr, pdReceiveAPILeaseFailed(dspErr))
		}
		if p.receiveJournal != nil {
			p.receiveJournal.recordExit(pdReceiveAPILeaseFailed(dspErr))
		}
		close(dspDone)
		p.Stop()
		setupChildren.expectStop()
		_ = p.setupOwner.Close()
		status.Close()
		calibration.Close()
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		guardErr := guard.wait(ctx)
		testUnkeyErr := p.testUnkeys.wait(ctx)
		var leaseErr, receiveErr error
		var receiveRetired bool
		if lease != nil {
			leaseErr = lease.wait(ctx)
			if lease.receive != nil {
				result := lease.receive.waitResult(ctx)
				receiveRetired, receiveErr = result.retired, result.err
			}
		}
		setupErr := setupChildren.wait(ctx)
		cancel()
		if guardErr != nil {
			log.Printf("AetherPD: owned unkey guardian exited: %v", guardErr)
		}
		if testUnkeyErr != nil {
			log.Printf("AetherPD: bounded test guardian cleanup: %v", testUnkeyErr)
		}
		if leaseErr != nil {
			log.Printf("AetherPD: owned API lease guardian exited: %v", leaseErr)
		}
		if setupErr != nil {
			log.Printf("AetherPD: setup children exited: %v", setupErr)
		}
		if receiveErr != nil {
			log.Printf("AetherPD: owned receive guardian exited: %v", receiveErr)
		}
		cleanupErr := errors.Join(dspErr, guardErr, testUnkeyErr, leaseErr, setupErr)
		// A reaped receive helper can refuse recovery without preventing DSP,
		// unkey and resource retirement. A timeout never proves it was reaped.
		p.receiveRecoveryIncomplete = receiveRetired && receiveErr != nil && cleanupErr == nil
		p.err = errors.Join(cleanupErr, receiveErr)
		close(p.done)
	}()
	return p
}

// Must match the native waveform's dedicated API-lease failure exit code.
const pdAPILeaseFailureExit = 7

func pdReceiveAPILeaseFailed(err error) bool {
	var exited *exec.ExitError
	return errors.As(err, &exited) && exited.ExitCode() == pdAPILeaseFailureExit
}

func (p *aetherPDProcess) restoreReceive(ctx context.Context, authorized func() bool) error {
	if p == nil || p.receiveJournal == nil || p.unkey == nil {
		return errors.New("receive session journal unavailable")
	}
	select {
	case <-p.done:
	default:
		return errors.New("AetherPD session is still running")
	}
	select {
	case <-p.unkey.done:
	default:
		return errors.New("unkey guardian is still running")
	}
	if p.lease != nil {
		select {
		case <-p.lease.done:
		default:
			return errors.New("API lease guardian is still running")
		}
	}
	if p.lease != nil && p.lease.receive != nil {
		select {
		case <-p.lease.receive.done:
		default:
			return errors.New("receive guardian is still running")
		}
	}
	if err := ctx.Err(); err != nil {
		return err
	}
	original, err := p.receiveJournal.beginExistingRestore()
	if err != nil {
		return err
	}
	defer p.receiveJournal.endRestore()
	if !authorized() {
		return errUnauthorized
	}
	writer, err := newPDReceiveWriter(ctx, original)
	if err != nil {
		return err
	}
	defer writer.conn.Close()
	adapter := pdReceiveRestoreIO{
		observe: func(ctx context.Context) (pdReceiveView, error) { return observePDReceiveWorld(ctx, original) },
		write: func(ctx context.Context, command string) error {
			if !authorized() {
				return errUnauthorized
			}
			return writer.command(ctx, command)
		},
		readback: func(ctx context.Context) (pdReceiveObservation, error) {
			return observePDReceive(ctx, original.guiHandle, original.panID, original.sliceID)
		},
	}
	view, err := adapter.observe(ctx)
	if err != nil {
		return err
	}
	allocatedNow := false
	if !view.slices[original.sliceID].present {
		allocation := pdReceiveAllocationIO{observe: adapter.observe, write: adapter.write,
			consume: p.receiveJournal.consumeAllocation, confirm: p.receiveJournal.confirmAllocation,
			create: func(ctx context.Context, command string) (pdReceiveCreated, error) {
				if !authorized() {
					return pdReceiveCreated{}, errUnauthorized
				}
				body, err := writer.reply(ctx, command)
				if err != nil {
					return pdReceiveCreated{}, err
				}
				id, err := pdReceiveDecimal(body)
				if err != nil || id < 0 || id >= 16 {
					return pdReceiveCreated{}, errors.New("invalid slice allocation reply")
				}
				return pdReceiveCreated{acknowledged: true, sliceID: id}, nil
			},
		}
		if err = allocatePDReceive(ctx, p.receiveJournal.recoveryRequest(true), allocation, authorized); err != nil {
			return err
		}
		allocatedNow = true
	}
	if !allocatedNow && p.receiveJournal.hasConfirmedAllocation() {
		fresh, err := adapter.observe(ctx)
		if err != nil {
			return err
		}
		if err = validateExistingPDReceiveRestore(original, fresh, time.Now()); err != nil {
			return err
		}
		if err = ctx.Err(); err != nil {
			return err
		}
		if !authorized() {
			return errUnauthorized
		}
		if err = writer.command(ctx, fmt.Sprintf("slice set %d step=%d", original.sliceID, original.recreateStep)); err != nil {
			return err
		}
	}
	if err := restoreExistingPDReceive(ctx, original, adapter, authorized); err != nil {
		return err
	}
	if p.receiveJournal.hasConfirmedAllocation() {
		observed, err := adapter.readback(ctx)
		if err != nil {
			return err
		}
		if observed.original.recreateStep != original.recreateStep || !observed.original.recreateSafe {
			return errors.New("allocated receive profile not confirmed")
		}
	}
	log.Print("AetherPD: restored modeled receive settings and verified radio readback")
	return nil
}

// The helper and leased-API guardian observe the same lifetime pipe. Merely
// accepting two unrelated pipes could leave unkey waiting after DSP death.
func validatePDUnkeyDescriptors(fd []int) error {
	if len(fd) != 6 {
		return errors.New("incomplete guardian descriptors")
	}
	seen := map[int]bool{}
	for _, n := range fd {
		if seen[n] {
			return errors.New("session descriptors must be distinct")
		}
		seen[n] = true
	}
	if !samePDLifetimePipe(fd[2], fd[5]) {
		return errors.New("unkey lifetime must read the guardian pipe")
	}

	typ, err := syscall.GetsockoptInt(fd[4], syscall.SOL_SOCKET, syscall.SO_TYPE)
	if err != nil || typ != syscall.SOCK_STREAM {
		return errors.New("unkey API must be TCP")
	}
	protocol, err := syscall.GetsockoptInt(fd[4], syscall.SOL_SOCKET, syscall.SO_PROTOCOL)
	if err != nil || protocol != syscall.IPPROTO_TCP {
		return errors.New("unkey API must be TCP")
	}
	peer, err := syscall.Getpeername(fd[4])
	if err != nil {
		return err
	}
	remote, ok := peer.(*syscall.SockaddrInet4)
	if !ok || remote.Addr != [4]byte{127, 0, 0, 1} || remote.Port != 4992 {
		return errors.New("unkey API must connect to the local radio")
	}
	first, err := syscall.Getsockname(fd[1])
	if err != nil {
		return err
	}
	second, err := syscall.Getsockname(fd[4])
	if err != nil {
		return err
	}
	a, aok := first.(*syscall.SockaddrInet4)
	b, bok := second.(*syscall.SockaddrInet4)
	if !aok || !bok || *a == *b {
		return errors.New("unkey API must be a separate connection")
	}
	return nil
}

func samePDLifetimePipe(writeFD, readFD int) bool {
	var writer, reader syscall.Stat_t
	a, _, e1 := syscall.Syscall(syscall.SYS_FCNTL, uintptr(writeFD), syscall.F_GETFL, 0)
	b, _, e2 := syscall.Syscall(syscall.SYS_FCNTL, uintptr(readFD), syscall.F_GETFL, 0)
	return e1 == 0 && e2 == 0 && int(a)&syscall.O_ACCMODE == syscall.O_WRONLY && int(b)&syscall.O_ACCMODE == syscall.O_RDONLY &&
		syscall.Fstat(writeFD, &writer) == nil && syscall.Fstat(readFD, &reader) == nil && writer.Mode&syscall.S_IFMT == syscall.S_IFIFO && reader.Mode&syscall.S_IFMT == syscall.S_IFIFO && writer.Dev == reader.Dev && writer.Ino == reader.Ino
}
