#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
bash -n "$root/scripts/build-sign-install-mac.sh"
bash "$root/scripts/build-sign-install-mac.sh" --help >/dev/null
# Exercise install-only on a fake native Mac, including verification failure.
task_tmp="$(mktemp -d)"
trap 'rm -rf "$task_tmp"' EXIT
mkdir -p "$task_tmp/bin" "$task_tmp/input/YodoChoir.vst3/Contents/MacOS" "$task_tmp/installed/YodoChoir.vst3"
touch "$task_tmp/input/YodoChoir.vst3/Contents/MacOS/YodoChoir"
printf old > "$task_tmp/installed/YodoChoir.vst3/previous"
cat > "$task_tmp/bin/uname" <<'STUB'
#!/usr/bin/env bash
if [[ "$1" == -s ]]; then echo Darwin; else echo arm64; fi
STUB
cat > "$task_tmp/bin/ditto" <<'STUB'
#!/usr/bin/env bash
cp -R "$1" "$2"
STUB
cat > "$task_tmp/bin/codesign" <<'STUB'
#!/usr/bin/env bash
if [[ "${FAIL_VERIFY:-0}" == 1 && "$1" == --verify ]]; then exit 1; fi
STUB
cat > "$task_tmp/bin/mv" <<'STUB'
#!/usr/bin/env bash
if [[ "${FAIL_REPLACE:-0}" == 1 && "$1" == */.YodoChoir-install.*/YodoChoir.vst3 ]]; then exit 1; fi
/bin/mv "$@"
STUB
chmod +x "$task_tmp/bin/"*
if PATH="$task_tmp/bin:$PATH" FAIL_VERIFY=1 YODOCHOIR_INSTALL_DIR="$task_tmp/installed" bash "$root/scripts/build-sign-install-mac.sh" --install-only "$task_tmp/input/YodoChoir.vst3" >/dev/null 2>&1; then
  echo 'Verification failure incorrectly accepted'; exit 1
fi
[[ -f "$task_tmp/installed/YodoChoir.vst3/previous" ]]
if PATH="$task_tmp/bin:$PATH" FAIL_REPLACE=1 YODOCHOIR_INSTALL_DIR="$task_tmp/installed" bash "$root/scripts/build-sign-install-mac.sh" --install-only "$task_tmp/input/YodoChoir.vst3" >/dev/null 2>&1; then
  echo 'Replacement failure incorrectly accepted'; exit 1
fi
[[ -f "$task_tmp/installed/YodoChoir.vst3/previous" ]]
PATH="$task_tmp/bin:$PATH" YODOCHOIR_INSTALL_DIR="$task_tmp/installed" bash "$root/scripts/build-sign-install-mac.sh" --install-only "$task_tmp/input/YodoChoir.vst3" >/dev/null
[[ -f "$task_tmp/installed/YodoChoir.vst3/Contents/MacOS/YodoChoir" ]]
[[ ! -f "$task_tmp/installed/YodoChoir.vst3/previous" ]]
echo 'Installer help, syntax, staged replacement and verification/replacement-failure rollback passed (simulated Mac)'
