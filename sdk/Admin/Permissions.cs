using System;
using System.Collections.Generic;

namespace Recreation.Net;

// The authoritative permission model for privileged actions, an ACE (Access Control
// Entry) system. It answers one question: may this principal act on this object?
//
//   * Principals are strings: a peer is "player.5", a role is "group.admin".
//   * Objects are dotted names: "command.kick", "command.announce". An ACE object
//     may be a wildcard ("command.*", or "*" for everything).
//   * A principal joins groups and inherits their ACEs, resolved transitively.
//
// Lives on the server. On a standalone host the local player is treated as a
// superadmin (see IsPlayerAllowed).
public static class Permissions
{
    // principal -> (object -> allow). One entry per (principal, object) pair; the
    // bool is allow (true) or deny (false).
    private static readonly Dictionary<string, Dictionary<string, bool>> Aces = new();

    // principal -> the groups it belongs to. A group is itself a principal, so the
    // graph can nest (player.5 -> group.mod -> group.admin).
    private static readonly Dictionary<string, HashSet<string>> Groups = new();

    // The canonical principal string for a peer id.
    public static string PlayerPrincipal(uint id) => $"player.{id}";

    // Grant or revoke: record that principal is allowed (or denied) on object.
    // Re-adding the same pair replaces the previous decision.
    public static void AddAce(string principal, string obj, bool allow)
    {
        if (!Aces.TryGetValue(principal, out Dictionary<string, bool>? table))
        {
            table = new Dictionary<string, bool>();
            Aces[principal] = table;
        }
        table[obj] = allow;
    }

    // Drop a specific ACE. Other entries for the principal are untouched.
    public static void RemoveAce(string principal, string obj)
    {
        if (Aces.TryGetValue(principal, out Dictionary<string, bool>? table))
            table.Remove(obj);
    }

    // Make principal a member of group, so it inherits the group's ACEs.
    public static void AddPrincipalToGroup(string principal, string group)
    {
        if (!Groups.TryGetValue(principal, out HashSet<string>? set))
        {
            set = new HashSet<string>();
            Groups[principal] = set;
        }
        set.Add(group);
    }

    public static void RemovePrincipalFromGroup(string principal, string group)
    {
        if (Groups.TryGetValue(principal, out HashSet<string>? set))
            set.Remove(group);
    }

    // The core decision. True when some matching ACE (on the principal or an inherited
    // group) allows the object and none denies it. Deny always wins; an object with no
    // matching ACE is denied by default.
    public static bool IsAllowed(string principal, string obj)
    {
        HashSet<string> principals = new();
        Collect(principal, principals);

        bool allowed = false;
        foreach (string p in principals)
        {
            if (!Aces.TryGetValue(p, out Dictionary<string, bool>? table)) continue;
            foreach (KeyValuePair<string, bool> ace in table)
            {
                if (!Matches(ace.Key, obj)) continue;
                if (!ace.Value) return false;  // a deny anywhere overrides every allow
                allowed = true;
            }
        }
        return allowed;
    }

    // Resolve a peer through its groups. On a standalone host the local player is
    // implicitly allowed; otherwise the server's ACE table decides.
    public static bool IsPlayerAllowed(uint id, string obj) =>
        Platform.IsStandalone || IsAllowed(PlayerPrincipal(id), obj);

    // Gather a principal and every group it transitively belongs to. The visited
    // set both accumulates the result and guards against membership cycles.
    private static void Collect(string principal, HashSet<string> visited)
    {
        if (!visited.Add(principal)) return;  // already seen: stop, breaking any cycle
        if (Groups.TryGetValue(principal, out HashSet<string>? groups))
            foreach (string g in groups) Collect(g, visited);
    }

    // Does an ACE object pattern cover the queried object? Exact match, the catch-all
    // "*", or a trailing ".*" prefix wildcard ("command.*" covers "command.kick").
    private static bool Matches(string acePattern, string obj)
    {
        if (acePattern == obj || acePattern == "*") return true;
        if (acePattern.EndsWith(".*", StringComparison.Ordinal))
            return obj.StartsWith(acePattern[..^1], StringComparison.Ordinal);
        return false;
    }

    // Forget every ACE and group. Called by Admin.Reset on session teardown.
    internal static void Reset()
    {
        Aces.Clear();
        Groups.Clear();
    }
}
