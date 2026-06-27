using System;
using System.Collections.Generic;
using System.Linq;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Net;

// The party system: server-authoritative groups of players. Membership lives in
// each member's replicated player state bag (key `party`, 0 = none) and the registry
// on every machine is rebuilt by observing those changes. The leader rides each
// member's `party_leader` key. Only the server mutates the bags; a client asks via
// the `social:req_*` RPCs, which the server validates (only a leader may invite/kick).
public static class Parties
{
    // Player-bag keys. Namespaced so they never collide with a mod's own keys.
    private const string PartyKey = "party";          // int: the party id (0 = none)
    private const string LeaderKey = "party_leader";  // int: the leading player's id

    // RPC names. Clients forward requests; the server notifies an invitee.
    private const string ReqInviteRpc = "social:req_invite";  // client -> server: invite a player
    private const string ReqAcceptRpc = "social:req_accept";  // client -> server: accept my invite
    private const string ReqLeaveRpc = "social:req_leave";    // client -> server: leave my party
    private const string InviteRpc = "social:invite";         // server -> invitee: you're invited

    // The registry, derived on every machine from the replicated player bags.
    // MemberOf is the reverse index for O(1) Of().
    private static readonly Dictionary<int, HashSet<uint>> Groups = new();
    private static readonly Dictionary<int, uint> Leaders = new();
    private static readonly Dictionary<uint, int> MemberOf = new();
    private static readonly List<IDisposable> Subscriptions = new();

    // Server-only: the next party id to hand out, and outstanding invites
    // (invitee -> the party they were invited to).
    private static int _nextId = 1;
    private static readonly Dictionary<uint, int> PendingInvites = new();

    private static NetRole _role = NetRole.Standalone;

    // Where this process owns the truth (host or single-player). A client never
    // mutates the bags directly; it uses the Request* forwarders.
    private static bool Authoritative => _role != NetRole.Client;

    // Wire the system for a role. Every machine observes bag changes; only the server
    // answers request RPCs, only a client takes invite notifications. Idempotent.
    internal static void Bind(NetRole role)
    {
        Reset();
        _role = role;
        Subscriptions.Add(StateBags.OnAnyChange(OnBagChange));
        switch (role)
        {
            case NetRole.Server:
                Rpc.On(ReqInviteRpc, e => Invite(e.Sender, e.Args.Length > 0 ? (uint)e.Args[0].AsInt() : 0));
                Rpc.On(ReqAcceptRpc, e => Accept(e.Sender));
                Rpc.On(ReqLeaveRpc, e => Leave(e.Sender));
                break;
            case NetRole.Client:
                Rpc.On(InviteRpc, OnInvite);
                break;
            case NetRole.Standalone:
                break;  // local-only parties, no RPC
        }
    }

    internal static void Reset()
    {
        foreach (IDisposable s in Subscriptions) s.Dispose();
        Subscriptions.Clear();
        Groups.Clear();
        Leaders.Clear();
        MemberOf.Clear();
        PendingInvites.Clear();
        _nextId = 1;
        _role = NetRole.Standalone;
    }

    // --- queries (any machine) ---

    public static Party? Of(uint player) =>
        MemberOf.TryGetValue(player, out int party) ? ById(party) : null;

    public static Party? ById(int partyId)
    {
        if (!Groups.TryGetValue(partyId, out HashSet<uint>? members)) return null;
        uint leader = Leaders.TryGetValue(partyId, out uint l) ? l : 0;
        return new Party(partyId, leader, members.ToList());  // snapshot copy
    }

    public static IReadOnlyCollection<uint> Members(int partyId) =>
        Groups.TryGetValue(partyId, out HashSet<uint>? members) ? members.ToList() : Array.Empty<uint>();

    public static bool IsLeader(uint player) =>
        MemberOf.TryGetValue(player, out int party) &&
        Leaders.TryGetValue(party, out uint leader) && leader == player;

    // --- server authority ---

    // Open a new party led by `leader` (its only member to start). Returns the new
    // party id, or 0 on a client (which has no authority to create one).
    public static int Create(uint leader)
    {
        if (!Authoritative) return 0;
        int id = _nextId++;
        SetMembership(leader, id, leader);
        return id;
    }

    // Record a pending invite and notify the invitee. Only a leader may invite.
    public static void Invite(uint inviter, uint invitee)
    {
        if (!Authoritative || !IsLeader(inviter)) return;
        int party = MemberOf[inviter];
        PendingInvites[invitee] = party;
        Rpc.ToClient(invitee, InviteRpc, Value.Int(party), Value.Int((int)inviter));
    }

    // Join the party the player was invited to, if the invite still stands and the
    // party still exists. Returns whether the join happened.
    public static bool Accept(uint invitee)
    {
        if (!Authoritative) return false;
        if (!PendingInvites.Remove(invitee, out int party)) return false;
        if (!Groups.ContainsKey(party)) return false;  // party disbanded before accept
        uint leader = Leaders.TryGetValue(party, out uint l) ? l : 0;
        SetMembership(invitee, party, leader);
        return true;
    }

