using System;
using Recreation;
using Recreation.Modding;

namespace Recreation.Games.Fallout;

// Raised when the Adrenaline rank changes (a kill earns one, idle time sheds one).
public readonly struct AdrenalineChanged(int rank, float damageBonus) : IGameEvent
{
    public int Rank { get; } = rank;
    public float DamageBonus { get; } = damageBonus;  // fractional, e.g. 0.20 = +20%
}

// Fallout 4 Survival-mode Adrenaline as managed soft logic, a GameBehaviour. Kills
// build Adrenaline ranks (each rank a flat damage bonus); standing down for a while
// without a kill sheds a rank, so the meter rewards staying on the offensive and
// punishes turtling, exactly the Survival mechanic. The engine has no Adrenaline
// stat, so this is the authority: it counts kills made during the player's fights
// (the same in-combat ActorDied signal FalloutCombatRewards uses) and runs a decay
// timer. The damage bonus is published for a combat hook to read; ranks cap so it
// cannot run away.
public sealed class Adrenaline : GameBehaviour
{
    // Kills needed to earn one rank (Survival mode counts kills, not single hits).
    public int KillsPerRank { get; set; } = 5;
    // The most ranks the meter holds (Fallout 4 caps Adrenaline at 50% => 10 ranks).
    public int MaxRank { get; set; } = 10;
    // Damage added per rank, as a fraction (0.05 = +5% per rank).
    public float DamagePerRank { get; set; } = 0.05f;
    // Real seconds out of combat with no kill before a rank decays.
    public float DecaySeconds { get; set; } = 60f;

    public int Rank { get; private set; }
    public float DamageBonus => Rank * DamagePerRank;

    private int _killsTowardRank;
    private float _idleSeconds;
    private EventBus.Subscription? _death;

    protected override void OnStart() => _death = EventBus.Subscribe<ActorDied>(OnDeath);

    protected override void OnDestroy() => _death?.Dispose();

    protected override void OnUpdate(float deltaTime)
    {
        // Show the adrenaline rank as a HUD bar (red) while a kill streak holds.
        if (Rank > 0) Hud.Gauge("adrenaline", (float)Rank / MaxRank, "Adrenaline", 0xe04848ffu);
        else Hud.ClearGauge("adrenaline");

        if (deltaTime <= 0f || Rank == 0) return;

        // Only idle (out-of-combat) time decays the meter; a sustained fight holds
        // adrenaline up, the Survival behaviour.
        if (Game.Player.IsInCombat)
        {
            _idleSeconds = 0f;
            return;
        }
        _idleSeconds += deltaTime;
        if (_idleSeconds < DecaySeconds) return;
        _idleSeconds = 0f;
        SetRank(Rank - 1);
    }

    private void OnDeath(ActorDied e)
    {
        Actor player = Game.Player;
        if (e.ActorHandle == player.Handle) return;  // the player's own death earns nothing
        if (!player.IsInCombat) return;              // only kills during the player's fights

        _idleSeconds = 0f;
        if (Rank >= MaxRank) return;
        if (++_killsTowardRank < KillsPerRank) return;
        _killsTowardRank = 0;
        SetRank(Rank + 1);
    }

    private void SetRank(int rank)
    {
        int clamped = Math.Clamp(rank, 0, MaxRank);
        if (clamped == Rank) return;
        Rank = clamped;
        EventBus.Publish(new AdrenalineChanged(Rank, DamageBonus));
    }
}
