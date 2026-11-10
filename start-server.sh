#!/usr/bin/env bash
set -euo pipefail
app_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# Finder does not inherit a Homebrew shell's PATH.
export PATH="/opt/homebrew/bin:/usr/local/bin:${PATH}"
if [[ -n "${PYTHON:-}" ]]; then
    candidates=("${PYTHON}")
else
    candidates=(python3 python3.14 python3.13 python3.12 python3.11 python3.10)
fi
for candidate in "${candidates[@]}"; do
    if command -v "$candidate" >/dev/null 2>&1 &&
       "$candidate" -c 'import sys; raise SystemExit(sys.version_info < (3, 10))' 2>/dev/null; then
        exec "$candidate" "$app_root/tools/run_server.py" "$@"
    fi
done
echo 'Python 3.10+ is required. See README.' >&2
exit 1
