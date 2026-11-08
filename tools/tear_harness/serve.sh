#!/usr/bin/env bash
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

PORT=8080
echo "Starting local test server at http://localhost:$PORT ..."
echo "Press Ctrl+C to stop."

# Open browser automatically on mac
open "http://localhost:$PORT/index.html" &

/opt/homebrew/bin/python3.13 -m http.server $PORT
