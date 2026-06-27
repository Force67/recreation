namespace Recreation.Net;

// Which side of a session this process runs as. Mirrors the host realm the engine
// hands the managed world at boot (ModDiscovery's Server/Client/Standalone).
//
//   Server     - the authoritative host. Owns shared state, validates client
//                writes, broadcasts truth.
//   Client     - a connected player. Mirrors the server's state, requests changes.
//   Standalone - single-player. Authoritative over itself, nobody to sync to.
public enum NetRole
{
    Server,
    Client,
    Standalone,
}

// Tiny IDisposable that runs an action once on dispose. Handed back by subscriptions
// so a caller can detach.
public sealed class Unsubscriber : System.IDisposable
{
    private System.Action? _undo;
    internal Unsubscriber(System.Action undo) => _undo = undo;

    public void Dispose()
    {
        System.Action? undo = _undo;
        _undo = null;
        undo?.Invoke();
    }
}
