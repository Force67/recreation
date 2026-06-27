using System;

namespace Recreation.Modding;

// Which side of a multiplayer session a mod runs on. Authoritative
// gameplay belongs on the Server; UI, local effects and "ask the server" RPC
// belong on the Client; Shared runs on both. A mod with no [Realm] is Server, so
// existing gameplay mods stay host-only and never double-simulate on a client.
public enum ModRealm
{
    Server = 0,
    Client = 1,
    Shared = 2,
}

// Declares the realm a mod (an IMod) or an auto-start behaviour runs in. The host
// starts only the mods its own role admits: a server runs Server + Shared, a
// client runs Client + Shared, single-player runs everything.
[AttributeUsage(AttributeTargets.Class, Inherited = false)]
public sealed class RealmAttribute : Attribute
{
    public ModRealm Realm { get; }

    public RealmAttribute(ModRealm realm) => Realm = realm;
}
