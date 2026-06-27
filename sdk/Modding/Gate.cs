using System;
using System.Collections.Generic;

namespace Recreation.Modding;

// A reason-based gate over a shared on/off resource. Several systems may each
// have a reason to hold it closed; it is closed while any reason holds and opens
// only when the last clears, invoking the change callback just on the aggregate
// open<->closed transition. This is the pattern that lets cooperating systems
// (and mods) coordinate a single switch, fast travel or player controls, without
// fighting over it. Mods can build their own gates for their own resources.
public sealed class Gate
{
    private readonly HashSet<string> _reasons = new();
    private readonly Action<bool>? _onChanged;  // invoked with the new "open" state

    // onChanged is called false when the gate first closes and true when it fully
    // reopens; null for a gate that is only queried.
    public Gate(Action<bool>? onChanged = null) => _onChanged = onChanged;

    public bool IsClosed => _reasons.Count > 0;

    // Holds the gate closed for `reason`. Idempotent per reason.
    public void Close(string reason)
    {
        bool wasOpen = _reasons.Count == 0;
        if (_reasons.Add(reason) && wasOpen) _onChanged?.Invoke(false);
    }

    // Clears `reason`; the gate reopens only if no other reason remains.
    public void Open(string reason)
    {
        if (_reasons.Remove(reason) && _reasons.Count == 0) _onChanged?.Invoke(true);
    }

    // Drops every reason and reopens. Used on managed-world teardown.
    public void Reset()
    {
        if (_reasons.Count == 0) return;
        _reasons.Clear();
        _onChanged?.Invoke(true);
    }
}
