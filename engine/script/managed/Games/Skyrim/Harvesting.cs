using Recreation.Modding;

namespace Recreation.Games.Skyrim;

// Raised when the player harvests a flora, carrying the produce.
public readonly struct FloraHarvested(ulong floraHandle, ulong ingredientHandle) : IGameEvent
{
    public ulong FloraHandle { get; } = floraHandle;
    public ulong IngredientHandle { get; } = ingredientHandle;

    public Form Ingredient => Form.From(IngredientHandle);
}

// Harvesting flora, the Skyrim mechanic where activating a plant gives you its
// ingredient and the plant is picked. Driven by the player-activate hook: when
// the activated reference's base is a flora that produces something, the produce
// is added to the player and the plant is disabled. Pure managed soft logic over
// the activate event, the form's record data and the inventory.
public sealed class Harvesting : GameBehaviour
{
    private const int FloraFormType = 40;  // FLOR

    private EventBus.Subscription? _subscription;

    protected override void OnStart() =>
        _subscription = EventBus.Subscribe<PlayerActivated>(OnActivated);

    protected override void OnDestroy() => _subscription?.Dispose();

    private void OnActivated(PlayerActivated e)
    {
        ObjectReference target = e.Target;
        Form flora = target.BaseObject;
        if (flora.FormType != FloraFormType) return;

        Form ingredient = flora.HarvestIngredient;
        if (!ingredient.Exists) return;

        Game.Player.AddItem(ingredient, 1);
        target.Enabled = false;  // the plant is picked
        EventBus.Publish(new FloraHarvested(target.Handle, ingredient.Handle));
    }
}
