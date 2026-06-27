using System.Collections.Generic;
using System.Linq;
using Recreation;
using Recreation.Interop;

namespace Recreation.Tests;

// Covers inventory enumeration: a mod can list the distinct forms a container
// holds, the basis for sorting, scanning and auto-equip logic.
public static class InventoryTests
{
    public static void Run(Check check)
    {
        var fake = new FakeBackend();
        Native.Backend = fake;

        const ulong chest = 0x500;
        check.Equal("empty container has no items", 0, ObjectReference.From(chest).ItemCount);

        fake.AddInventoryItem(chest, 0x11);
        fake.AddInventoryItem(chest, 0x22);
        fake.AddInventoryItem(chest, 0x33);

        ObjectReference reference = ObjectReference.From(chest);
        check.Equal("item count reflects contents", 3, reference.ItemCount);

        List<ulong> handles = reference.Items().Select(f => f.Handle).ToList();
        check.Equal("enumerates every item", 3, handles.Count);
        check.That("contains the added forms",
                   handles.Contains(0x11) && handles.Contains(0x22) && handles.Contains(0x33));

        // Loot-all: move everything to the player, leaving the chest empty.
        fake.AddInventoryItem(chest, 0x22, count: 4);  // 5 total of 0x22 now
        const ulong player = 0x14;
        reference.TransferAllItemsTo(ObjectReference.From(player));
        check.Equal("source emptied after loot-all", 0, reference.ItemCount);
        check.Equal("all distinct forms moved", 3, ObjectReference.From(player).ItemCount);
        check.Equal("stacked count moved", 5,
                    ObjectReference.From(player).GetItemCount(Form.From(0x22)));

        Native.Backend = null;
    }
}
