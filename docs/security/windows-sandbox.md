# Windows Worker sandbox

The Windows launcher creates a package-specific Less Privileged AppContainer
(LPAC) process with `STARTUPINFOEX`, starts it suspended, assigns a Job Object,
then resumes it. The Job enforces kill-on-close, process count one, and the
manifest-bounded memory limit. Only the intended IPC handles are inherited.

Before a package is selected, the trusted Host constructs a move-only trust
boundary from the package-store root, sandbox-temp root, and the smallest
immutable runtime roots containing the deployed Worker and its Qt/OpenSSL/QML
closure. It holds stable handles and identities, rejects volume or broad roots,
reparse ancestors, overlaps, and Worker-writable approved roots, and repeats
the complete predicate before launch. The Worker executable must match a
captured runtime file exactly; files added after capture receive no authority.
The Release image deploys the Host/CLI and Worker closures to distinct
directories because the Host application directory is intentionally rejected
as an approved Worker runtime root. WebEngine modules, resources, and helper
process are Host-only; the Worker closure contains Qt Quick/Network/QML,
OpenSSL, and complete MSVC runtime dependencies but no WebEngine surface.

The LPAC receives non-inherited read/execute access only to captured runtime
objects, read-only/non-execute access to the selected verified package, and
read/write/non-execute access only to its per-worker temp directory. ACL
changes are transactional and restored in reverse order. Validation occurs
before mutation; checked cleanup errors are surfaced rather than hidden by
destructors.

The launcher opts out of `ALL APPLICATION PACKAGES`. Desktop Qt 6.11's
Qt6Core loader requires exactly one non-network compatibility capability,
`registryRead`; controlled tests show that zero capabilities and `lpacCom`
alone fail before the helper handshake. Internet capability SIDs and broad
capabilities are absent. A Host-created protected Host/SYSTEM-only sentinel is
the portable filesystem-denial invariant. `C:\Windows\win.ini` is diagnostic
only because Windows installations may grant it to all AppContainers.

Limitations: LPAC does not protect against a compromised Host, kernel, Qt
runtime, or inherited user authority outside the explicit grants. Registry
read compatibility increases the visible metadata surface. The sandbox cannot
make malicious UI trustworthy, prevent all denial of service, or replace
capability checks. Windows ACL inheritance and enterprise policy vary, so every
production image must rerun the real token, loader-handshake, sentinel,
network, file, process, and cleanup tests; do not weaken or skip a failed gate.
The deployment root, runtime, package, trust, manifest, and attestation paths
use protected Host/SYSTEM-only write ACLs. Verification is read-only, rejects
every symlink/junction/reparse ancestor or member, and never repairs an unsafe
image in place.
