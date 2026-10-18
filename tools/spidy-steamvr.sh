#!/usr/bin/env bash
# Start Spidy VR with SteamVR; see spidy-linux.sh for the steps.
SPIDY_RUNTIME=steamvr exec bash "$(dirname "${BASH_SOURCE[0]}")/spidy-linux.sh" "$@"
