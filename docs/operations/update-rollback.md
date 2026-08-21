# Package update and rollback

An update follows one authority-preserving transaction:

1. Copy the candidate to a new staging area.
2. Verify archive safety, manifest schema, canonical digest, Ed25519 signature,
   runtime compatibility, imports, content policy, and declared limits.
3. Commit authenticated files to a new immutable version directory.
4. Preflight and launch the exact installed entry point in an LPAC Worker.
5. Recheck the captured activation binding before admission.
6. Atomically select `current`, retain `previous`, and mark the healthy binding
   `last-known-good` only after its health window.

The Host never overwrites a verified version directory. A failed install,
preflight, launch, handshake, admission, or health window leaves or restores a
known authenticated selection. Repeated early crashes or heartbeat loss retire
the Worker, reverify the previous/LKG package, atomically recover its binding,
and launch that version. A stale concurrent operation cannot admit its Worker
or roll back a newer binding.

Operational rules:

- retain the original signed package and trusted public key used to reproduce
  an incident; never retain or request the private key;
- do not edit `candidate.qapkg`, extracted version content, or activation state;
- do not copy a version directory over another version;
- stop the Host cleanly before backup/restore of the whole package-store root;
- treat cleanup/ACL restoration failures as fatal and investigate before retry;
- confirm recovery with structured version/binding/worker-health events, not
  only with a visible page.

Development verification is covered by the Release acceptance command in
[getting started](../development/getting-started.md). Production rollout and
key ceremony are explicit MVP non-goals; design those processes separately
before using a non-development trust root.
