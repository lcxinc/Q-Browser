# Runtime architecture

Q-Browser separates the trusted desktop shell from downloaded application
code. The Host is the only authority that accepts navigation, verifies and
activates packages, evaluates capabilities, owns WebEngine, records telemetry,
and supervises recovery. Package QML is loaded only by an independent Worker.
The portable deployment also separates the Host/CLI dependency closure under
`host` from the immutable LPAC Worker closure under `runtime`; the sandbox
therefore never treats the Host application directory as a Worker-approved
root. WebEngine exists only in the Host closure; it is forbidden from the
Worker closure. Both closures carry their own Qt/OpenSSL/MSVC dependencies.

```text
user -> Host/Router -> trusted WebEngine surface
              |
              +-> Package Manager -> verified version store
              |                         |
              +-> LPAC Worker <--- framed IPC
                        |
                        +-> Runtime facade -> Host capability broker
```

The Host derives the application identity from its own activation record and
IPC endpoint. It does not accept an identity asserted by package messages.
IPC uses bounded JSON frames with a protocol version and request identifier;
malformed, oversized, duplicated, replayed, or out-of-order frames close or
reject the session. The Worker receives the narrow Runtime facade instead of
raw Host objects, sockets, files, or process APIs.

The route registry maps nine Pilot routes to the package Worker and
`/web/help` to the isolated WebEngine adapter. WebEngine has a dedicated
profile, permits only the configured loopback mock origin and trusted error
resource, and denies downloads, popups, external protocols, and permission
requests.

Package activation is immutable and pointer-based. A verified candidate is
installed in a new version directory, preflighted, and atomically selected.
`current`, `previous`, and `last-known-good` records are updated without
overwriting verified bytes. The worker is admitted only while its captured
activation binding still matches the selected version. Startup failure,
heartbeat loss, or a crash loop causes recovery to a reverified previous/LKG
binding. See [update and rollback](../operations/update-rollback.md).

Release publication follows the same one-way authority boundary: CMake
assembles a unique protected staging tree, mandatory signature/identity and
closure checks run there, deployment-only UI/WebEngine/update/rollback
acceptance runs there, and only then is the whole tree moved atomically to the
final name. A failure never leaves a final authoritative directory. Existing
releases are verified read-only and require a canonical acceptance attestation.

Trust boundaries are deliberately asymmetric:

- Host, compiled resources, route definitions, policy, public trust key, and
  package-store pointers are trusted.
- Signed package content is authenticated but remains untrusted code.
- Worker output and IPC input are untrusted even after authentication.
- Web content is untrusted and isolated from Worker QML.
- Development private keys are signing authority and never belong in a runtime
  deployment.

The MVP does not provide arbitrary Internet QML, a public package repository,
production key ceremony, macOS/Linux sandboxes, native package plug-ins,
browser-complete HTML compatibility, or a multi-tenant marketplace.
