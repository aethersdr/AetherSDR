# Fixed capture constructor

This development helper replaces the bench harness's raw-socket constructor.
It accepts inherited UDP, private Unix datagram output and owner-pipe
descriptors, plus bounded stream ID, generation, rate and duration values.
It has no listener, command interpreter, executable path or RF control.

Only the measured FLEX-8600 profile is accepted: `eth0.10`, local address
`172.30.2.1`, FPGA source `192.168.50.100:50001`. The exact UDP destination,
stream and class filter is built inside the helper. Root is dropped before
opening capture; NET_RAW is retired before forking the linked ring worker.
The worker closes the raw descriptor before entering strict seccomp and
parsing packets. Owner-pipe hangup stops the single-threaded supervisor.

```sh
python3 tools/pd-waveform/capture/build.py --output build/pd-capture --arch arm64
python3 tools/pd-waveform/capture/build.py --output build/pd-capture --arch host --test
build/pd-capture/capture-test-host
```

This builds ordinary files. It installs no SUID bit, capability, daemon or
boot change. A trusted launcher must establish descriptor provenance and
provide temporary setup authority. It must also provision and clean up the
IQ stream, fixed ARP entry and independent API guardian. This is not a
complete installation mechanism, and its fixed profile is not a FLEX-6000
profile. Missing kernel features fail closed.

On the FLEX-8600 bench, two independent paired 14-second transmissions at
the 10 W setting retained IMD3 improvements of 16.1/23.3 dB and 18.8/19.0 dB.
Every consecutive one-second interval in each four-second measurement improved.
Peak sampled forward power was 7.34 W; no stale-IQ, deadline or send faults
occurred. Owner-pipe DSP exit, ARP restoration and API-lease cleanup passed.
These tests used the trusted development harness for startup and scheduling,
plus a separate diagnostic capture tap. They do not qualify production shim
startup during RF, continuous operation, automatic stall recovery or a 6000.
