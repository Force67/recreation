using Recreation.Modding;

namespace Recreation.Games.Skyrim;

// Enforces Skyrim's rule that the player cannot fast travel from inside, as
// managed soft logic driven by the LocationChanged event. It holds the
// fast-travel gate closed while indoors through the shared FastTravel
// coordinator, so it composes with the combat and encumbrance locks rather than
// fighting them over the engine flag.
public sealed class IndoorFastTravelLock : GameBehaviour
{
    private EventBus.Subscription? _subscription;

    protected override void OnStart() =>
        _subscription = EventBus.Subscribe<LocationChanged>(OnLocationChanged);

    protected override void OnDestroy()
    {
        _subscription?.Dispose();
        FastTravel.Unblock("indoors");
    }

    private void OnLocationChanged(LocationChanged e)
    {
        if (e.IsInterior) FastTravel.Block("indoors");
        else FastTravel.Unblock("indoors");
    }
}
