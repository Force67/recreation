namespace Recreation.Net;

// The persistence subsystem's boot seam (Bind/Reset).
public static class Persistence
{
    public static void Bind(NetRole role) => PlayerData.Bind(role);

    public static void Reset() => PlayerData.Reset();
}
