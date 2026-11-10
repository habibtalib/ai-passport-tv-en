#!/usr/bin/env bash
app_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
"$app_root/start-server.sh" "$@"
result=$?
if [[ $result -ne 0 && -t 0 ]]; then
    read -r -p 'Launch failed. Press Return to close… ' _reply
fi
exit "$result"
