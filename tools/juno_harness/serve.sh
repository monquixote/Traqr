#!/usr/bin/env bash
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

PORT=8081
echo "Starting JUNO harness server at http://localhost:$PORT ..."
echo "Press Ctrl+C to stop."

open "http://localhost:$PORT/index.html" &

python3 -m http.server $PORT
