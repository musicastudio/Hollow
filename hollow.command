#!/bin/sh
# Hollow launcher for macOS: double-click for the menu, or ./hollow.command <command> (see hollow.py).
cd "$(dirname "$0")" && exec python3 hollow.py "$@"
