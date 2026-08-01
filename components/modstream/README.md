# modstream

Hides **how mod content gets from a server to a client**, FiveM style. Pure
distribution logic: it scans a mods directory into a manifest, hashes and caches
content-addressably, and mounts what arrived back into the asset Vfs. It never
touches a socket; gamenet carries the bytes.
