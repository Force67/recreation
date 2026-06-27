using System.Linq;
using Recreation;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers timed actor-value effects: apply adds the magnitude, expiry and early
// removal restore it, and permanent effects last until removed.
public static class EffectsTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        EventBus.Clear();

        Actor player = Game.Player;
        fake.SetValue(player.Handle, ActorValue.Health, current: 50, baseValue: 100);

        int applied = 0, expired = 0;
        using var a = EventBus.Subscribe<EffectApplied>(_ => applied++);
        using var e = EventBus.Subscribe<EffectExpired>(_ => expired++);

        // A 5-second +20 health buff.
        Effect buff = Effects.Apply(player, ActorValue.Health, 20f, durationSeconds: 5f);
        check.Equal("magnitude applied", 70f, fake.GetCurrent(player.Handle, ActorValue.Health));
        check.Equal("effect is active", 1, Effects.ActiveCount);
        check.Equal("apply event raised", 1, applied);

        // Not yet expired after 4 seconds.
        ModHost.Tick(4f);
        check.Equal("still active before expiry", 70f,
                    fake.GetCurrent(player.Handle, ActorValue.Health));

        // Expires at 5 seconds, restoring the value.
        ModHost.Tick(1f);
        check.Equal("value restored on expiry", 50f,
                    fake.GetCurrent(player.Handle, ActorValue.Health));
        check.Equal("no active effects", 0, Effects.ActiveCount);
        check.Equal("expire event raised", 1, expired);

        // A permanent effect lasts until removed.
        Effect perm = Effects.Apply(player, ActorValue.Health, 10f, durationSeconds: 0f);
        ModHost.Tick(100f);
        check.Equal("permanent effect persists", 60f,
                    fake.GetCurrent(player.Handle, ActorValue.Health));
        check.That("queryable on the actor", Effects.ActiveOn(player).Contains(perm));
        Effects.Remove(perm);
        check.Equal("removed effect restores", 50f,
                    fake.GetCurrent(player.Handle, ActorValue.Health));

        ModHost.Shutdown();
        EventBus.Clear();
        Native.Backend = null;
    }
}
