using System;
using Recreation.Modding;

namespace Recreation.Net.Samples;

// Sample gamemode composing the platform: teams, economy with jobs, chat commands
// and emotes. Inert unless REC_RP_SAMPLE is set. The command logic is exposed as
// static methods so it is unit-tested directly.
[Mod("RoleplayServer"), Realm(ModRealm.Server)]
public sealed class RoleplayServer : IMod
{
    public const int TeamCitizen = 1;
    public const int TeamPolice = 2;

    public void OnLoad()
    {
        if (Environment.GetEnvironmentVariable("REC_RP_SAMPLE") == null) return;
        Setup();
    }

    // Define the world (teams + jobs) and wire the commands and join flow. Exposed so
    // a test can stand the gamemode up without the env gate.
    public static void Setup()
    {
        Teams.Define(TeamCitizen, "Citizens", 0x4f7fd8ffu);  // blue
        Teams.Define(TeamPolice, "Police", 0xffd24affu);     // gold
        Jobs.Register(new Job("unemployed", 0));
        Jobs.Register(new Job("miner", 50));
        Jobs.Register(new Job("medic", 70));

        Chat.RegisterCommand("team", c => c.Reply(
            JoinTeam(c.Sender, Arg(c, 0)) ? $"Joined {Arg(c, 0)}." : "Usage: /team citizens|police"));
        Chat.RegisterCommand("job", c => c.Reply(
            TakeJob(c.Sender, Arg(c, 0)) ? $"You are now a {Arg(c, 0)}." : $"No such job '{Arg(c, 0)}'."));
        Chat.RegisterCommand("pay", DoPay);
        Chat.RegisterCommand("me", c =>
        {
            if (c.Args.Length > 0) Chat.System($"* {Name(c.Sender)} {string.Join(' ', c.Args)}");
        });

        EventBus.Subscribe<ClientJoined>(e => OnJoin(e.Peer));
    }

    private static void OnJoin(uint peer)
    {
        Teams.Assign(peer, TeamCitizen);
        Jobs.Assign(peer, "unemployed");
        Chat.System($"{Name(peer)} joined the city.");
    }

    // --- testable command logic ---

    // Join a team by friendly name. Returns false for an unknown team.
    public static bool JoinTeam(uint player, string name)
    {
        int id = name.ToLowerInvariant() switch
        {
            "police" or "cop" or "cops" => TeamPolice,
            "citizen" or "citizens" => TeamCitizen,
            _ => 0,
        };
        if (id == 0) return false;
        Teams.Assign(player, id);
        return true;
    }

    // Take a registered job. Returns false for an unknown job.
    public static bool TakeJob(uint player, string name)
    {
        if (Jobs.Get(name) == null) return false;
        Jobs.Assign(player, name);
        return true;
    }

    private static void DoPay(ChatCommandContext c)
    {
        if (c.Args.Length < 2 || !uint.TryParse(c.Args[0], out uint to) ||
            !int.TryParse(c.Args[1], out int amount))
        {
            c.Reply("Usage: /pay <playerId> <amount>");
            return;
        }
        TransactionResult r = Economy.Transfer(c.Sender, to, amount);
        c.Reply(r == TransactionResult.Ok ? $"Paid {amount} to {Name(to)}." : $"Payment failed: {r}.");
    }

    private static string Arg(ChatCommandContext c, int i) => i < c.Args.Length ? c.Args[i] : "";

    private static string Name(uint id) => Players.Get(id)?.Name ?? $"Player {id}";
}
