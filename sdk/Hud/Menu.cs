using System;
using System.Collections.Generic;
using System.Text;
using Recreation.Interop;

namespace Recreation.Net;

// One row of a menu: a plain action item, or a toggle that flips a bool state and
// reports the new value. A disabled row is shown but does nothing when chosen.
public sealed class MenuItem
{
    public string Label { get; set; }
    public bool Enabled { get; set; } = true;
    public bool IsToggle { get; }
    public bool ToggleState { get; private set; }

    private readonly Action? _onSelect;
    private readonly Action<bool>? _onChanged;

    internal MenuItem(string label, Action onSelect)
    {
        Label = label;
        _onSelect = onSelect;
    }

    internal MenuItem(string label, bool initial, Action<bool> onChanged)
    {
        Label = label;
        IsToggle = true;
        ToggleState = initial;
        _onChanged = onChanged;
    }

    internal void Activate()
    {
        if (IsToggle)
        {
            ToggleState = !ToggleState;
            _onChanged?.Invoke(ToggleState);
        }
        else
        {
            _onSelect?.Invoke();
        }
    }
}

// A data-driven, navigable pop-up menu. Every change re-renders the whole menu
// (no partial-update protocol); opening registers it as HudKit.ActiveMenu.
public sealed class Menu
{
    public string Title { get; }

    private readonly List<MenuItem> _items = new();
    public IReadOnlyList<MenuItem> Items => _items;

    // The focused row's index. Navigation wraps, so it stays in range.
    public int Selected { get; private set; }
    public bool IsOpen { get; private set; }

    public Menu(string title) => Title = title;

    // Add a plain action row; returns the item so callers can chain (e.g. disable it).
    public MenuItem Add(string label, Action onSelect)
    {
        var item = new MenuItem(label, onSelect);
        _items.Add(item);
        if (IsOpen) Render();
        return item;
    }

    // Add a toggle row starting at `initial`; onChanged fires with the new state on
    // each flip. Returns the item for chaining.
    public MenuItem AddToggle(string label, bool initial, Action<bool> onChanged)
    {
        var item = new MenuItem(label, initial, onChanged);
        _items.Add(item);
        if (IsOpen) Render();
        return item;
    }

    public void MoveDown()
    {
        if (_items.Count == 0) return;
        Selected = (Selected + 1) % _items.Count;
        Render();
    }

    public void MoveUp()
    {
        if (_items.Count == 0) return;
        Selected = (Selected - 1 + _items.Count) % _items.Count;
        Render();
    }

    // Activate the focused row. A disabled row is a no-op.
    public void Select()
    {
        if (_items.Count == 0) return;
        MenuItem item = _items[Selected];
        if (!item.Enabled) return;
        item.Activate();
        Render();
    }

    public void Open()
    {
        IsOpen = true;
        HudKit.ActiveMenu = this;
        Render();
    }

    public void Close()
    {
        IsOpen = false;
        if (ReferenceEquals(HudKit.ActiveMenu, this)) HudKit.ActiveMenu = null;
        Native.CallGlobal("Hud", "CloseMenu", new Value[] { Title });
    }

    // Push the whole menu to the HUD in one call. Encoding: (title, focused index,
    // body) where body is a newline-joined list of rows, one per item:
    //   "> "/"  " focus marker, the label, then " [on]"/" [off]" for a toggle.
    // The HUD splits body on '\n'.
    private void Render()
    {
        var body = new StringBuilder();
        for (int i = 0; i < _items.Count; i++)
        {
            if (i > 0) body.Append('\n');
            body.Append(i == Selected ? "> " : "  ");
            body.Append(_items[i].Label);
            if (_items[i].IsToggle) body.Append(_items[i].ToggleState ? " [on]" : " [off]");
        }
        Native.CallGlobal("Hud", "Menu", new Value[] { Title, Selected, body.ToString() });
    }
}
