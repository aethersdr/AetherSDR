package main

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"strconv"
	"strings"
	"time"
)

// This first adapter restores settings on a surviving receiver only. It has no
// allocation, deletion, tuning, TX-selection or keying command.
type pdReceiveRestoreIO struct {
	observe  func(context.Context) (pdReceiveView, error)
	write    func(context.Context, string) error
	readback func(context.Context) (pdReceiveObservation, error)
}

var errPDReceiveDAXOccupied = errors.New("original DAX channel is occupied")

func validateExistingPDReceiveRestore(original pdReceiveOriginal, view pdReceiveView, now time.Time) error {
	if err := validatePDReceiveIdentity(original, view, now); err != nil {
		return err
	}
	slice := view.slices[original.sliceID]
	if !slice.present || slice.owner != original.guiHandle || slice.panID != original.panID || slice.frequencyHz != original.settings.frequencyHz || slice.mode != original.settings.mode || slice.daxChannel != 0 && slice.daxChannel != original.settings.daxChannel {
		return errors.New("original receive identity absent or changed")
	}
	for id, other := range view.slices {
		if id != original.sliceID && other.present && original.settings.daxChannel != 0 && other.daxChannel == original.settings.daxChannel {
			return errPDReceiveDAXOccupied
		}
	}
	return nil
}

func pdReceiveRestoreCommands(original pdReceiveOriginal) ([2]string, error) {
	var commands [2]string
	if !original.complete || original.sliceID < 0 || original.sliceID >= 16 || !pdReceiveSettingsValid(original.settings) {
		return commands, errors.New("invalid sealed receive settings")
	}
	s := original.settings
	b := func(value bool) int {
		if value {
			return 1
		}
		return 0
	}
	commands[0] = fmt.Sprintf("filt %d %d %d", original.sliceID, s.filterLow, s.filterHigh)
	commands[1] = fmt.Sprintf("slice set %d agc_mode=%s agc_threshold=%d agc_off_level=%d audio_level=%d audio_pan=%d audio_mute=%d dax=%d rit_on=%d rit_freq=%d squelch=%d squelch_level=%d nr=%d nr_level=%d nb=%d nb_level=%d anf=%d apf=%d apf_level=%d", original.sliceID, s.agcMode, s.agcThreshold, s.agcOffLevel, s.audioLevel, s.audioPan, b(s.audioMute), s.daxChannel, b(s.ritOn), s.ritHz, b(s.squelch), s.squelchLevel, b(s.nr), s.nrLevel, b(s.nb), s.nbLevel, b(s.anf), b(s.apf), s.apfLevel)
	return commands, nil
}

func restoreExistingPDReceive(ctx context.Context, original pdReceiveOriginal, adapter pdReceiveRestoreIO, authorized func() bool) error {
	commands, err := pdReceiveRestoreCommands(original)
	if err != nil {
		return err
	}
	for _, command := range commands {
		if err := ctx.Err(); err != nil {
			return err
		}
		if !authorized() {
			return errUnauthorized
		}
		view, err := adapter.observe(ctx)
		if err != nil {
			return err
		}
		if err := validateExistingPDReceiveRestore(original, view, time.Now()); err != nil {
			return err
		}
		if err := ctx.Err(); err != nil {
			return err
		}
		if !authorized() {
			return errUnauthorized
		}
		if err := adapter.write(ctx, command); err != nil {
			return err
		}
	}
	if err := ctx.Err(); err != nil {
		return err
	}
	if !authorized() {
		return errUnauthorized
	}
	observed, err := adapter.readback(ctx)
	if err != nil {
		return err
	}
	if err := validateExistingPDReceiveRestore(original, observed.view, time.Now()); err != nil {
		return err
	}
	if !observed.original.complete || observed.original.settings != original.settings {
		return errors.New("radio did not confirm restored receive settings")
	}
	return nil
}

type pdReceiveWriter struct {
	conn     net.Conn
	reader   *bufio.Reader
	sequence int
}

func newPDReceiveWriter(ctx context.Context, original pdReceiveOriginal) (*pdReceiveWriter, error) {
	if !pdReceiveClientIDValid(original.clientID) {
		return nil, errors.New("invalid original GUI identity")
	}
	var dialer net.Dialer
	conn, err := dialer.DialContext(ctx, "tcp4", "127.0.0.1:4992")
	if err != nil {
		return nil, err
	}
	deadline, ok := ctx.Deadline()
	if !ok {
		conn.Close()
		return nil, errors.New("receive restore deadline required")
	}
	if err := conn.SetDeadline(deadline); err != nil {
		conn.Close()
		return nil, err
	}
	writer := &pdReceiveWriter{conn: conn, reader: bufio.NewReaderSize(io.LimitReader(conn, 1<<20), 4098), sequence: 40}
	if err := writer.command(ctx, "client bind client_id="+original.clientID); err != nil {
		conn.Close()
		return nil, err
	}
	return writer, nil
}

func (writer *pdReceiveWriter) command(ctx context.Context, command string) error {
	_, err := writer.reply(ctx, command)
	return err
}

func (writer *pdReceiveWriter) reply(ctx context.Context, command string) (string, error) {
	if err := ctx.Err(); err != nil {
		return "", err
	}
	writer.sequence++
	line := fmt.Sprintf("C%d|%s\n", writer.sequence, command)
	if n, err := io.WriteString(writer.conn, line); err != nil {
		return "", err
	} else if n != len(line) {
		return "", io.ErrShortWrite
	}
	for frames := 0; frames < 8192; frames++ {
		if err := ctx.Err(); err != nil {
			return "", err
		}
		line, err := writer.reader.ReadSlice('\n')
		if err != nil {
			return "", err
		}
		if len(line) > 4097 {
			return "", errors.New("oversized receive command reply")
		}
		if len(line) == 0 || line[0] != 'R' {
			continue
		}
		fields := strings.SplitN(strings.TrimSpace(string(line[1:])), "|", 4)
		if len(fields) < 3 {
			return "", errors.New("malformed receive command reply")
		}
		seq, err := pdReceiveDecimal(fields[0])
		if err != nil {
			return "", errors.New("invalid receive reply sequence")
		}
		code, err := strconv.ParseUint(fields[1], 16, 32)
		if err != nil {
			return "", errors.New("invalid receive reply code")
		}
		// FlexLib Radio.ParseReply dispatches only a registered sequence.
		// An unrelated reply cannot acknowledge or reject this command.
		if seq != writer.sequence {
			continue
		}
		if code != 0 {
			return "", errors.New("receive command rejected")
		}
		return fields[2], nil
	}
	return "", errors.New("receive command reply limit")
}
