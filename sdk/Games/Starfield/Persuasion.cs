using System;

namespace Recreation.Games.Starfield;

// The outcome of resolving one persuasion option against a challenge.
public enum PersuasionResult
{
    // The option landed but the target is not yet met: keep going.
    Progress,
    // The option landed and reached the target: the challenge is won.
    Won,
    // The option missed (no points) and the last attempt is spent: the challenge is
    // lost.
    Lost,
    // The option missed but attempts remain.
    Missed,
}

// A live persuasion challenge: how many points are still needed and how many
// attempts remain. Immutable; each resolved option yields the next state, so a
// challenge is a value that threads through the conversation rather than mutable
// dialogue state.
public readonly struct PersuasionState(int remaining, int attemptsLeft)
{
    // Persuasion points still needed to win.
    public int Remaining { get; } = remaining;
    // Attempts (dialogue rounds) the player has left.
    public int AttemptsLeft { get; } = attemptsLeft;

    public bool Won => Remaining <= 0;
    public bool Lost => Remaining > 0 && AttemptsLeft <= 0;
    public bool Over => Won || Lost;
}

// Starfield's persuasion (speech) challenge resolver, the deterministic core of the
// dialogue minigame. A challenge has a target score the player must reach by
// spending persuasion options across a handful of attempts; each option is worth
// some points at an odds level (Easy/Medium/Hard), and whether it lands is decided
// by the player's Persuasion skill against that level. Pure math with no dialogue
// state, like Skyrim's BarterPricing: a UI previews an option's chance, the same
// formula settles the round, and a mod retunes the difficulty by constructing its
// own. The randomness is injected (a roll in [0,1)), so a test pins it and the
// engine feeds Utility.RandomFloat for a live conversation.
public sealed class Persuasion
{
    // The base chance an option lands at each odds level before skill is folded in.
    public float EasyChance { get; init; } = 0.85f;
    public float MediumChance { get; init; } = 0.55f;
    public float HardChance { get; init; } = 0.30f;

    // How much one point of Persuasion skill improves the landing chance. With the
    // default, 50 skill adds a quarter, so a maxed talker rarely misses an Easy line.
    public float ChancePerSkill { get; init; } = 0.005f;

    // The odds tier of a single persuasion option.
    public enum Difficulty
    {
        Easy,
        Medium,
        Hard,
    }

    // Begins a challenge worth `target` points with `attempts` rounds to clear it.
    public static PersuasionState Begin(int target, int attempts) =>
        new(Math.Max(0, target), Math.Max(0, attempts));

    // The chance (0..1) an option at `difficulty` lands for a speaker with
    // `persuasionSkill`, the value a UI shows as a percentage.
    public float ChanceOf(Difficulty difficulty, float persuasionSkill)
    {
        float baseChance = difficulty switch
        {
            Difficulty.Easy => EasyChance,
            Difficulty.Medium => MediumChance,
            _ => HardChance,
        };
        return Math.Clamp(baseChance + ChancePerSkill * persuasionSkill, 0f, 1f);
    }

    // Resolves one option worth `points` at `difficulty` against the running `state`,
    // given the speaker's skill and an injected `roll` in [0,1). A landed option
    // spends one attempt and banks its points; a miss spends an attempt for nothing.
    // Returns the next state and the result.
    public (PersuasionState State, PersuasionResult Result) Resolve(
        PersuasionState state, int points, Difficulty difficulty, float persuasionSkill, float roll)
    {
        // A finished or attempt-less challenge does not advance.
        if (state.Over || state.AttemptsLeft <= 0)
            return (state, state.Won ? PersuasionResult.Won : PersuasionResult.Lost);

        bool landed = roll < ChanceOf(difficulty, persuasionSkill);
        int remaining = landed ? Math.Max(0, state.Remaining - Math.Max(0, points)) : state.Remaining;
        var next = new PersuasionState(remaining, state.AttemptsLeft - 1);

        PersuasionResult result =
            next.Won ? PersuasionResult.Won :
            next.Lost ? PersuasionResult.Lost :
            landed ? PersuasionResult.Progress : PersuasionResult.Missed;
        return (next, result);
    }
}
