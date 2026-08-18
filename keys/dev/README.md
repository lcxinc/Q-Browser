# Development signing keys

Generate disposable Ed25519 development keys locally; never commit private
material:

```powershell
qbrowser-package keygen --private-key keys/dev/private.pem --public-key keys/dev/public.pem
```

The CLI refuses to overwrite either key and restricts the private key to the
current user. The ignore rules in this directory deny all generated files by
default and allow only this README and the ignore file.