    // Leave a party. A leader leaving disbands the whole party; a member leaving
    // just drops itself (no promotion policy).
    public static void Leave(uint player)
    {
        if (!Authoritative || !MemberOf.TryGetValue(player, out int party)) return;
        if (IsLeader(player)) Disband(party);
        else ClearMembership(player);
    }

    // Remove a player from the leader's own party. Only a leader may kick; a leader
    // disbands via Leave rather than kicking itself.
    public static void Kick(uint leader, uint target)
    {
        if (!Authoritative || !IsLeader(leader) || target == leader) return;
        if (MemberOf.TryGetValue(target, out int party) && party == MemberOf[leader])
            ClearMembership(target);
    }

    // --- client request forwarders (no-ops with no RPC backend / on the server) ---

    public static void RequestInvite(uint invitee) => Rpc.Emit(ReqInviteRpc, Value.Int((int)invitee));

    public static void RequestAccept() => Rpc.Emit(ReqAcceptRpc);

    public static void RequestLeave() => Rpc.Emit(ReqLeaveRpc);

    // --- internals ---

    // Server: write the authoritative membership bags. Both keys replicate, so every
    // machine's registry updates through OnBagChange.
    private static void SetMembership(uint player, int party, uint leader)
    {
        StateBag bag = StateBags.Player(player);
        bag.Set(PartyKey, party);
        bag.Set(LeaderKey, (int)leader);
    }

    private static void Disband(int party)
    {
        if (Groups.TryGetValue(party, out HashSet<uint>? members))
            foreach (uint member in members.ToArray())  // snapshot: ClearMembership mutates the set
                ClearMembership(member);
        foreach (uint invitee in PendingInvites.Where(kv => kv.Value == party).Select(kv => kv.Key).ToArray())
            PendingInvites.Remove(invitee);
    }

    private static void ClearMembership(uint player)
    {
        StateBag bag = StateBags.Player(player);
        bag.Remove(PartyKey);   // drives RemoveMember through OnBagChange
        bag.Remove(LeaderKey);
    }

    // Keeps the registry in step with replicated bags. Runs on every machine.
    private static void OnBagChange(StateBagChange change)
    {
        if (!TryPlayerId(change.Bag.Name, out uint id)) return;
        switch (change.Key)
        {
            case PartyKey:
                int previous = change.Previous.IsNone ? 0 : change.Previous.AsInt();
                int current = change.Removed ? 0 : change.Value.AsInt();
                UpdateMembership(id, previous, current);
                break;
            case LeaderKey:
                UpdateLeader(id, change);
                break;
        }
    }

    private static void UpdateMembership(uint id, int oldParty, int newParty)
    {
        if (oldParty == newParty) return;
        if (oldParty != 0) RemoveMember(id, oldParty);
        if (newParty != 0) AddMember(id, newParty);
    }

    private static void AddMember(uint id, int party)
    {
        if (!Groups.TryGetValue(party, out HashSet<uint>? members))
        {
            members = new HashSet<uint>();
            Groups[party] = members;
        }
        members.Add(id);
        MemberOf[id] = party;
        Leaders.TryAdd(party, 0);  // filled in by the party_leader key
        EventBus.Publish(new PartyChanged(party));
    }

    private static void RemoveMember(uint id, int party)
    {
        MemberOf.Remove(id);
        if (Groups.TryGetValue(party, out HashSet<uint>? members))
        {
            members.Remove(id);
            if (members.Count == 0)
            {
                Groups.Remove(party);
                Leaders.Remove(party);
            }
            else if (Leaders.TryGetValue(party, out uint leader) && leader == id)
            {
                Leaders[party] = 0;  // leader gone but members linger: clear the stale owner
            }
        }
        EventBus.Publish(new PartyChanged(party));
    }

    private static void UpdateLeader(uint id, in StateBagChange change)
    {
        if (!MemberOf.TryGetValue(id, out int party)) return;  // not in a party yet
        uint leader = change.Removed ? 0 : (uint)change.Value.AsInt();
        if (Leaders.TryGetValue(party, out uint current) && current == leader) return;
        Leaders[party] = leader;
        EventBus.Publish(new PartyChanged(party));
    }

    private static void OnInvite(RpcEvent e)
    {
        if (e.Args.Length < 2) return;
        EventBus.Publish(new PartyInviteReceived(e.Args[0].AsInt(), (uint)e.Args[1].AsInt()));
    }

    // Parse "player:<id>"; false for any other bag (global, entity:*).
    private static bool TryPlayerId(string bagName, out uint id)
    {
        id = 0;
        const string prefix = "player:";
        return bagName.StartsWith(prefix, StringComparison.Ordinal) &&
               uint.TryParse(bagName.AsSpan(prefix.Length), out id);
    }
}
