using System;
using System.Collections.Generic;
using System.Linq;
using Recreation.Interop;
using Recreation.Modding;
using Recreation.Net;

namespace Recreation.Tests;

// Exercises the teams subsystem on top of the state-bag layer: the host defines, assigns,
// scores and gates friendly fire, and a client learns it from replicated bags.
public static class TeamsTests
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
        var changed = new List<uint>();
        EventBus.Subscribe<TeamChanged>(e => changed.Add(e.Player.Id));

        // --- server: define, assign, score, friendly fire, join requests ---------
        Rpc.Clear();
        var rec = new Recording();
        Rpc.Bind(rec);
        Platform.Boot(NetRole.Server);
        Teams.Bind(NetRole.Server);

        // Define a couple of teams; Get/All reflect them, and a definition
        // replicates as an sb:set on the global bag (so clients learn it).
        rec.Emits.Clear();
        Teams.Define(1, "Cops", 0x1144FFFFu);
        Teams.Define(2, "Crooks", 0xFF2222FFu);
        check.Equal("two teams are defined", 2, Teams.All.Count);
        check.Equal("Get resolves the team name", "Cops", Teams.Get(1)!.Name);
        check.Equal("Get resolves the packed colour", 0xFF2222FFu, Teams.Get(2)!.Color);
        check.That("an undefined team is null", Teams.Get(9) == null);
        check.That("defining replicates over the global bag",
            rec.Emits.Exists(e => e.Name == "sb:set" && e.Args[0].AsString() == "global" &&
                                  e.Args[1].AsString() == "team.1.name"));

        // Two peers join the roster.
        EventBus.Publish(new ClientJoined(5u));
        EventBus.Publish(new ClientJoined(6u));

        // Assign a player: its bag is set, TeamOf/Members resolve, TeamChanged fires.
        changed.Clear();
        Teams.Assign(5u, 1);
        check.Equal("assigned player's team bag is set", 1, StateBags.Player(5).Get("team").AsInt());
        check.Equal("TeamOf resolves the team", "Cops", Teams.TeamOf(Players.Get(5u)!)!.Name);
        check.Equal("TeamIdOf reads the id", 1, Teams.TeamIdOf(5u));
        check.That("Members lists the assigned player", Teams.Members(1).Any(p => p.Id == 5u));
        check.That("TeamChanged fired for the assignment", changed.Contains(5u));

        // Two players on the same team: SameTeam, and friendly fire gates damage.
        Teams.Assign(6u, 1);
        Player p5 = Players.Get(5u)!;
        Player p6 = Players.Get(6u)!;
        check.That("teammates are SameTeam", Teams.SameTeam(p5, p6));
        check.That("friendly fire off blocks damage between teammates", !Teams.CanDamage(p5, p6));
        Teams.FriendlyFire = true;
        check.That("friendly fire on allows damage between teammates", Teams.CanDamage(p5, p6));
        Teams.FriendlyFire = false;

        // Moving a player to another team updates the reverse index both ways.
        Teams.Assign(6u, 2);
        check.That("opponents are not SameTeam", !Teams.SameTeam(p5, p6));
        check.That("opponents can always be damaged", Teams.CanDamage(p5, p6));
        check.That("reassignment left the old team", !Teams.Members(1).Any(p => p.Id == 6u));
        check.That("reassignment joined the new team", Teams.Members(2).Any(p => p.Id == 6u));

        // Scoring accumulates in the global bag.
        Teams.AddScore(1, 3);
        Teams.AddScore(1, 2);
        check.Equal("score accumulates", 5, Teams.ScoreOf(1));
        check.Equal("an unscored team reads zero", 0, Teams.ScoreOf(2));

        // A client team:join request for a defined team assigns the sender...
        Rpc.Dispatch("team:join", 6u, false, new[] { Value.Int(1) });
        check.Equal("join request to a defined team assigns the sender", 1, Teams.TeamIdOf(6u));
        // ...but for an undefined team it is rejected (membership unchanged).
        Rpc.Dispatch("team:join", 6u, false, new[] { Value.Int(9) });
        check.Equal("join request to an undefined team is ignored", 1, Teams.TeamIdOf(6u));

        // --- client: definitions and membership arrive only by replication -------
        Rpc.Clear();
        Rpc.Bind(new Recording());
        Platform.Boot(NetRole.Client);
        Teams.Bind(NetRole.Client);
        changed.Clear();

        // The server replicates a player's presence and a team definition...
        Rpc.Dispatch("sb:set", 0u, true,
            new[] { Value.String("player:2"), Value.String("present"), Value.Bool(true) });
        Rpc.Dispatch("sb:set", 0u, true,
            new[] { Value.String("global"), Value.String("team.1.name"), Value.String("Cops") });
        Rpc.Dispatch("sb:set", 0u, true,
            new[] { Value.String("global"), Value.String("team.1.color"), Value.Int(0x1144FFFF) });
        check.Equal("client learns the team by replication", "Cops", Teams.Get(1)!.Name);

        // ...then that player's team membership; the client's view reflects it.
        Rpc.Dispatch("sb:set", 0u, true,
            new[] { Value.String("player:2"), Value.String("team"), Value.Int(1) });
        check.Equal("client TeamOf reflects replicated membership", "Cops", Teams.TeamOf(Players.Get(2u)!)!.Name);
        check.That("client Members reflects replicated membership", Teams.Members(1).Any(p => p.Id == 2u));
        check.That("client TeamChanged fired", changed.Contains(2u));

        Teams.Reset();
        Platform.Reset();
        Rpc.Clear();
        EventBus.Clear();
    }
}
