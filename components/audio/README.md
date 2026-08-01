# audio

Hides **which Bethesda sound records map to which playable sound**. The mixer,
decoders and output backend are rx::audio; this component only knows the game's
record model.

- `sound_catalog`: SOUN/SNDR/SNDX lookup and path normalisation (Bethesda's
  ANAM strings are inconsistent about the `data\sound\` prefix).
- `ambient`: the REGN-driven ambient bed that follows the camera.
