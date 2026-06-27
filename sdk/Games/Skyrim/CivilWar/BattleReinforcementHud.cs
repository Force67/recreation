using Recreation.Modding;

namespace Recreation.Games.Skyrim;

// The siege reinforcement HUD: while a Civil War city battle runs, two bars track
// each army's remaining reinforcement pool, the modern read-out of who is winning.
// It polls on an interval (the pools drain slowly) and clears the bars the moment
// no battle is active, so the HUD is empty in peacetime.
public sealed class BattleReinforcementHud : GameBehaviour
{
    // Seconds between HUD refreshes; the reinforcement pools change slowly.
    public float RefreshInterval { get; set; } = 0.5f;

    // Packed rgba8 bar colours: attackers in war-red, defenders in steel-blue.
    private const uint AttackerColor = 0xc83c3cffu;
    private const uint DefenderColor = 0x3c6ec8ffu;
    private const string AttackerGauge = "cw_attackers";
    private const string DefenderGauge = "cw_defenders";

    private float _timer;
    private bool _shown;

    protected override void OnUpdate(float deltaTime)
    {
        _timer += deltaTime;
        if (_timer < RefreshInterval) return;
        _timer = 0f;

        GlobalVariable attacker = Game.GetGlobal(CivilWarForms.PercentPoolRemainingAttacker);
        GlobalVariable defender = Game.GetGlobal(CivilWarForms.PercentPoolRemainingDefender);
        if (!BattleActive() || !attacker.Exists || !defender.Exists)
        {
            Clear();
            return;
        }

        // The pool globals are 0..100 percentages; the gauge fraction is 0..1.
        Hud.Gauge(AttackerGauge, attacker.Value / 100f, "Attackers", AttackerColor);
        Hud.Gauge(DefenderGauge, defender.Value / 100f, "Defenders", DefenderColor);
        _shown = true;
    }

    protected override void OnDestroy() => Clear();

    // A siege is on when its battle quest is live.
    private static bool BattleActive()
    {
        Quest siege = Game.GetQuest(CivilWarForms.SiegeQuest);
        return siege.Exists && siege.IsRunning;
    }

    private void Clear()
    {
        if (!_shown) return;
        Hud.ClearGauge(AttackerGauge);
        Hud.ClearGauge(DefenderGauge);
        _shown = false;
    }
}
