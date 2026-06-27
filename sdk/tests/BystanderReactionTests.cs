using System.Collections.Generic;
using Recreation;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Tests;

// Covers the bystander layer: a death alarms the living within earshot (not the
// dead, not the far-off), a theft alarms the actors near the looted container (but
// not the container), and removing the behaviour stops the reactions.
public static class BystanderReactionTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;
        ModHost.Shutdown();
        EventBus.Clear();

        var alarms = new List<BystanderAlarmed>();
        EventBus.Subscribe<BystanderAlarmed>(alarms.Add);

        var bystanders = new BystanderReaction { DeathAlarmRadius = 800f, TheftAlarmRadius = 300f };
        ModHost.Register(bystanders);

        // --- a death alarms the living within earshot ----------------------------
        const ulong victim = 0x50, nearWitness = 0x51, deadWitness = 0x52, farWitness = 0x53;
        fake.SetPosition(victim, 0, 0, 0);
        fake.SetPosition(nearWitness, 200, 0, 0);   // within earshot
        fake.SetPosition(deadWitness, 100, 0, 0);   // within earshot but a corpse
        fake.SetPosition(farWitness, 5000, 0, 0);   // too far
        fake.SetActorDead(deadWitness, true);        // a corpse does not react

        EventBus.Publish(new ActorDied(victim));
        check.Equal("only the near, living witness is alarmed", 1, alarms.Count);
        check.Equal("the death alarm names the witness", nearWitness, alarms[0].WitnessHandle);
        check.Equal("the reason is death", AlarmReason.Death, alarms[0].Reason);
        check.Equal("the subject is the victim", victim, alarms[0].SubjectHandle);

        // --- a theft alarms the actors near the looted container -----------------
        alarms.Clear();
        const ulong chest = 0x60, owner = 0x61, passerby = 0x62, stolen = 0x70;
        fake.SetPosition(chest, 1000, 0, 0);
        fake.SetPosition(owner, 1100, 0, 0);      // near the chest
        fake.SetPosition(passerby, 1200, 0, 0);   // near the chest
        // The container itself sits in range of its own query but must not alarm.

        EventBus.Publish(new ItemRemoved(chest, stolen, 1));
        check.Equal("both nearby actors are alarmed by the theft", 2, alarms.Count);
        check.That("the owner is alarmed",
                   alarms.Exists(a => a.WitnessHandle == owner && a.Reason == AlarmReason.Theft));
        check.That("the passerby is alarmed",
                   alarms.Exists(a => a.WitnessHandle == passerby));
        check.That("the container did not cry out",
                   !alarms.Exists(a => a.WitnessHandle == chest));
        check.That("the theft subject is the stolen item",
                   alarms.TrueForAll(a => a.SubjectHandle == stolen));

        // --- teardown stops the reactions ----------------------------------------
        ModHost.Unregister(bystanders);
        alarms.Clear();
        EventBus.Publish(new ActorDied(victim));
        check.Equal("no reactions after teardown", 0, alarms.Count);

        ModHost.Shutdown();
        EventBus.Clear();
        Native.Backend = null;
    }
}
