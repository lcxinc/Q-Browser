# Safe diagnostics

Collect the smallest evidence needed: Q-Browser version, Windows version,
timestamp/time zone, stable error code, app ID, package version/digest (not
package contents), activation binding, Worker PID/exit code, health transition,
and the deployment `SHA-256SUMS`. Preserve event ordering.

Verify deployed bytes without modifying them:

```powershell
$deploy = (Resolve-Path build\release-deploy).Path
& 'E:\DevEnv\qt\Tools\CMake_64\bin\cmake.exe' `
  "-DQ_BROWSER_DEPLOY_MODE=VERIFY" `
  "-DQ_BROWSER_DEPLOY_DIR=$deploy" `
  "-DQ_BROWSER_PACKAGE_CLI=$deploy\host\qbrowser-package.exe" `
  "-DQ_BROWSER_PUBLIC_KEY=$deploy\trust\dev-public.pem" `
  -P cmake\Deploy.cmake
```

Inspect a package only with the matching trusted public key:

```powershell
build\release-deploy\host\qbrowser-package.exe inspect `
  --package build\release-deploy\packages\com.qbrowser.pilot-1.0.0.qapkg `
  --public-key build\release-deploy\trust\dev-public.pem
```

Never attach secrets, private keys, arbitrary package payloads, user file or
clipboard contents, raw IPC payloads, full environment blocks, access tokens,
memory dumps, or unrestricted filesystem/registry exports. Redact user names
and paths only in copies; keep originals under the incident owner's access
controls. Do not disable LPAC, add broad ACLs/capabilities, alter system ACLs,
turn off WebEngine sandboxing, edit activation files, or rerun a malicious
package to “get more logs.” `C:\Windows\win.ini` access is not a portable
sandbox verdict; use the protected sentinel and token/security tests.

If `SHA-256SUMS`, package inspection, trust-key ACL, Worker cleanup, or LKG
reverification fails, stop deployment/use, preserve read-only evidence, and
escalate with the stable error and exact command output.
