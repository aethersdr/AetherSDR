# Managed-owner credential rotation research

Status: unfinished research. The server endpoint now has offline component
coverage recorded in [its qualification report](./MANAGED-OWNER-KEY-SERVER-QUALIFICATION.json).
The client transaction, operator UI, packaged installation and radio qualification
remain outstanding. Managed sign-out continues to preserve the owner credential.
The [credential component report](./MANAGED-OWNER-KEY-CREDENTIAL-QUALIFICATION.json)
records conditional promotion, an installation-scoped candidate slot, injected
failure tests and real Linux keychain checks. It does not qualify the complete
client transaction or the new code on macOS/Windows.

[The original draft](./MANAGED-OWNER-ROTATION-DRAFT.json) is preserved verbatim.
It describes a two-phase server transaction; it is not an approved shipping design.

Subsequent source review identified a smaller alternative to evaluate: the client
first generates and durably saves a replacement credential, then submits a single
known-owner authenticated replacement bound to the installation identity. Atomic
server persistence would replace the owner hash and retire PD session authority.
The server implementation uses this smaller alternative, without persistent
pending-rotation state on the radio. Its offline tests do not qualify the client
credential transaction or a shipping workflow. The original two-phase proposal
remains an unimplemented research record.

Any implementation must retain both client credentials when a reply or persistence
outcome is uncertain. A successful request with the old credential during an
in-flight replacement does not prove rollback. Authentication must be rechecked
under the operation lock, active installation must be handled explicitly, and no
rotation may automatically start, arm, or transmit. The boot identity anchors,
credential-store compare-and-swap behavior, concurrent requests, and lost replies
still require verification. Recovery without an existing owner credential remains
unresolved; this research does not authorize an authentication bypass.

Resume from source baseline `22f67a1fd976e1816721087163ac396e65db1505` and the
P4/P5 requirements in the production completion plan. Actual FLEX-6300 delivery
and hardware qualification remain separate outstanding work.
