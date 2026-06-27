using Recreation.Modding;

namespace Recreation.Games.Skyrim;

// Publishes a GameHourStarted event each time the in-game clock rolls into a new
// hour, turning the continuously-advancing game time into discrete cues mods can
// hook. It tracks the last hour and day it announced, so it fires once per hour
// and handles the midnight roll-over naturally. This is the backbone other mods
// build schedules on (shops closing at night, NPCs heading home).
public sealed class TimeOfDayService : GameBehaviour
{
    private int _lastHour = -1;
    private int _lastDay = -1;

    protected override void OnUpdate(float deltaTime)
    {
        int hour = GameClock.Hour;
        int day = GameClock.Day;
        if (hour == _lastHour && day == _lastDay) return;
        _lastHour = hour;
        _lastDay = day;
        EventBus.Publish(new GameHourStarted(hour, day));
    }
}
