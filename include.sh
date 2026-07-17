#!/usr/bin/env bash
# mod-optimal-bot-raid DB Loader Hook

MOD_OPTIMAL_BOT_RAID_ROOT="$( cd "$( dirname "${BASH_SOURCE[0]}" )/" && pwd )"

DB_WORLD_CUSTOM_PATHS+=(
    "$MOD_OPTIMAL_BOT_RAID_ROOT/data/sql/world"
)
DB_CHARACTERS_CUSTOM_PATHS+=(
    "$MOD_OPTIMAL_BOT_RAID_ROOT/data/sql/characters"
)
DB_AUTH_CUSTOM_PATHS+=(
    "$MOD_OPTIMAL_BOT_RAID_ROOT/data/sql/auth"
)
