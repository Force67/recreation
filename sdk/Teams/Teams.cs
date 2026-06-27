using System;
using System.Collections.Generic;
using System.Linq;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Net;

// Raised on every machine when a player's team changes (assigned, switched, or
// dropped on leave); fires on clients too.
public readonly struct TeamChanged(Player player) : IGameEvent
{
    public Player Player { get; } = player;
}

// Teams/factions: the host defines teams, sorts players into them, scores them and
// decides who may shoot whom. Built on the state-bag layer; every machine rebuilds
// its index by observing the bags. Defining, assigning and scoring are host-only.
public static class Teams
{
    // Reserved bag keys and RPC names.
    internal const string TeamKey = "team";       // player bag: the team id (0 = none)
    private const string JoinRpc = "team:join";   // client -> host: please put me on a team

    // Defined teams and the membership reverse-index (team id -> player ids), both
    // derived from bag observation; the bags are the truth.
    private static readonly Dictionary<int, Team> Defined = new();
    private static readonly Dictionary<int, HashSet<uint>> Membership = new();
    private static readonly List<IDisposable> Subscriptions = new();

    private static NetRole _role = NetRole.Standalone;

    // Whether teammates can damage each other. Off by default.
    public static bool FriendlyFire { get; set; }

    public static NetRole Role => _role;

    // The currently defined teams (a snapshot, safe to enumerate).
    public static IReadOnlyCollection<Team> All => Defined.Values.ToList();

    // Wire teams for a role. Idempotent (re-binding resets first). Only the host
    // listens for join requests.
    public static void Bind(NetRole role)
    {
        Reset();
        _role = role;
        Subscriptions.Add(StateBags.OnAnyChange(OnBagChange));
        if (role == NetRole.Server) Rpc.On(JoinRpc, OnJoinRequest);
    }

    // Drop the derived index, subscriptions and rules. Called on teardown.
    public static void Reset()
    {
        foreach (IDisposable s in Subscriptions) s.Dispose();
        Subscriptions.Clear();
        Defined.Clear();
        Membership.Clear();
        FriendlyFire = false;
        _role = NetRole.Standalone;
    }

    // --- registry (host-authoritative) -------------------------------------------

    // Host: define (or redefine) a team in the global bag. No-op on a client and
    // for non-positive ids (0 is "none").
    public static void Define(int id, string name, uint color)
    {
        if (_role == NetRole.Client || id <= 0) return;
        StateBags.Global.Set(NameKeyOf(id), name);
        StateBags.Global.Set(ColorKeyOf(id), unchecked((int)color));
    }

    public static Team? Get(int id) => Defined.TryGetValue(id, out Team? team) ? team : null;

    // --- membership ---------------------------------------------------------------

    // Host: put a player on a team (0 to unassign). Writes the player's `team` bag;
    // no-op on a client.
    public static void Assign(uint player, int teamId)
    {
        if (_role == NetRole.Client) return;
        StateBags.Player(player).Set(TeamKey, teamId);
    }

    // The team id in a player's bag (0 when unassigned), read straight from the bag
    // so it is correct even before the index sees it.
    public static int TeamIdOf(uint id) => StateBags.Player(id).Get(TeamKey).AsInt();

    // The defined team a player belongs to, or null when unassigned or unknown here.
    public static Team? TeamOf(Player player) => Get(TeamIdOf(player.Id));

    // The players currently on a team (a snapshot); empty for team 0 or an unknown team.
    public static IEnumerable<Player> Members(int teamId)
    {
        if (teamId == 0 || !Membership.TryGetValue(teamId, out HashSet<uint>? set))
            return Array.Empty<Player>();
        return set.Select(WrapPlayer).ToList();
    }

    // Whether two players share the same real (non-zero) team; two unassigned
    // players are not teammates.
    public static bool SameTeam(Player a, Player b)
    {
        int team = TeamIdOf(a.Id);
        return team != 0 && team == TeamIdOf(b.Id);
    }

    // Client: ask the host to move the local player onto a team. On the
    // authoritative side, validate and assign directly.
    public static void RequestJoin(int teamId)
    {
        if (_role == NetRole.Client)
        {
            Rpc.Emit(JoinRpc, Value.Int(teamId));
            return;
        }
        if (Get(teamId) != null) Assign(Players.LocalId, teamId);
    }

