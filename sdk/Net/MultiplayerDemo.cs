using System;
using Recreation.Modding;

namespace Recreation.Net;

// Built-in showcase of the multiplayer platform reaching the on-screen HUD: chat
// lines and toasts flow C# -> native -> engine HUD. Inert unless REC_MP_DEMO is set.
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

        // Populate a roster and open the scoreboard, so the player list renders too.
        Players.Local.SetName("Dragonborn");
        SetStats(0, 1200, 12);
        AddPlayer(1, "Lydia", 980, 34);
        AddPlayer(2, "Mjoll", 1450, 58);
        AddPlayer(3, "Aela the Huntress", 760, 22);
        Scoreboard.Title = "Recreation RP   |   whiterun-rp.example:29700";
        Scoreboard.Open();

        // A couple of interaction prompts.
        Prompts.Show("trade", "Trade with Belethor", "E");
        Prompts.Show("rob", "Rob the till", "G");

        // Drop blips and spawn objects around the player. Done after a beat, once
        // the player has streamed in and the engine reports its world position.
        Scheduler.After(2.5f, () =>
        {
            Vector3 p = Players.LocalWorldPos;  // engine-space platform position
            Blips.CreateShared("shop", new Vector3(p.X + 26, p.Y, p.Z + 9), "General Store",
                               BlipSprite.Shop, 0x4fd87fffu);     // green
            Blips.CreateShared("job", new Vector3(p.X - 6, p.Y, p.Z + 27), "Job Centre",
                               BlipSprite.Quest, 0xffd24affu);    // gold
            Blips.CreateShared("gang", new Vector3(p.X - 23, p.Y, p.Z - 10), "Bandit Camp",
                               BlipSprite.Enemy, 0xd84f4fffu);    // red

            // Place the other players ahead of us (the camera looks down -Z in the
            // demo cell) so their floating nametags project into view.
            SetPos(1, p.X + 3f, p.Y, p.Z - 11f);
            SetPos(2, p.X - 3f, p.Y, p.Z - 13f);
            SetPos(3, p.X + 0.5f, p.Y, p.Z - 16f);

            // Spawn networked objects ahead of us, in front and above the water.
            NetEntities.Spawn("WRDrawbridge01", new Vector3(p.X + 6f, p.Y + 2f, p.Z - 16f));
            NetEntities.Spawn("WRDrawbridge01", new Vector3(p.X - 6f, p.Y + 2f, p.Z - 18f));
        });
    }

    private static void SetPos(uint id, float x, float y, float z)
    {
        StateBags.Player(id).Set(Nametags.PosX, x);
        StateBags.Player(id).Set(Nametags.PosY, y);
        StateBags.Player(id).Set(Nametags.PosZ, z);
    }

    private static void Say(uint id, string name, string text) =>
        Chat.Post(new ChatMessage(id, name, ChatChannel.Global, text));

    // Mark a player present with a name and stats. In single-player these stay
    // local; the roster and scoreboard pick them up through state-bag observation.
    private static void AddPlayer(uint id, string name, int score, int ping)
    {
        StateBags.Player(id).Set(Player.PresentKey, true);
        StateBags.Player(id).Set(Player.NameKey, name);
        SetStats(id, score, ping);
    }

    private static void SetStats(uint id, int score, int ping)
    {
        StateBags.Player(id).Set("score", score);
        StateBags.Player(id).Set("ping", ping);
    }
}
