# ==============================================================================
# OPTIMAL BOT RAID - CMAKE CONFIGURATION
# ==============================================================================
message(STATUS "Loading custom module: mod-optimal-bot-raid")

# 1. Playerbot Dependency Enforcement
# Fails gracefully if the required Playerbot AI framework is not present in the tree.
set(PLAYERBOT_FOUND FALSE)

if(EXISTS "${CMAKE_SOURCE_DIR}/src/server/game/AI/Playerbot/PlayerbotAI.h")
    set(PLAYERBOT_FOUND TRUE)
elseif(EXISTS "${CMAKE_SOURCE_DIR}/modules/mod-playerbots/src/Bot/PlayerbotAI.h")
    set(PLAYERBOT_FOUND TRUE)
endif()

if(NOT PLAYERBOT_FOUND)
    message(WARNING "\n[mod-optimal-bot-raid] WARNING: Playerbot autonomous agent framework is missing! The module has been DISABLED to prevent compilation failures on vanilla AzerothCore distributions.\n")
    # Tell parent scope to disable this module
    set(MODULE_MOD-OPTIMAL-BOT-RAID "disabled" PARENT_SCOPE)
    return()
endif()

# 2. Configuration Deployment
# Automatically copy any .conf.dist files to the server's binary/etc/modules directory
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/conf")
    file(GLOB conf_files "${CMAKE_CURRENT_LIST_DIR}/conf/*.conf.dist")
    foreach(c_file IN LISTS conf_files)
        install(FILES "${c_file}" DESTINATION "${CMAKE_INSTALL_PREFIX}/etc/modules")
    endforeach()
endif()