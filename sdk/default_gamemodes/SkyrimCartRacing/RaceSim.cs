using System;
using System.Collections.Generic;
using Recreation;

namespace Recreation.Games.SkyrimCartRacing;

// The pure race-rule state for "super cart xtreme racing", with no engine calls
// so the whole scoring/gating/boost machine can be tested against nothing more
// than a runner. The behaviour drives it: it feeds the player's position, calls
// EnterCheckpoint as the rider crosses blips, and reads back speed-sensitive
// rules like Done, WrongWay and BestLap each frame.
//
// Checkpoints are ordered; a lap is clearing the whole course once. Crossing a
// blip that is not the expected next one is a wrong way (you have gone back over
// a checkpoint you already cleared), which a mod can punish however it likes.
public sealed class RaceSim
{
    // Course layout in game units, in order. The respawn point is the last
    // checkpoint cleared (the start position for a fresh race).
    public IReadOnlyList<Vector3> Checkpoints { get; }

    public int TotalLaps { get; }

    // The lap currently being raced: 1-based, 1 before the first lap is done.
    public int Lap { get; private set; } = 1;

    // Index of the next checkpoint the rider must clear (see Checkpoints).
    public int CheckpointIndex { get; private set; }

    // True once the final checkpoint of the final lap is cleared.
    public bool Done { get; private set; }

    // True while the rider is stood over a checkpoint out of order, i.e. going
    // the wrong way up the course.
    public bool Wrong { get; private set; }

    // The boost meter, 0..1. Drains while boosting, refills while not.
    public float Boost { get; private set; } = 1f;

    public float BoostDrainPerSecond { get; set; } = 0.25f;
    public float BoostRegenPerSecond { get; set; } = 0.15f;

    // Seconds since the last lap (or race start) began, and total elapsed.
    public float LapTime { get; private set; }
    public float TotalTime { get; private set; }

    // Best complete lap in seconds; 0 when none has finished yet.
    public float BestLap { get; private set; }

    // Where a stuck rider comes back to: the last checkpoint cleared, in game
    // units. Starts at the first checkpoint.
    public Vector3 RespawnPosition { get; private set; }

    public RaceSim(IReadOnlyList<Vector3> checkpoints, int totalLaps)
    {
        if (checkpoints == null || checkpoints.Count < 2)
            throw new ArgumentException("a course needs at least two checkpoints", nameof(checkpoints));
        if (totalLaps < 1)
            throw new ArgumentException("a race needs at least one lap", nameof(totalLaps));
        Checkpoints = checkpoints;
        TotalLaps = totalLaps;
        RespawnPosition = checkpoints[0];
    }

    public Vector3 Checkpoint => Checkpoints[CheckpointIndex];

    // Advances the clock and the boost meter. Call every frame; `boosting` is
    // true only when the rider is actually burning the meter (there is on-road
    // speed), so a stall doesn't silently empty it.
    public void Update(float dt, bool boosting)
    {
        if (Done) return;
        TotalTime += dt;
        LapTime += dt;
        Boost = Math.Clamp(Boost + (boosting ? -BoostDrainPerSecond : BoostRegenPerSecond) * dt, 0f, 1f);
    }

    // The rider is over checkpoint `index`. Returns true when that cleared a lap
    // or the race (a milestone a mod celebrates). Non-expected checkpoints are
    // marked Wrong but do not move progress.
    public bool EnterCheckpoint(int index)
    {
        if (index != CheckpointIndex)
        {
            Wrong = true;
            return false;
        }
        Wrong = false;
        RespawnPosition = Checkpoints[index];
        CheckpointIndex++;
        if (CheckpointIndex < Checkpoints.Count)
            return false;

        // Cleared the whole course: a lap. Travel the clock onto the record.
        float lapTime = LapTime;
        LapTime = 0;
        CheckpointIndex = 0;
        if (BestLap == 0f || lapTime < BestLap)
            BestLap = lapTime;
        if (Lap >= TotalLaps)
        {
            Done = true;
            return true;
        }
        Lap++;
        return true;
    }

    // How far through the race, useful as a scoreboard sort key.
    public int Progress => (Lap - 1) * Checkpoints.Count + CheckpointIndex;
}