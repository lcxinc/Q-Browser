# Threat model

The protected assets are Host integrity, package signing authority, package
store activation state, user files and clipboard, capability credentials,
telemetry integrity, and availability of the desktop shell.

The adversary may supply a package, mutate it in transit or on disk, craft QML
and assets, send malformed/replayed IPC, crash or hang a Worker, race package
activation and filesystem paths, or control Web content outside the approved
origin. Local administrator, kernel, firmware, compromised Host/Qt/OpenSSL,
physical attacks, and theft of a production signing key are outside this MVP's
protection boundary.

Primary controls are:

- deterministic archives, domain-separated SHA-256 digests, Ed25519
  signatures, strict manifests, preflight checks, and immutable versions;
- stable-handle path identity, reparse/overlap/writable-root rejection,
  transactional ACL grants, LPAC, and Job Object resource limits;
- a capability broker that enforces the authenticated package manifest and
  Host policy at every operation;
- bounded, versioned, identity-bound IPC with replay rejection;
- a restricted WebEngine profile and origin interceptor;
- atomic activation, health supervision, and reverified LKG rollback;
- fail-closed reparse/identity checks, protected deployment ACLs, mandatory
  deployed-key signature verification, and atomic post-acceptance publication;
- a non-repository `%LOCALAPPDATA%\QBrowserTask18` authority whose managed
  ancestors have protected DACLs, stable directory leases, and no untrusted
  delete-child/rename capability; Release inputs are copied into that root
  before configure/build;
- structured stable diagnostics that avoid package contents, credentials,
  private keys, arbitrary filesystem data, and raw untrusted payloads.

The public key is trust configuration, not a secret; unauthorized replacement
is still security-critical, so its file and ancestors must not be writable by
the Worker or untrusted principals. A private key is signing authority and must
never be deployed, logged, archived with diagnostics, or committed.

The deployment manifest detects accidental or post-publication byte changes;
it is not a signature and cannot create trust. Verification always checks the
Pilot package with the deployed CLI and deployed public key, including exact
application ID and version, so regenerating `SHA-256SUMS` cannot bless package
tampering. Reparse points and untrusted ancestor replacement rights are rejected
from the deployment through the trusted user-profile boundary.

Residual risk includes defects in Windows, Qt, WebEngine, OpenSSL, the ZIP
implementation, or the Host policy; UI deception within the Worker surface;
denial of service within configured limits; and broad user-approved file or
clipboard disclosure. LPAC is defense in depth, not a substitute for package
verification, broker policy, IPC validation, or timely dependency patching.

Security acceptance covers wrong keys, post-signature mutations, ZIP slip and
decompression limits, executable/native content, forbidden and remote imports,
undeclared capabilities, direct network/file/process attempts, oversized or
malformed frames, replay IDs, token properties, protected-sentinel denial,
crash isolation, update rejection, and rollback.
