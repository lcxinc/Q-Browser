# Archive security fixtures

`tst_archive.cpp` creates ZIP inputs programmatically instead of storing opaque
malicious binaries in the repository. The generated matrix covers invalid
Windows paths, duplicate canonical names, unsupported ZIP flags and entry
types, local/central metadata disagreement, CRC failure, multi-disk archives,
resource-limit boundaries, compression ratio, and zip-slip attempts.

Extraction requires an existing empty staging directory. Validation and CRC
checking complete before the first output file is created. Parent directories
are checked for Windows reparse points before each atomic `QSaveFile` commit;
if a write-phase operation fails, the operation removes everything it created
under that initially empty staging root.

The writer emits no directory entries. It sorts UTF-8 archive-name bytes,
uses a fixed compression configuration, DOS timestamp `1980-01-01 00:00:00`,
Unix creator metadata and mode `0644`, and excludes only the exact
`metadata/signature.ed25519` entry from the content-digest enumeration. Crypto
and signature verification belong to Task 5 and are deliberately absent here.
