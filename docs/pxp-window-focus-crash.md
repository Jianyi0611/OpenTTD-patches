# Window focus lifetime regression

The 2026-10-18 09:26:39 UTC crash faults in `EditBoxInGlobalFocus()` while reading a freed window's `nested_focus`. The packaged executable maps the fault to RVA 0xABBA51; the following frames are `VideoDriver_Win32Base::PollEvent()` and `VideoDriver::Tick()`.

Coupling criteria and other child dialogs restore their parent's focus when closing. `Window::Close()` previously cleared its focus before closing its children, allowing that restoration to leave `_focused_window` pointing at a closed parent. Once freed and reused, its widget pointer could contain pixel data (0xFFE1E1E1FFE1E1E1 in this crash).

Close children first, while the parent's widgets and query strings are still available, then clear parent focus and run focus-loss callbacks before discarding the query strings. Assert on destruction that the window is no longer focused.

Regression tests cover overlapping parent/child windows, nested grandchildren, initial parent or child focus, query-string availability during closing callbacks, and closing only the child. Before the fix, all four parent-close combinations leave a dangling focus; after the fix, the complete suite passes (264 cases, 5055 assertions).

The fix touches UI lifetime only; it does not change simulation state, random seeds, savegame fields or network commands. Coupling speed-filter changes in the working tree are retained.
