# Addons development guide

This document tells a developer — or a coding assistant — everything needed to
write an addon for usdtweak. Read it end to end before writing code; the
patterns here are load-bearing.

## What an addon is

An addon is a self-contained feature that lives in its own subdirectory under
`src/addons/`. It registers itself with usdtweak at startup, draws an ImGui UI,
and talks to the running editor only through the frontend API in
`src/addons/Api.h`.

The whole point of the system is **separation**: usdtweak can be updated without
touching your addon, and your addon can be added, removed, or shared by dropping
or deleting a single folder. To preserve that property, an addon must obey two
rules:

1. **Prefer `addons/Api.h`** for everything related to the editor. It is the
   stable contract — usdtweak will try to keep it the same across updates. You
   *may* include other parts of the application (`Editor.h`, widgets, the
   viewport, etc.) when the API doesn't cover what you need, but with a caveat:
   those internals can change between versions, and when they do you'll have to
   update your addon. (`GetEditor()` is the in-API escape hatch to the live
   `Editor`.) If you find yourself needing access that `Api.h` doesn't provide,
   **please open an issue on GitHub** — requests to extend the API are very
   welcome, and that's how the stable surface grows.
2. **Never edit files outside your own addon folder.** Adding an addon must not
   require changes to `src/addons/CMakeLists.txt`, the `Editor`, or any shared
   file. If you find yourself wanting to, stop and reconsider — the API is
   probably missing something and the host should be extended deliberately
   (open an issue), not patched from an addon.

## Anatomy of an addon

A minimal addon is two files in a new folder:

```
src/addons/MyAddon/
  CMakeLists.txt
  MyAddon.cpp
```

### `CMakeLists.txt`

One line. `usdtweak_add_addon` is a helper defined in
`src/addons/CMakeLists.txt`; it compiles your sources directly into the
`usdtweak` executable and adds your folder to the include path.

```cmake
usdtweak_add_addon(MyAddon
    SOURCES
        MyAddon.cpp
)
```

List every `.cpp`/`.h` your addon needs under `SOURCES`. If you need an extra
USD or third-party library beyond what usdtweak already links, that is a sign
you may be over-reaching for an addon — most of USD is already linked into the
target and available.

### `MyAddon.cpp`

```cpp
#include "addons/Api.h"

#include "Gui.h"   // ImGui + usdtweak icon macros (ICON_FA_*), if you draw UI

namespace {

void DrawMyAddon() {
    ImGui::Text("Current stage: %s",
                usdtweak::GetCurrentStage() ? "loaded" : "none");
}

} // namespace

TF_REGISTRY_FUNCTION_WITH_TAG(UsdTweakAddonRegistry, MyAddon) {
    UsdTweakAddon addon;
    addon.id = "MyAddon";                 // unique, stable; used as settings key
    addon.menuLabel = "My addon";         // shown in the Tools menu + window title
    addon.kind = UsdTweakAddon::Kind::Window;
    addon.draw = &DrawMyAddon;
    UsdTweakAddonRegistry::GetInstance().Add(std::move(addon));
}
```

That's a complete addon. Re-run cmake (see *Building* below) and it appears in
the **Tools** menu.

## How registration works

Addons use USD's registry mechanism rather than a `plugInfo.json`:

- Each addon translation unit declares a
  `TF_REGISTRY_FUNCTION_WITH_TAG(UsdTweakAddonRegistry, <UniqueTag>)` body.
- At startup the `Editor` constructor calls
  `TfRegistryManager::GetInstance().SubscribeTo<UsdTweakAddonRegistry>()`
  (wrapped as `UsdTweakAddonRegistry::SubscribeAll()`), which fires every
  registered body exactly once. Each body calls
  `UsdTweakAddonRegistry::GetInstance().Add(...)`.
- The `<UniqueTag>` (second macro argument) must be unique across the whole
  program. Use your addon's name. The `addon.id` string must also be unique and
  stable — it keys persisted settings, so don't rename it casually.

Because of the `_ARCH_ENSURE_PER_LIB_INIT` shim baked into the macro, the
registration survives linker stripping even though addons are compiled into the
executable.

## Folder auto-discovery (do not edit the parent CMakeLists)

`src/addons/CMakeLists.txt` globs every `*/CMakeLists.txt` under `src/addons/`
with `CONFIGURE_DEPENDS` and `add_subdirectory`s each one. **Dropping in a new
folder is all that's required** — the next `cmake --build` picks it up
automatically. Deleting a folder removes the addon. Never list your addon by
hand in the parent file.

## The `UsdTweakAddon` descriptor

Defined in `src/addons/AddonRegistry.h`:

