namespace Recreation;

// A cell (CELL record): an interior room or an exterior worldspace tile.
public class Cell : Form
{
    protected Cell(ulong handle) : base(handle) { }

    public static new Cell From(ulong handle) => new(handle);

    public bool IsInterior => Call("IsInterior").AsBool();
    public float WaterLevel => Call("GetWaterLevel").AsFloat();
}
