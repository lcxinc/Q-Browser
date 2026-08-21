# `.qapkg` format version 1

A `.qapkg` is a deterministic ZIP archive. Every entry is a regular file with
a canonical UTF-8 forward-slash path; directories, symlinks, absolute paths,
drive/UNC paths, `.`/`..`, alternate separators, Windows device names,
case-folding collisions, and trailing dot/space components are rejected.
Archive metadata and entry order are normalized by `qbrowser-package pack`.
The verifier enforces compressed/uncompressed byte, entry-count, path, and
manifest limits before publishing extracted data.

Required members are:

```text
manifest.json
qml/Main.qml                 # or the manifest entryPoint
metadata/content.sha256
metadata/signature.ed25519
```

Other package QML and non-executable assets may be included. Native libraries,
executables, renamed PE payloads, forbidden imports, remote URL imports, and
undeclared capabilities are rejected before activation.

## Canonical authenticated bytes

The payload digest is SHA-256 over this byte stream:

1. ASCII `Q-Browser qapkg payload v1` followed by one NUL byte.
2. All regular entries except `metadata/content.sha256` and
   `metadata/signature.ed25519`, sorted by canonical path bytes.
3. For each entry: the path byte length as an unsigned 64-bit big-endian
   integer, the path bytes, the content length in the same encoding, then the
   exact content bytes.

`metadata/content.sha256` contains exactly 64 lowercase hexadecimal bytes.
The Ed25519 input is a second SHA-256 digest produced by the same encoding,
using domain `Q-Browser qapkg signed v1` plus NUL, and excluding only
`metadata/signature.ed25519`. The signature therefore commits to both payload
bytes and `metadata/content.sha256` without a circular dependency. The
signature file contains the raw 64-byte Ed25519 signature.

`manifest.json` is UTF-8 JSON conforming to
`schemas/qapkg-manifest-v1.schema.json`: unknown fields are rejected. It binds
the application ID, semantic version, exact QML entry point, compatible runtime
range, import allowlist, capability declarations, resource limits, and routes.
Important permission values are allowlists, not grants of ambient OS access:

- network: exact normalized hosts and `GET`/`POST`/`PUT`/`PATCH` methods;
- storage: `app-private` only;
- clipboard read: `user-gesture`; clipboard write: explicit boolean;
- file open: `user-brokered` only;
- process: always `false`;
- process count: exactly one; memory: at most 384 MiB.

## Reference commands

```powershell
qbrowser-package pack --source packages\pilot --output pilot.unsigned.qapkg
qbrowser-package sign --package pilot.unsigned.qapkg `
  --private-key .qbrowser-dev\signing\private.pem `
  --output com.qbrowser.pilot-1.0.0.qapkg
qbrowser-package inspect --package com.qbrowser.pilot-1.0.0.qapkg `
  --public-key .qbrowser-dev\signing\public.pem
```

`inspect` succeeds only when archive structure, manifest, content digest,
public key, and Ed25519 signature all verify. Do not infer trust from a ZIP
tool, filename, or digest alone.