```cpp
struct UsdTweakAddon {
    enum class Kind { Window, Action };

    std::string id;                    // unique, stable; settings key
    std::string menuLabel;             // menu entry + window title
    Kind kind = Kind::Window;

    std::function<void()> draw;        // Kind::Window: body, drawn inside Begin/End
    std::function<void()> activate;    // Kind::Action: fired on menu click

    std::function<bool()> isAvailable; // optional; false => menu item greyed out
    bool defaultOpen = false;          // Kind::Window only
    ImGuiWindowFlags windowFlags = 0;  // Kind::Window only
};
```

### `Kind::Window`

A dockable ImGui window. The editor:

- wraps your `draw` callback in `ImGui::Begin(menuLabel, &open, windowFlags)` /
  `ImGui::End()` — **do not call `Begin`/`End` yourself**; just draw widgets.
- shows a checkable entry in the Tools menu that toggles the window.
- persists the open/closed state automatically under `Addon.<id>.open`, so the
  window reopens on the next launch if it was open.
- honors `defaultOpen` the first time (no saved setting yet) and `windowFlags`
  (e.g. `ImGuiWindowFlags_MenuBar`).

### `Kind::Action`

A one-shot menu item. Clicking it runs `activate` once. Typical use: open a
modal dialog. Set `draw` to nothing; set `activate`.

### `isAvailable`

Optional predicate evaluated every frame the menu is drawn. Return `false` to
grey out the entry (e.g. "no stage loaded", "Storm not available"). For
`Kind::Window`, an unavailable window is also skipped when drawing. Keep it
cheap — it runs on every menu draw.

## The frontend API (`src/addons/Api.h`)

All functions live in `namespace usdtweak`. This is the entire supported surface
for talking to the editor.

### Read editor state

```cpp
UsdStageRefPtr   usdtweak::GetCurrentStage();
SdfLayerRefPtr   usdtweak::GetCurrentLayer();
UsdEditTarget    usdtweak::GetCurrentEditTarget();
UsdStageCache   &usdtweak::GetStageCache();
UsdTimeCode      usdtweak::GetCurrentTimeCode();
const Selection &usdtweak::GetSelection();
```

Always null-check `GetCurrentStage()` / `GetCurrentLayer()` — there may be no
stage or layer open.

### Mutate editor state (undoable)

These route through the editor's command system, so they participate in
undo/redo:

```cpp
void usdtweak::SetCurrentStage(UsdStageRefPtr stage);
void usdtweak::SetCurrentLayer(SdfLayerRefPtr layer);
void usdtweak::SetCurrentEditTarget(SdfLayerHandle layer);

void usdtweak::SetStagePathSelection(const SdfPath &primPath);  // replace selection
void usdtweak::AddStagePathSelection(const SdfPath &primPath);  // extend selection
void usdtweak::SetLayerPathSelection(const SdfPath &primPath);
void usdtweak::AddLayerPathSelection(const SdfPath &primPath);

void usdtweak::OpenStage(const std::string &path);
void usdtweak::FindOrOpenLayer(const std::string &path);
void usdtweak::CreateStage(const std::string &path);

void usdtweak::FrameCameraOnSelection();
```

### Search

```cpp
std::vector<SdfPath> usdtweak::SearchPrimsByName(const std::string &needle,
                                                 uint32_t limit = 0);
```

Substring search over the editor's prim-name index (current stage + layer prim
names). `limit == 0` means unbounded.

### Per-addon persisted settings

A simple key/value bag stored in the same `.ini` as the editor's settings, under
the prefix `Addon.<addonId>.<key>`. Pass your own `addon.id` as `addonId`.

```cpp
bool        usdtweak::GetAddonBool  (const std::string &id, const std::string &key, bool def = false);
void        usdtweak::SetAddonBool  (const std::string &id, const std::string &key, bool value);
std::string usdtweak::GetAddonString(const std::string &id, const std::string &key, const std::string &def = "");
void        usdtweak::SetAddonString(const std::string &id, const std::string &key, const std::string &value);
```

Only bool and string are provided. To store numbers or lists, serialize to
strings yourself (see the LauncherBar addon, which stores a `count` plus indexed
`launcher.<i>.name` / `launcher.<i>.cmd` keys).

### Escape hatch

```cpp
Editor *usdtweak::GetEditor();
```

Returns the live `Editor*`. Reach for it (or for other host headers) when the
curated API can't express what you need — just be aware those internals may
change between versions and force an addon update. If you hit this, it's worth
opening a GitHub issue so the missing capability can be added to the stable
`Api.h`.

## Editing the stage with undo support

Do **not** call mutating USD APIs (e.g. `prim.SetActive(...)`,
`attr.Set(...)`, `stage->DefinePrim(...)`) directly from your `draw` callback.
Direct edits bypass undo/redo and run mid-frame. Instead defer them through
`ExecuteAfterDraw`, re-exported from `commands/Commands.h` via `Api.h`:

```cpp
// Generic form: bind a USD member function to a stage/layer/prim/attribute.
// The change is recorded on the edit target's layer and pushed onto the
// undo stack.
ExecuteAfterDraw(&UsdPrim::SetActive, prim, false);
ExecuteAfterDraw(&UsdAttribute::Set, attribute, value, UsdTimeCode::Default());
```

