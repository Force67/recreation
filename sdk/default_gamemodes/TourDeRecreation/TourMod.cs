using Recreation;
using Recreation.Modding;

namespace Recreation.Games.TourDeRecreation;

// Tour de Recreation: the guided demo, shipped as an optional gamemode so it
// shows up as its own tile on the front screen and only loads when someone picks
// it. It is a first-party mod and nothing more -- the same [Mod] entry point,
// the same boot scan, the same SDK as anything a player would write.
//
// Game-agnostic on purpose. The tile is mounted on Skyrim because that is the
// world most people have, but every call the tour makes exists in all three
// domains, so arming it on Fallout or Starfield runs the same seven stops.
[Mod("TourDeRecreation", Author = "Recreation", Version = "1.0.0")]
public sealed class TourMod : IMod
{
    public void OnLoad()
    {
        if (Domains.Primary == null)
        {
            Debug.Trace("[tour] no primary world; not arming");
            return;
        }

        ModConfig config = ModConfig.Load("TourDeRecreation");
        ModHost.Register(new TourDirector
        {
            LeadIn = config.GetFloat("leadInSeconds", 3f),
            MinNeighbours = config.GetInt("minNeighbours", 6),
            Settle = config.GetFloat("settleSeconds", 25f),
            RingSize = config.GetInt("ringSize", 12),
            RingRadius = config.GetFloat("ringRadiusUnits", 420f),
            RingLift = config.GetFloat("ringLiftUnits", 90f),
            RingBob = config.GetFloat("ringBobUnits", 34f),
            SearchRadius = config.GetFloat("searchRadiusUnits", 6000f),
            NudgeStep = config.GetFloat("nudgeStepUnits", 120f),
        });
        Debug.Trace($"[tour] loaded on {Domains.Primary.Name}");
    }
}
