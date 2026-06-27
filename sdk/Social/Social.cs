namespace Recreation.Net;

// The social subsystem's single wiring point: parties and presence.
public static class Social
{
    // Bring social up for a role. Idempotent. Requires StateBags and Players already
    // bound, so call Platform.Boot first.
    public static void Bind(NetRole role)
    {
        Reset();
        Parties.Bind(role);
        Presence.Bind();
    }

    public static void Reset()
    {
        Parties.Reset();
        Presence.Reset();
    }
}
