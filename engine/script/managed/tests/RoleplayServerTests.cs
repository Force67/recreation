using System;
using System.Collections.Generic;
using Recreation.Interop;
using Recreation.Modding;
using Recreation.Net;
using Recreation.Net.Samples;

namespace Recreation.Tests;

// Exercises the sample roleplay gamemode end to end: teams, the economy with jobs, and chat commands.
public static class RoleplayServerTests
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
        Rpc.Clear();
        Rpc.Bind(new Recording());
        Platform.Boot(NetRole.Server);
        RoleplayServer.Setup();

        EventBus.Publish(new ClientJoined(5u));
        EventBus.Publish(new ClientJoined(6u));

        // The join flow placed each player on the citizen team with the default job.
        check.Equal("joiner is a citizen", RoleplayServer.TeamCitizen, Teams.TeamIdOf(5u));
        check.Equal("joiner is unemployed", "unemployed", Jobs.JobOf(Players.Get(5u)!)!.Name);

        // Team switching by friendly name; an unknown team is rejected.
        check.That("join police ok", RoleplayServer.JoinTeam(5u, "police"));
        check.Equal("now on police", RoleplayServer.TeamPolice, Teams.TeamIdOf(5u));
        check.That("unknown team rejected", !RoleplayServer.JoinTeam(5u, "wizards"));

        // Taking a registered job; an unknown job is rejected.
        check.That("take job ok", RoleplayServer.TakeJob(5u, "medic"));
        check.Equal("job set", "medic", Jobs.JobOf(Players.Get(5u)!)!.Name);
        check.That("unknown job rejected", !RoleplayServer.TakeJob(5u, "astronaut"));

        // The /pay command moves money between players (both have starting cash).
        int before5 = Wallet.Of(5u).Cash;
        int before6 = Wallet.Of(6u).Cash;
        check.That("players have starting cash", before5 > 0 && before6 > 0);
        Rpc.Dispatch("chat:say", 5u, false, new[] { Value.Int(0), Value.String("/pay 6 100") });
        check.Equal("payer debited", before5 - 100, Wallet.Of(5u).Cash);
        check.Equal("payee credited", before6 + 100, Wallet.Of(6u).Cash);

        Platform.Reset();
        Rpc.Clear();
        EventBus.Clear();
    }
}
