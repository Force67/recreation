# modstream

Hides **how mod content gets from a server to a client**, FiveM style. Pure
distribution logic: it scans a mods directory into a manifest, hashes and caches
content-addressably, and mounts what arrived back into the asset Vfs. It never
touches a socket; gamenet carries the bytes.

A resource may also declare *client scripts*: `client_scripts.txt` at the
resource root lists resource-relative managed assemblies the server asks joining
clients to load and run (see `client_scripts.h`). The catalog resolves the
declaration against the files it can actually serve — a missing or
`.streamignore`d assembly is dropped, so clients only ever hear about code they
can receive — and the offer travels as its own generation-tagged message, never
as part of the manifest. Executing it is the client's decision.
