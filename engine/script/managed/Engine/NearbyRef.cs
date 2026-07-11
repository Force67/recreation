namespace Recreation;

// A reference found by a proximity query, with its live distance (game units)
// from the query centre. Sort by Distance for the nearest, or filter Reference
// by type. Returned by ObjectReference.RefsNear.
public readonly struct NearbyRef(ObjectReference reference, float distance)
{
    public ObjectReference Reference { get; } = reference;
    public float Distance { get; } = distance;
}
