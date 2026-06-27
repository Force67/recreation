using System;
using Recreation.Modding;

namespace Recreation.Games.Skyrim;

// How hungry the player is, in coarse stages other systems and the HUD react to.
public enum HungerStage
{
    Sated,
    Hungry,
    Starving,
}

// Raised when the player's hunger crosses into a new stage.
public readonly struct HungerStageChanged(HungerStage stage) : IGameEvent
{
    public HungerStage Stage { get; } = stage;
}

// A survival hunger model, an optional gameplay system that shows how a complex,
// stateful mechanic composes from the runtime's parts entirely in C#: it rises
// with the game clock, falls when the player eats (detected from the food keyword
// on items added to their inventory), is tunable from config, and announces stage
// changes on the event bus. It does not itself punish starvation; a HUD or effect
// mod subscribes to HungerStageChanged and decides what that means.
public sealed class SurvivalNeeds : GameBehaviour
{
    // Hunger gained per in-game hour.
    public float DrainPerGameHour { get; set; } = 4f;
    // Hunger restored per food item eaten.
    public float RestorePerFood { get; set; } = 25f;
    // Stage thresholds on the 0-100 scale.
    public float HungryAt { get; set; } = 50f;
    public float StarvingAt { get; set; } = 85f;
    // The keyword that marks an item as edible.
    public uint FoodKeyword { get; set; } = SkyrimForms.VendorItemFood;

    public float Hunger { get; private set; }

    public HungerStage Stage =>
        Hunger >= StarvingAt ? HungerStage.Starving :
        Hunger >= HungryAt ? HungerStage.Hungry : HungerStage.Sated;

    private float _lastGameTime = -1f;
    private HungerStage _lastStage = HungerStage.Sated;
    private EventBus.Subscription? _itemSubscription;

    protected override void OnStart()
    {
        _lastGameTime = GameClock.GameTime;
        _itemSubscription = EventBus.Subscribe<ItemAdded>(OnItemAdded);
    }

    protected override void OnDestroy() => _itemSubscription?.Dispose();

    protected override void OnUpdate(float deltaTime)
    {
        float now = GameClock.GameTime;
        float gameHours = (now - _lastGameTime) * 24f;
        _lastGameTime = now;
        if (gameHours > 0f) Adjust(gameHours * DrainPerGameHour);
    }

    private void OnItemAdded(ItemAdded e)
    {
        if (e.ContainerHandle != Game.Player.Handle) return;
        if (!e.Item.HasKeyword(Game.GetForm(FoodKeyword))) return;
        Adjust(-RestorePerFood * e.Count);
    }

    private void Adjust(float delta)
    {
        Hunger = Math.Clamp(Hunger + delta, 0f, 100f);
        if (Stage == _lastStage) return;
        _lastStage = Stage;
        EventBus.Publish(new HungerStageChanged(Stage));
    }
}
