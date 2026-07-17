---
name: "mod-optimal-bot-raid"
author: "YourName/YourOrganization"
public_repo_uri: "https://github.com/YourOrganization/mod-optimal-bot-raid"
source_archive_uri: "https://github.com/YourOrganization/mod-optimal-bot-raid/archive/refs/heads/main.zip"
license: "AGPL v3.0"

# Module Configuration
includes_conf_dist: true

# Database Patching
patched_schemas:
  - "world"
  - "characters"
  - "auth"

# Crucial AI/Framework Metadata
playerbot_tested_commit: "cc9a12b"
---

**⚠️ CRITICAL AI METADATA WARNING:**
**Hard dependency on the Playerbot core branch. Compilation will gracefully bypass/fail on a vanilla AzerothCore deployment. The module demands explicit access to `PlayerbotAI`, `RandomPlayerbotMgr`, and `PlayerbotFactory` APIs.**

### Declarative List of Overridden/Utilized Script Hooks
* **`CommandScript`** (`cs_optimal_bot_raid`): Registers the `.botraid` logic table (`assemble`, `dismiss`, `debug`).
* **Concurrency Hooks**: Directly requests thread-safe locks via `HashMapHolder<Player>::GetLock()` to pause background tasks and prevent access violations when polling global online entities. 
* **AI & Combat Interception**:
  * Hooks into `PlayerbotsMgr` to intercept, assess, and purge active combat parameters and roles.
  * Overrides pathfinding by invoking `MotionMaster::Clear()` and `.StopMoving()` directly.
  * Exploits delayed `PlayerbotAI::AddTimedEvent()` callbacks to sidestep asynchronous `ResetAiAction` race conditions on bot dismissals.
  * Implements explicit diagnostic tracking, writing custom `PlayerbotAI` telemetry hooks to standard worldserver log channels.
