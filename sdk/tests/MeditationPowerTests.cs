using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers the meditation power end to end: the hotkey starts it, controls lock,
// stats recover over the duration, controls return and a cooldown gates a repeat.
public static class MeditationPowerTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        EventBus.Clear();
        PlayerControls.Clear();

        foreach (string av in new[] { ActorValue.Health, ActorValue.Magicka, ActorValue.Stamina })
            fake.SetValue(fake.Player, av, current: 0, baseValue: 100);

        ModHost.Register(new MeditationPower { Hotkey = Key.T, Duration = 4f, Cooldown = 30f });

        // Press the key: the coroutine starts on the next tick and locks controls.
        EventBus.Publish(new KeyPressed(Key.T));
        ModHost.Tick(0f);
        check.That("controls lock while meditating", !fake.PlayerControlsEnabled);
        check.That("on cooldown once started", !Cooldowns.IsReady("meditation"));

        // Advance through the duration; stats recover and controls return.
        for (int i = 0; i < 4; i++) ModHost.Tick(1f);
        check.Equal("health restored", 100f, fake.GetCurrent(fake.Player, ActorValue.Health));
        check.Equal("magicka restored", 100f, fake.GetCurrent(fake.Player, ActorValue.Magicka));
        check.That("controls return after resting", fake.PlayerControlsEnabled);

        // Pressing again during cooldown does nothing.
        fake.SetValue(fake.Player, ActorValue.Health, 0, 100);
        EventBus.Publish(new KeyPressed(Key.T));
        ModHost.Tick(0f);
        check.That("cooldown blocks a repeat", fake.PlayerControlsEnabled);
        check.Equal("no restore while on cooldown", 0f, fake.GetCurrent(fake.Player, ActorValue.Health));

        ModHost.Shutdown();
        EventBus.Clear();
        PlayerControls.Clear();
        Native.Backend = null;
    }
}
