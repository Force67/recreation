# weather

Hides **Bethesda's WTHR/CLMT records and the rule that picks one**. It maps them
onto the physical sky (cloud coverage, aerosol, sun tint) rather than replaying
the game's baked skydome.

`weather_loader` reads the records, `weather` is the stateless deterministic
climate selection and cross-fade, `director` drives it and resolves weather
sounds through the audio catalog.