    // --- scoring (host-authoritative) ---------------------------------------------

    // Host: add to a team's score (delta may be negative); the read-modify-write is
    // safe because only the host runs it.
    public static void AddScore(int teamId, int delta)
    {
        if (_role == NetRole.Client) return;
        StateBags.Global.Set(ScoreKeyOf(teamId), ScoreOf(teamId) + delta);
    }

    public static int ScoreOf(int teamId) => StateBags.Global.Get(ScoreKeyOf(teamId)).AsInt();

    // --- combat -------------------------------------------------------------------

    // Whether an attacker may damage a victim. Blocked only between teammates when
    // friendly fire is off.
    public static bool CanDamage(Player attacker, Player victim) =>
        FriendlyFire || !SameTeam(attacker, victim);

    // --- internals ----------------------------------------------------------------

    // Host: a client asked to join a team. Honour it only for a defined team.
    private static void OnJoinRequest(RpcEvent e)
    {
        if (e.Args.Length < 1) return;
        int teamId = e.Args[0].AsInt();
        if (Get(teamId) != null) Assign(e.Sender, teamId);
    }

    // Route each bag change to the index it feeds: global team.* keys to the
    // registry, a player's `team` key to membership.
    private static void OnBagChange(StateBagChange change)
    {
        if (change.Bag.Name == StateBags.GlobalName) OnDefinitionChange(change.Key);
        else if (TryPlayerId(change.Bag.Name, out uint id) && change.Key == TeamKey)
            OnMembershipChange(id, change);
    }

    // Rebuild a team when its definition key changes, so partial updates (name
    // before colour) converge; score keys share the prefix but are read on demand.
    private static void OnDefinitionChange(string key)
    {
        string[] parts = key.Split('.');
        if (parts.Length != 3 || parts[0] != "team") return;
        if (parts[2] != "name" && parts[2] != "color") return;
        if (!int.TryParse(parts[1], out int id) || id <= 0) return;
        RebuildTeam(id);
    }

    private static void RebuildTeam(int id)
    {
        string name = StateBags.Global.Get(NameKeyOf(id)).AsString();
        if (string.IsNullOrEmpty(name))
        {
            Defined.Remove(id);  // an undefined name means the team is gone
            return;
        }
        uint color = unchecked((uint)StateBags.Global.Get(ColorKeyOf(id)).AsInt());
        Defined[id] = new Team(id, name, color);
    }

    // A player moved teams. Use the change's previous value to know which team to
    // leave, then announce it.
    private static void OnMembershipChange(uint playerId, StateBagChange change)
    {
        int oldTeam = change.Previous.AsInt();
        int newTeam = change.Removed ? 0 : change.Value.AsInt();
        if (oldTeam != 0 && Membership.TryGetValue(oldTeam, out HashSet<uint>? oldSet))
            oldSet.Remove(playerId);
        if (newTeam != 0) SetFor(newTeam).Add(playerId);
        EventBus.Publish(new TeamChanged(WrapPlayer(playerId)));
    }

    private static HashSet<uint> SetFor(int teamId)
    {
        if (!Membership.TryGetValue(teamId, out HashSet<uint>? set))
        {
            set = new HashSet<uint>();
            Membership[teamId] = set;
        }
        return set;
    }

    // Prefer the roster's cached handle; fall back to a bare handle if the player
    // hasn't replicated here.
    private static Player WrapPlayer(uint id) => Players.Get(id) ?? new Player(id);

    private static string NameKeyOf(int id) => $"team.{id}.name";
    private static string ColorKeyOf(int id) => $"team.{id}.color";
    private static string ScoreKeyOf(int id) => $"team.{id}.score";

    // Parse "player:<id>"; false for any other bag name (global, entity:*).
    private static bool TryPlayerId(string bagName, out uint id)
    {
        id = 0;
        const string prefix = "player:";
        return bagName.StartsWith(prefix, StringComparison.Ordinal) &&
               uint.TryParse(bagName.AsSpan(prefix.Length), out id);
    }
}
