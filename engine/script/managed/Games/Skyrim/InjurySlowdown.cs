using Recreation.Modding;

namespace Recreation.Games.Skyrim;

// Slows the player while badly wounded, a survival-flavoured rule implemented as
// managed soft logic. When health drops below a threshold it applies a one-time
// speed penalty (a limp) and removes it once health recovers, tracking whether
// the penalty is currently applied so it never stacks or leaks. SpeedMult is
// adjusted relatively (ModValue), so it composes with other speed effects.
public sealed class InjurySlowdown : GameBehaviour
{
    // Health fraction (current/base) below which the limp applies.
    public float Threshold { get; set; } = 0.25f;

    // Points removed from SpeedMult while injured.
    public float SpeedPenalty { get; set; } = 30f;

    private bool _applied;

    protected override void OnUpdate(float deltaTime)
    {
        Actor player = Game.Player;
        if (!player.Exists) return;

        bool injured = player.GetValuePercent(ActorValue.Health) < Threshold;
        if (injured && !_applied)
        {
            player.ModValue(ActorValue.SpeedMult, -SpeedPenalty);
            _applied = true;
        }
        else if (!injured && _applied)
        {
            player.ModValue(ActorValue.SpeedMult, SpeedPenalty);
            _applied = false;
        }
    }
}
