# gamenet

Hides **recreation's game payloads on the wire**. rx::net owns the transport,
join handshake, per-peer entity replication and streaming bubbles; this adds the
game layer: quest / war-map / actor / world-command codecs, dialogue, stage and
activation routing, and mod asset streaming.
