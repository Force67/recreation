using Recreation;
using Recreation.Modding;

// A minimal server-side mod. The host loads any .dll in RECREATION_MODS_DIR and
// scans it for [Mod] entry points and auto-start behaviours, exactly like the
// built-in mods. Mods default to the Server realm (host only); add
// [Realm(ModRealm.Client)] or [Realm(ModRealm.Shared)] to run on players.
[Mod("MyMod")]
public sealed class MyMod : IMod
{
    public void OnLoad()
    {
        Debug.Log($"MyMod loaded against SDK {SdkInfo.Version}");
    }
}
