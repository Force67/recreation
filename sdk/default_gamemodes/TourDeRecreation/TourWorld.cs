using System;
using System.Collections.Generic;
using Recreation;

namespace Recreation.Games.TourDeRecreation;

// The tour's hands: finding what is already in the world, cloning it, and
// arranging the clones around the player.
//
// Two coordinate spaces meet here and getting them the wrong way round is the
// classic way to spawn a prop a kilometre underground, so the conversion lives
// in one place:
//
//   script space  Bethesda game units, Z-up      (ObjectReference.Position)
//   engine space  metres, Y-up                   (Players.LocalWorldPos)
//
// The tour needs both. It places in script space, because that is what
// PlaceAtMe and Position speak. It reads the player's live viewpoint from
// engine space, because the script-space position of the player reference is
// the one the game authored, not the one the player is standing on.
public sealed class TourWorld
{
    // Metres per game unit, the engine's own constant for this conversion.
    private const float MetresPerUnit = 0.01428f;

    private readonly List<ObjectReference> _placed = new();

    public int PlacedCount => _placed.Count;

    // Where the player actually is, in the units placement speaks.
    public static Vector3 PlayerPosition
    {
        get
        {
            Vector3 e = Recreation.Net.Players.LocalWorldPos;
            // Inverse of the engine's game->engine mapping (x, z, -y).
            return new Vector3(e.X / MetresPerUnit, -e.Z / MetresPerUnit, e.Y / MetresPerUnit);
        }
    }

    // Base forms worth cloning, taken from what is standing around the player
    // right now. Actors are left out: the tour rearranges scenery, and cloning
    // the innkeeper into a ring of innkeepers is a different demo.
    public static List<Form> NearbyBaseForms(float radiusUnits, int max)
    {
        var forms = new List<Form>();
        var seen = new HashSet<ulong>();
        foreach (NearbyRef near in Sorted(Game.Player.RefsNear(radiusUnits)))
        {
            if (forms.Count >= max) break;
            ObjectReference reference = near.Reference;
            if (!reference.Exists || reference.Is("Actor")) continue;
            Form baseForm = reference.BaseObject;
            // A form with no name is usually a marker or a trigger volume: real
            // enough, but nothing the player would see appear.
            if (!baseForm.Exists || string.IsNullOrEmpty(baseForm.Name)) continue;
            if (!seen.Add(baseForm.Handle)) continue;
            forms.Add(baseForm);
        }
        return forms;
    }

    public static NearbyRef[] Sorted(NearbyRef[] refs)
    {
        Array.Sort(refs, (a, b) => a.Distance.CompareTo(b.Distance));
        return refs;
    }

    // Places `count` clones in a ring around the player, cycling through the
    // forms given. Returns how many actually took: a spawn can come back with a
    // null handle (a replica client never spawns), and the tour reports the real
    // number rather than claiming the ring it asked for.
    public int PlaceRing(IReadOnlyList<Form> forms, int count, float radiusUnits, float liftUnits)
    {
        if (forms.Count == 0) return 0;
        Vector3 centre = PlayerPosition;
        int placed = 0;
        for (int i = 0; i < count; i++)
        {
            double angle = i * (Math.PI * 2.0 / count);
            ObjectReference spawned = Game.Player.PlaceAtMe(forms[i % forms.Count]);
            if (!spawned.Exists) continue;
            spawned.Position = RingPoint(centre, angle, radiusUnits, liftUnits);
            _placed.Add(spawned);
            placed++;
        }
        return placed;
    }

    // Turns the whole ring, and rides it up and down, from a phase in seconds.
    // Every prop is moved every frame this is called: the point being made is
    // that world references are live objects a mod owns, not scenery it decorates
    // around.
    public void Orbit(float phase, float radiusUnits, float liftUnits, float bobUnits)
    {
        if (_placed.Count == 0) return;
        Vector3 centre = PlayerPosition;
        for (int i = 0; i < _placed.Count; i++)
        {
            double angle = phase + i * (Math.PI * 2.0 / _placed.Count);
            float bob = bobUnits * MathF.Sin(phase * 1.7f + i * 0.6f);
            _placed[i].Position = RingPoint(centre, angle, radiusUnits, liftUnits + bob);
        }
    }

    private static Vector3 RingPoint(Vector3 centre, double angle, float radius, float lift) =>
        new(centre.X + radius * (float)Math.Cos(angle),
            centre.Y + radius * (float)Math.Sin(angle),
            centre.Z + lift);

    // Takes every clone back out. The tour calls this at its last stop and again
    // on teardown, because a demo that leaves its props lying in someone's world
    // has not finished demonstrating anything.
    public void Clear()
    {
        foreach (ObjectReference reference in _placed)
        {
            if (reference.Exists) reference.Delete();
        }
        _placed.Clear();
    }
}
