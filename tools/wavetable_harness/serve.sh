#!/usr/bin/env bash
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

PORT=8082
echo "Starting Wavetable harness server at http://localhost:$PORT ..."
echo "Press Ctrl+C to stop."

open "http://localhost:$PORT/index.html" &

python3 -m http.server $PORT
