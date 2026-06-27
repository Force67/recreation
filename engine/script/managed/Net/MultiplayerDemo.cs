using System;
using Recreation.Modding;

namespace Recreation.Net;

// A tiny built-in showcase that proves the multiplayer platform reaches the
// on-screen HUD end to end: chat lines flow C# -> the Hud.ChatLine native -> the
// engine chat box, and a toast notification flows the same way. Inert unless
// REC_MP_DEMO is set, so it never affects a normal session. Shared realm so it
// runs in single-player and on either side of a session.
[Mod("MultiplayerDemo"), Realm(ModRealm.Shared)]
public sealed class MultiplayerDemo : IMod
{
    public void OnLoad()
    {
        if (Environment.GetEnvironmentVariable("REC_MP_DEMO") == null) return;

        Notify.Show("Connected to Recreation Multiplayer", NoticeKind.Success);
        Chat.System("Welcome to the server. Type /help for commands.");

        // A short scripted exchange so the chat box fills in front of the camera.
        Scheduler.After(1.0f, () => Say(2, "Lydia", "anyone heading to Whiterun?"));
        Scheduler.After(2.0f, () => Say(3, "Mjoll", "on my way to the meadery"));
        Scheduler.After(3.0f, () => Say(2, "Lydia", "save me a mead!"));
        Scheduler.After(4.0f, () => Chat.System("Mjoll reached Whiterun."));
    }

    private static void Say(uint id, string name, string text) =>
        Chat.Post(new ChatMessage(id, name, ChatChannel.Global, text));
}
