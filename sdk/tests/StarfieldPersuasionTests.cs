using Recreation.Games.Starfield;

namespace Recreation.Tests;

// Covers the Starfield persuasion resolver: the landing chance rises with skill and
// falls with difficulty, a landed option banks its points and a miss spends an
// attempt for nothing, reaching the target wins, and running out of attempts loses.
// All deterministic via the injected roll, no engine needed.
public static class StarfieldPersuasionTests
{
    public static void Run(Check check)
    {
        var speech = new Persuasion();

        // Chance rises with skill and falls with difficulty.
        float easyLow = speech.ChanceOf(Persuasion.Difficulty.Easy, 0f);
        float easyHigh = speech.ChanceOf(Persuasion.Difficulty.Easy, 50f);
        float hardHigh = speech.ChanceOf(Persuasion.Difficulty.Hard, 50f);
        check.That("base easy chance", easyLow > 0.84f && easyLow < 0.86f);
        check.That("skill raises the chance", easyHigh > easyLow);
        check.That("a harder line lands less often", hardHigh < easyHigh);
        check.That("chance never exceeds one",
                   speech.ChanceOf(Persuasion.Difficulty.Easy, 1000f) <= 1f);

        // A challenge worth 8 points over 4 attempts.
        PersuasionState state = Persuasion.Begin(target: 8, attempts: 4);
        check.That("a fresh challenge is unresolved", !state.Over);
        check.Equal("starts needing the full target", 8, state.Remaining);

        // A roll below the chance lands the option (roll 0 always lands); it banks the
        // points and spends one attempt.
        (state, PersuasionResult r1) = speech.Resolve(state, points: 3,
            Persuasion.Difficulty.Medium, persuasionSkill: 20f, roll: 0f);
        check.Equal("a landed option makes progress", PersuasionResult.Progress, r1);
        check.Equal("points were banked", 5, state.Remaining);
        check.Equal("an attempt was spent", 3, state.AttemptsLeft);

        // A roll at or above the chance misses: an attempt is spent, no points.
        (state, PersuasionResult r2) = speech.Resolve(state, points: 3,
            Persuasion.Difficulty.Hard, persuasionSkill: 0f, roll: 0.99f);
        check.Equal("a missed option still spends an attempt", PersuasionResult.Missed, r2);
        check.Equal("no points from a miss", 5, state.Remaining);
        check.Equal("attempt spent on the miss", 2, state.AttemptsLeft);

        // Landing enough to reach the target wins.
        (state, PersuasionResult r3) = speech.Resolve(state, points: 5,
            Persuasion.Difficulty.Easy, persuasionSkill: 50f, roll: 0.1f);
        check.Equal("reaching the target wins", PersuasionResult.Won, r3);
        check.That("the challenge is won", state.Won);
        check.That("the challenge is over", state.Over);

        // A resolved challenge does not advance further.
        (PersuasionState after, PersuasionResult r4) = speech.Resolve(state, points: 5,
            Persuasion.Difficulty.Easy, persuasionSkill: 50f, roll: 0f);
        check.Equal("a won challenge stays won", PersuasionResult.Won, r4);
        check.Equal("its state is unchanged", state.Remaining, after.Remaining);

        // Burning the last attempt without reaching the target loses.
        PersuasionState losing = Persuasion.Begin(target: 10, attempts: 1);
        (losing, PersuasionResult r5) = speech.Resolve(losing, points: 3,
            Persuasion.Difficulty.Medium, persuasionSkill: 30f, roll: 0f);
        check.Equal("falling short on the last attempt loses", PersuasionResult.Lost, r5);
        check.That("the challenge is lost", losing.Lost);
        check.That("the challenge is over", losing.Over);
    }
}
