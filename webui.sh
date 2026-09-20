#!/bin/sh
# Start the web console (Linux / macOS). Extra arguments are passed on, e.g. ./webui.sh --port 9000
exec python3 "$(dirname "$0")/webui/server.py" "$@"