There are overloads keyed on the second argument type (`SdfLayerRefPtr`,
`UsdStageRefPtr`, `SdfPrimSpecHandle`, `const UsdPrim&`, `const UsdAttribute&`);
each captures the path safely so the call is valid when it runs after the frame.
See `src/commands/Commands.h` for all forms. The state-mutation API functions
above already use this internally, so calling them from `draw` is safe.

## Modal dialogs

`Api.h` re-exports the modal helpers from `widgets/ModalDialogs.h`. Define a
subclass of `ModalDialog`, then trigger it with `DrawModalDialog<T>(...)`
(constructor args are forwarded). Use a `Kind::Action` addon's `activate`, or a
button inside a `Kind::Window`, to trigger it.

```cpp
struct MyDialog : public ModalDialog {
    void Draw() override {
        ImGui::InputText("Name", &_name);
        DrawModalButtonsOkCancel([&]() {
            // ... apply on OK ...
        });
    }
    const char *DialogId() const override { return "My dialog"; }
    std::string _name;
};

// trigger it:
DrawModalDialog<MyDialog>();
```

Override `WindowFlags()`, `PrepareModal()` (for size/pos), or `PushStyles()` for
customization. Call `CloseModal()` to dismiss from inside `Draw()`.

## Reacting to editor changes (notices)

The editor sends `TfNotice`s when its top-level state changes
(`src/addons/Notices.h`):

- `UsdTweakCurrentStageChangedNotice`
- `UsdTweakCurrentLayerChangedNotice`
- `UsdTweakCurrentEditTargetChangedNotice`
- `UsdTweakSelectionChangedNotice`

Subscribe from a class deriving `TfWeakBase`:

```cpp
class MyAddonListener : public TfWeakBase {
public:
    MyAddonListener() {
        TfNotice::Register(TfCreateWeakPtr(this),
                           &MyAddonListener::OnStageChanged);
    }
    void OnStageChanged(const UsdTweakCurrentStageChangedNotice &) {
        // refresh cached state
    }
};
```

For fine-grained stage edits, subscribe directly to USD-native notices
(`UsdNotice::StageContentsChanged`, `ObjectsChanged`, etc.) — those come from
USD itself, not from this layer. Note: `UsdTweakSelectionChangedNotice` is only
sent by the `Editor::Set/Add*PathSelection` paths; selection changes made
elsewhere may not fire it.

## ImGui drawing notes

- Include `"Gui.h"` for ImGui plus the bundled Font-Awesome icon macros
  (`ICON_FA_IMAGES`, etc.); prefix a `menuLabel` or button with an icon as the
  existing addons do.
- For `Kind::Window`, draw widgets directly — the editor already opened the
  window. For child regions, tables, etc., use ImGui normally.
- Keep per-frame work cheap; `draw` runs every frame the window is open. Cache
  expensive results (see the ShaderRegistryInspector, which builds its shader
  list once behind an `initialized` flag and only re-filters when the query
  changes).

## Study the bundled addons

Three addons ship as worked examples — read them, they exercise every part of
the system:

- `src/addons/ShaderRegistryInspector/` — `Kind::Window`. Cached data, filtering,
  sortable tables, tooltips, version guards (`PXR_VERSION`).
- `src/addons/StormPlayblast/` — `Kind::Action` + `isAvailable`. Opens a modal
  dialog, reads the current stage, renders with `UsdAppUtilsFrameRecorder`.
- `src/addons/LauncherBar/` — `Kind::Window`. Per-addon settings persistence
  (serializing a list), a modal add-dialog, async `std::system` execution with
  self-cleaning futures, token substitution from editor state.

## Building & running

Build usdtweak as usual (see `doc/Building.md`) — for an existing build
directory it's just:

```bash
cmake --build <build-dir>
```

The first build after adding a new addon folder re-runs cmake automatically
(`CONFIGURE_DEPENDS`). Then launch the app from that build directory; your addon
appears under the **Tools** menu.

## Checklist for a new addon

- [ ] New folder `src/addons/<Name>/` with `CMakeLists.txt` + source(s).
- [ ] `CMakeLists.txt` calls `usdtweak_add_addon(<Name> SOURCES ...)`.
- [ ] Source talks to the editor through `addons/Api.h` where possible (other
      host headers are allowed but may change between versions).
- [ ] `TF_REGISTRY_FUNCTION_WITH_TAG(UsdTweakAddonRegistry, <UniqueTag>)` body
      that `Add`s a `UsdTweakAddon` with a unique, stable `id`.
- [ ] Stage/layer pointers null-checked before use.
- [ ] Mutations go through the API or `ExecuteAfterDraw` (never raw USD edits in
      `draw`).
- [ ] No files outside the addon folder were modified.
