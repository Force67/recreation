using System;
using System.Collections.Generic;
using Recreation.Interop;
using Recreation.Modding;
using Recreation.Net;

namespace Recreation.Tests;

// Exercises the social subsystem (parties + presence) across roles: leader-only invites/kicks, replicated membership, and presence round-trip.
public static class SocialTests
{
    private sealed class Recording : IRpcBackend
    {
        public readonly List<(RpcTarget Target, uint Peer, string Name, Value[] Args)> Emits = new();
        public void Emit(RpcTarget target, uint peer, string name, ReadOnlySpan<Value> args) =>
            Emits.Add((target, peer, name, args.ToArray()));
        public void Subscribe(string name) { }
    }

    public static void Run(Check check)
    {
        // ===== server: create a party, invite a player, accept =====
        Rpc.Clear();
        EventBus.Clear();
        var rec = new Recording();
        Rpc.Bind(rec);
        var changed = new List<int>();
        EventBus.Subscribe<PartyChanged>(e => changed.Add(e.PartyId));
        Platform.Boot(NetRole.Server);   // binds StateBags + Players
        Social.Bind(NetRole.Server);     // then social rides on them

        int party = Parties.Create(1u);
        check.That("create assigns a non-zero party id", party != 0);
        check.Equal("leader's party bag holds the id", party, StateBags.Player(1).Get("party").AsInt());
        check.That("the creator is the leader", Parties.IsLeader(1u));
        check.That("PartyChanged fired on create", changed.Contains(party));

        rec.Emits.Clear();
        Parties.Invite(1u, 2u);
        check.That("invite notifies the invitee client",
            rec.Emits.Exists(e => e.Name == "social:invite" && e.Target == RpcTarget.ToClient &&
                                  e.Peer == 2u && e.Args[0].AsInt() == party));

        changed.Clear();
        bool accepted = Parties.Accept(2u);
        check.That("accept joins the party", accepted);
        check.Equal("invitee's party bag equals the party id", party, StateBags.Player(2).Get("party").AsInt());
        Party? of2 = Parties.Of(2u);
        check.That("Parties.Of resolves the invitee's party", of2.HasValue && of2.Value.Id == party);
        check.That("party now holds both members", Parties.Members(party).Count == 2);
        check.That("PartyChanged fired on join", changed.Contains(party));

        // ===== leader-only enforcement =====
        rec.Emits.Clear();
        Parties.Invite(2u, 3u);  // 2 is a member, not the leader
        check.That("a non-leader invite is rejected (no notify)",
            !rec.Emits.Exists(e => e.Name == "social:invite"));
        Parties.Kick(2u, 1u);    // 2 tries to kick the leader
        check.That("a non-leader kick is rejected",
            Parties.IsLeader(1u) && Parties.Members(party).Count == 2);

        // the same enforcement over the wire: a req_invite from a non-leader is dropped
        rec.Emits.Clear();
        Rpc.Dispatch("social:req_invite", 2u, false, new[] { Value.Int(3) });
        check.That("req_invite from a non-leader sends no invite",
            !rec.Emits.Exists(e => e.Name == "social:invite"));

        // a req_invite from the leader is honoured
        rec.Emits.Clear();
        Rpc.Dispatch("social:req_invite", 1u, false, new[] { Value.Int(4) });
        check.That("req_invite from the leader notifies the invitee",
            rec.Emits.Exists(e => e.Name == "social:invite" && e.Peer == 4u));

        // ===== leave: a member drops, then the leader leaving disbands =====
        changed.Clear();
        Parties.Leave(2u);
        check.That("a leaving member is removed", Parties.Of(2u) == null);
        check.That("the party survives with the leader", Parties.Members(party).Count == 1);
        check.That("PartyChanged fired on leave", changed.Contains(party));

        Parties.Leave(1u);  // leader leaves -> whole party disbands
        check.That("leader leaving disbands the party", Parties.Of(1u) == null);
        check.That("a disbanded party has no members", Parties.Members(party).Count == 0);

        // ===== client: membership arrives purely by bag replication =====
        Rpc.Clear();
        EventBus.Clear();
        Rpc.Bind(new Recording());
        var clientChanged = new List<int>();
        EventBus.Subscribe<PartyChanged>(e => clientChanged.Add(e.PartyId));
        Platform.Boot(NetRole.Client);
        Social.Bind(NetRole.Client);

        Rpc.Dispatch("sb:set", 0u, true,
            new[] { Value.String("player:2"), Value.String("party"), Value.Int(7) });
        Party? clientParty = Parties.Of(2u);
        check.That("client learns membership from the replicated bag",
            clientParty.HasValue && clientParty.Value.Id == 7);
        check.That("client raised PartyChanged for the replicated party", clientChanged.Contains(7));

        // a server-pushed invite raises the client-side notification
        var invites = new List<(int Party, uint From)>();
        EventBus.Subscribe<PartyInviteReceived>(e => invites.Add((e.PartyId, e.FromLeader)));
        Rpc.Dispatch("social:invite", 0u, true, new[] { Value.Int(7), Value.Int(1) });
        check.That("client raises PartyInviteReceived", invites.Contains((7, 1u)));

        // the client request forwarders go to the server, unvalidated locally
        var clientRec = new Recording();
        Rpc.Bind(clientRec);
        Parties.RequestLeave();
        check.That("RequestLeave forwards to the server",
            clientRec.Emits.Exists(e => e.Name == "social:req_leave" && e.Target == RpcTarget.ToServer));

        // ===== presence: local status/activity round-trips and notifies =====
        Rpc.Clear();
        EventBus.Clear();
        Rpc.Bind(new Recording());
        var presence = new List<uint>();
        EventBus.Subscribe<PresenceChanged>(e => presence.Add(e.Player.Id));
        Platform.Boot(NetRole.Standalone);
        Social.Bind(NetRole.Standalone);

        check.Equal("an unset status reads as Offline",
            PlayerStatus.Offline, Presence.StatusOf(Players.Local));
        Presence.SetStatus(PlayerStatus.Busy);
        check.Equal("status reads back on the local player",
            PlayerStatus.Busy, Presence.StatusOf(Players.Local));
        check.That("PresenceChanged fired for the local player", presence.Contains(Players.LocalId));

        Presence.SetActivity("Exploring Whiterun");
        check.Equal("activity reads back", "Exploring Whiterun", Presence.ActivityOf(Players.Local));

        // ===== teardown =====
        Social.Reset();
        Platform.Reset();
        Rpc.Clear();
        EventBus.Clear();
    }
}
