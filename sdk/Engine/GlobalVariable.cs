namespace Recreation;

// A global variable (GLOB record): a single engine-wide number, the backing
// store for quest counters, flags and shared tunables.
public class GlobalVariable : Form
{
    protected GlobalVariable(ulong handle) : base(handle) { }

    public static new GlobalVariable From(ulong handle) => new(handle);

    public float Value
    {
        get => Call("GetValue").AsFloat();
        set => Call("SetValue", value);
    }

    public int IntValue
    {
        get => Call("GetValueInt").AsInt();
        set => Call("SetValueInt", value);
    }

    public void Mod(float delta) => Call("Mod", delta);
}
