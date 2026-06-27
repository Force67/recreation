# Recreation mod starter

Copy this folder out of the engine repo and rename it. It builds a drop-in mod
against a pinned SDK version.

1. Get the SDK package. Either grab the `Recreation.Scripting.<ver>.nupkg` that
   ships with the engine, or build it yourself from a checkout:
   ```sh
   dotnet pack -c Release sdk/Recreation.Scripting.csproj
   ```
   Put the `.nupkg` in `./packages` (the path `nuget.config` points at).

2. Build the mod:
   ```sh
   dotnet build -c Release
   ```

3. Deploy it:
   ```sh
   cp bin/Release/net9.0/MyMod.dll  "$RECREATION_MODS_DIR/"
   ```
   Or place it in a server resource so it streams to players (see `MODDING.md`).

Pin the SDK `Version` in `MyMod.csproj`. Your mod keeps working across engine
updates until the next **major** SDK bump. The SDK reference is compile-time only,
so do not ship `Recreation.Scripting.dll` with your mod.
