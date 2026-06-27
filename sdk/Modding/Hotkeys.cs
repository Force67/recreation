using System;

namespace Recreation.Modding;

// The ergonomic way to bind a key to an action, the gmod/Unity idiom for mod
// hotkeys. It is a thin wrapper over the KeyPressed event: Bind subscribes a
// handler that fires only for the chosen key. Dispose the returned token to
// unbind.
//
//   Hotkeys.Bind(Key.F, () => Game.Player.RestoreValue(ActorValue.Health, 50));
public static class Hotkeys
{
    public static EventBus.Subscription Bind(Key key, Action action)
    {
        ArgumentNullException.ThrowIfNull(action);
        return EventBus.Subscribe<KeyPressed>(e =>
        {
            if (e.Key == key) action();
        });
    }
}
