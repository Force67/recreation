using System;
using System.Collections.Generic;

namespace Recreation.Net;

// The job registry and per-player job assignment. Jobs are shared static data;
// assignment and payout are server operations.
public static class Jobs
{
    // The player-bag key holding a player's current job name.
    internal const string JobKey = "job";

    // Case-insensitive so "Miner" and "miner" resolve to the same job.
    private static readonly Dictionary<string, Job> Registry = new(StringComparer.OrdinalIgnoreCase);

    public static void Register(Job job) => Registry[job.Name] = job;

    public static Job? Get(string name) => Registry.TryGetValue(name, out Job? job) ? job : null;

    public static IReadOnlyCollection<Job> All => Registry.Values;

    // Server: put a player on a job. Writing the bag replicates the assignment.
    public static void Assign(uint player, string job)
    {
        if (!Economy.IsAuthoritative) return;
        StateBags.Player(player).Set(JobKey, job);
    }

    // The job a player currently holds, or null if unassigned (or assigned a job no
    // longer in the registry).
    public static Job? JobOf(Player player) => Get(player.State.Get(JobKey).AsString());

    // Server: pay a player one shift of their job's base pay. An unassigned or
    // zero-pay job pays nothing (Economy.Give returns InvalidAmount).
    public static TransactionResult Pay(uint player)
    {
        Job? job = Get(StateBags.Player(player).Get(JobKey).AsString());
        return Economy.Give(player, job?.Pay ?? 0);
    }

    // Register the sample jobs every session starts with.
    internal static void Bind()
    {
        Register(new Job("unemployed", 0));
        Register(new Job("miner", 50));
        Register(new Job("guard", 75));
    }

    internal static void Reset() => Registry.Clear();
}
