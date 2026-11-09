# XMB validation — November 9, 2026

## Passed locally

- `tests/xmb_navigation_test.cpp`: compiled with native GCC and executed successfully (exit 0). Covers empty and single-item lists, selection limits, long libraries, selection anchoring, fractional scroll geometry and category/touch alignment.
- `projects/cards/src/CardsActivity.cpp`: devkitA64 C++20 `-fsyntax-only` check passed with `NXUI_BACKEND_DEKO3D` for standalone and `SWITCHU_MENU` configurations using installed libnx and official dependency headers.
- `projects/manager/src/ManagerActivity.cpp`: devkitA64 C++20 production-backend syntax check passed.
- `git diff --check`: passed.
- Inspected the illustrative 1280 × 720 desktop preview for spacing, hierarchy and clipping. The preview uses sample titles, original HomeNX icons and approximated desktop text rendering. It is not an on-console screenshot.

## Not completed

GitHub Actions run [42697072345](https://github.com/Hamza-Adnan/homenx/actions/runs/42697072345) did not allocate runners. GitHub reported: “The job was not started because your account is locked due to a billing issue.” Neither compilation job executed. Re-run the Build HomeNX workflow after resolving the account issue.

No complete link/package build or physical Switch test has been performed. Compiler syntax checks do not verify linking, runtime service access, hardware inputs, launch/return transitions, rendering performance or SD installation. No installable binary is included in this change.

## Hardware checks after building

1. Try `CardsNX.nro` through hbmenu/Sphaira and verify all four categories, a large library, an empty category and custom/hidden categories.
2. Check D-pad and both sticks, held Up/Down repeat, ZL/ZR, toolbar access with Y/B, options/reordering, quick settings and touch swipe/tap behavior.
3. Launch and resume software, return HOME and open system applets in the HOME replacement build.
4. Verify saved Wii U preferences, new-install XMB selection, lock/wake behavior and motion disabled settings.

The upstream three-start frontend fallback and SD recovery method remain in place.
