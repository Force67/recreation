using Recreation;
using Recreation.Games.Skyrim;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers the Civil War enlistment prompt's lifecycle: pressing a hotkey enlists
// without error, the side settles through the allegiance tracker, the prompt
// then retires (so its keys go inert), and a prompt registered after the player
// has already committed offers nothing.
public static class CivilWarEnlistmentTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        EventBus.Clear();
        CivilWarState.Reset();

        ModHost.Register(new CivilWarAllegianceTracker());
        ModHost.Register(new CivilWarEnlistment());
        check.Equal("starts unaligned", CivilWarSide.None, CivilWarState.CurrentSide);

        // Pressing F2 runs the enlistment (it asks the engine to advance the
        // Stormcloak intro quest). The engine then fires that quest's stage event;
        // here we stand in for it so the tracker can settle the side.
        EventBus.Publish(new KeyPressed(Key.F2));
        EventBus.Publish(new QuestStageChanged(CivilWarForms.JoinStormcloakQuest, 10));
        check.Equal("F2 enlists with the Stormcloaks", CivilWarSide.Stormcloak,
                    CivilWarState.CurrentSide);

        // The prompt retired its hotkeys when the side settled, so F1 is inert.
        EventBus.Publish(new KeyPressed(Key.F1));
        check.Equal("the prompt cannot flip a committed side", CivilWarSide.Stormcloak,
                    CivilWarState.CurrentSide);

        // A prompt registered after the player has committed offers nothing.
        ModHost.Register(new CivilWarEnlistment());
        EventBus.Publish(new KeyPressed(Key.F1));
        check.Equal("a late prompt stays inert", CivilWarSide.Stormcloak, CivilWarState.CurrentSide);

        ModHost.Shutdown();
        EventBus.Clear();
        CivilWarState.Reset();
        Native.Backend = null;
    }
}
