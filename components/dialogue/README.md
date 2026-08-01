# dialogue

Hides **the DIAL/INFO dialogue database and where a spoken line's audio lives**.

`dialogue` builds the topic/response graph (gated by quest conditions);
`voice` resolves a response to its voice asset and reads the xWMA header for the
duration that paces lip sync and subtitles.
