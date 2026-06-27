using System.Collections;
using Recreation.Modding;

namespace Recreation.Games.Skyrim;

// A meditate-to-rest power, and the worked reference for a rich mod: it composes
// a hotkey, the player-controls gate, a cooldown, a coroutine and timed
// restoration into one feature. Press the key and the player meditates -- controls
// lock, health, magicka and stamina recover over a few seconds, then controls
// return and a cooldown gates the next rest. Opt-in (it adds a power and binds a
// key), enabled with "meditationPower": true in Skyrim.json.
public sealed class MeditationPower : GameBehaviour
{
    public Key Hotkey { get; set; } = Key.T;
    public float Duration { get; set; } = 4f;
    public float Cooldown { get; set; } = 30f;

    private const string ControlReason = "meditation";
    private const string CooldownKey = "meditation";

    private EventBus.Subscription? _binding;

    protected override void OnStart() => _binding = Hotkeys.Bind(Hotkey, Begin);

    protected override void OnDestroy() => _binding?.Dispose();

    private void Begin()
    {
        // Not while already resting or controls are otherwise taken, and only
        // once the cooldown has elapsed.
        if (PlayerControls.Disabled || !Cooldowns.IsReady(CooldownKey)) return;
        Cooldowns.Start(CooldownKey, Cooldown);
        StartCoroutine(Meditate());
    }

    private IEnumerator Meditate()
    {
        PlayerControls.Disable(ControlReason);
        Debug.Notification("Meditating...");

        Actor player = Game.Player;
        const int steps = 4;
        for (int i = 0; i < steps; i++)
        {
            yield return new WaitForSeconds(Duration / steps);
            Restore(player, ActorValue.Health);
            Restore(player, ActorValue.Magicka);
            Restore(player, ActorValue.Stamina);
        }

        Debug.Notification("Rested.");
        PlayerControls.Enable(ControlReason);
    }

    private static void Restore(Actor actor, string attribute) =>
        actor.RestoreValue(attribute, actor.GetBaseValue(attribute));
}
