# Safe diagnostics

Collect the smallest evidence needed: Q-Browser version, Windows version,
timestamp/time zone, stable error code, app ID, package version/digest (not
package contents), activation binding, Worker PID/exit code, health transition,
and the deployment `SHA-256SUMS`. Preserve event ordering.

Verify deployed bytes without modifying them:

```powershell
$task18Root = Join-Path ([Environment]::GetFolderPath(
  [Environment+SpecialFolder]::LocalApplicationData)) 'QBrowserTask18'
$deploy = (Resolve-Path (Join-Path $task18Root 'release-deploy')).Path
& powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\scripts\build-release.ps1
if ($LASTEXITCODE -ne 0) { throw 'Read-only deployment verification failed.' }
```

This no-`-Clean` form uses the protected verifier copied into the trusted
Task18 control directory and proves deployment hashes, raw ACL descriptors,
identity, signature/app/version, and the prior acceptance attestation did not
change. Do not substitute `cmake\Deploy.cmake` from a mutable checkout.

Inspect a package only with the matching trusted public key:

```powershell
& "$deploy\host\qbrowser-package.exe" inspect `
  --package "$deploy\packages\com.qbrowser.pilot-1.0.0.qapkg" `
  --public-key "$deploy\trust\dev-public.pem"
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
