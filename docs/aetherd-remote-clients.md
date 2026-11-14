# aetherd remote clients (slice 1): network listener, scoped credentials, pairing

Status: specified for implementation (RFC #3849). Receive-only. This amends the
staged order of [`aetherd-headless-engine-design.md`](aetherd-headless-engine-design.md)
§10 and the scope notes of [`aetherd-control-protocol-v1-design.md`](aetherd-control-protocol-v1-design.md)
§6 and [`aetherd-stage4-client-grants.md`](aetherd-stage4-client-grants.md), as
ruled by the maintainer on 2026-11-13.

## Why

A radio that serves one controller can still serve many operators, if one
engine holds the radio and the operators are clients of the engine. On the ANAN
G2, aetherd already runs on the radio and holds it for the front panel. Slice 1
lets a client on another machine reach that aetherd over the network, with the
same control protocol the local socket carries, so that state changed by any
client is mirrored to all of them through resource events.

Protocol 2 and the HL2 both obey whichever controller spoke last. Two
controllers send the radio different band, antenna and T/R relay settings, and
the relays chatter. So the radio never gets a second controller: remote clients
talk to aetherd, never to the radio.

## Rulings this document records (2026-11-13)

| Question | Ruling |
|---|---|
| Transport | WebSocket. TLS (`wss://`) on the LAN, plus a plain listener only on an explicitly chosen WireGuard (tailnet) address. No plain LAN listener. |
| Certificate | Self-signed, made by the host (on the G2, the kiosk package) and loaded by aetherd from files. The client pins its SHA-256 fingerprint at pairing. |
| Credentials | aetherd's own vault. A credential now carries scopes, and on the network the scope decides the grants. |
| Enrollment | Pairing: the radio's screen shows a one-time code; the client exchanges it once, over TLS, for its credential. |
| Order | Ahead of the RFC's original sequence, receive-only. No transmit method is reachable over the network until the Stage 4 arbiter work admits it there. |
| Shared mutation | Remote clients may create and close slices and panadapters (a later slice adds those methods; none exist yet). |

## The pieces

### 1. One control engine, several listeners

`ControlService`, its `ControlResourceStore` and `TransmitControlService` move
out of `LocalControlServer` into a `ControlEngine` that aetherd's `main()` owns.
The local listener and the network listener both serve sessions of that one
engine, so they share one resource store, one radio generation and one TX
client registry. `ReserveEndpoint` ordering is kept: no listener accepts a
session before `startServing()`. This is a refactor with no behavior change for
the local socket, and lands as its own commit.

### 2. Network sessions start unauthenticated

A session from the network listener is constructed `Unauthenticated`. Its
`hello` must carry `params.auth = {scheme:"bearer", token}`. The verified
principal's **scope** sets the session's grants:

| Scope | Grants | Local socket | Network |
|---|---|---|---|
| `client` (existing) | none of its own; TX client identity | yes | refused (`auth.invalid`) |
| `grant-admin` (existing) | TX grant administration | yes | refused |
| `remote-observe` (new) | observe | refused | yes |
| `remote-control` (new) | observe, control | refused | yes |

Network sessions are never attached to `TransmitControlService`. They are not
advertised `tx.*` or `txAdmin.*`, and those methods are refused with
`auth.grant_denied` even if a grant exists and `--allow-local-tx` is set.

The vault record format goes to version 2. The role byte gains the two remote
scopes, and remote records store only the token's SHA-256 digest, since the
daemon never needs the secret back. Each v2 record also carries the client's
name in 32 zero-padded bytes of UTF-8 (empty for a local role). Version 2 is
written only while a remote record exists; version-1 vaults read unchanged.
`--list-credentials` prints the role and, for a remote record, the name.

### 3. Pairing

- **`pairing.begin`** (new). Allowed only on the local socket, with the `control`
  grant. It returns `{code, expiresInMs, certificateSha256}`. One pairing can be
  open at a time; a new `begin` replaces it. The code expires after 5 minutes.
- **The code** is ten digits shown as `123-456-7890`: six random digits, then four
  check digits derived from the six and the certificate fingerprint: the first
  four bytes of SHA-256(six digits ‖ fingerprint), read as a big-endian
  unsigned integer, mod 10 000, zero-padded. The six digits and the fingerprint
  (64 lowercase hex characters) are hashed as ASCII text. The client checks
  them against the certificate it actually sees **before** it sends anything, so
  a man in the middle with another certificate is caught (1 in 10 000) without
  the operator comparing fingerprints by hand.
- **`pairing.complete {code, clientName}`** (new). Allowed only on the TLS
  listener (the check digits pin its certificate; the tailnet listener refuses
  it with `capability.unavailable`), before `hello`, with no `sessionId`, once
  per connection. `clientName` is 1 to 32 bytes of UTF-8 with no control
  characters, trimmed. A correct code mints a `remote-control` credential,
  stores it in the vault, and returns `{credentialId, token}` exactly once;
  `token` is 64 lowercase hex characters, presented at `hello` as
  `{scheme:"bearer", token}`. Every answer closes the connection, so a client
  reconnects with the token. Five wrong codes end the open pairing.
- **Errors:** a wrong code is `auth.invalid`; no open or an expired pairing is
  `capability.unavailable`; malformed parameters are `request.invalid_params`
  (they do not use up an attempt); a credential the vault would not store is
  `engine.failed`.
- **The reply is deferred** while the credential is written: the service
  returns no reply at once, the connection reads no further input, and the
  answer is written when the vault write completes. The handshake deadline is
  extended once, to 30 s, for that write. If the client disconnects first, the
  stored credential is an orphan: aetherd logs its id for removal.
- **Adding a credential retires no one.** A paired record joins the live
  verifier directly; a full reload would end every live session, the radio's
  own panel included.
- **Revocation (slice 1):** offline only. `--remove-credential <id>` and
  `--list-credentials` take the same authority reservation the serving daemon
  holds, so the daemon is stopped, the credential removed, and the daemon
  started again; on the G2 that briefly stops the panel's engine too. A later
  slice adds local-socket methods to list and revoke paired clients at run
  time, which the radio's pairing screen will use.
- **Rollback:** once a client has paired, the vault is version 2, and an
  aetherd that predates it refuses to load the authority (it fails closed).
  Going back to an older engine means removing the remote records first.

### 4. The network listener

- **TLS:** `--remote-listen <address>:<port>` with `--tls-cert <file>` and
  `--tls-key <file>`, in Qt's secure WebSocket mode. A wildcard address
  (`0.0.0.0`, `::`) is refused at startup.
- **Tailnet:** `--remote-listen-tailnet <address>:<port>`, plain WebSocket. It is
  refused unless the address belongs to a WireGuard interface (a `tailscale*` or
  `wg*` interface) at startup. If that address isn't up, aetherd does not
  listen, and never falls back to another address.
- **Framing:** one JSON object per text frame, no trailing newline. Binary frames
  close the connection. Incoming frame and message sizes are capped at the
  codec's 256 KiB.
- **Budget:** at most 4 network sessions, counted apart from the local socket's
  8, so LAN clients can never lock the radio's own panel out.
- **Rate:** the same per-session limits as the local socket. A connection that
  has not completed `hello` or `pairing.complete` within 5 s is closed.
- **Build:** only with `HAVE_WEBSOCKETS`. Without it, the options are refused
  with a message.

### 5. What slice 1 does not do

- **No spectrum, waterfall or audio** over the network: that's RFC step 4.
- **No desktop client:** that's slice 2, an `IRadioBackend` whose radio is a
  remote aetherd.
- **No transmit** for network sessions.
- **No slice or panadapter creation methods yet.**

## Tests

The listener is a thin adapter; authorization, scopes and pairing are tested
without sockets:
- scope-to-grant mapping, both directions;
- remote sessions never attached to TX, and refused even with a grant;
- `pairing.begin` refused off the local socket;
- code expiry, single use and the attempt limit;
- check-digit derivation;
- vault v1 → v2 reading;
- wildcard and non-WireGuard addresses refused.

A loopback `wss://` test with a throwaway certificate covers the real transport:
the pin, pairing and a mirrored change between a local and a network session.
It owns a socket and is registered as such.
