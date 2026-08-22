# Development signing keys

This directory is retained only for public documentation compatibility. Do not
generate or store private signing material anywhere in the repository, even in
an ignored path.

Create or reuse the development authority through the guarded packaging script:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts\create-dev-package.ps1 -Configuration Release -Clean
```

It stores the private key outside the repository in the protected
`%LOCALAPPDATA%\QBrowserTask18\signing` directory and publishes only the signed
package and development public key. The authority is development-only and must
never be promoted to production or included in diagnostics.
