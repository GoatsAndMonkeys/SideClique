#!/bin/bash
# SideClique Integration Script
# Copies ONLY SideClique module files into the Meshtastic firmware tree
# No TinyBBS games, no survival guide, no wordle — just P2P mesh comms

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
MODULE_SRC="$PROJECT_ROOT/module-src"
FIRMWARE_ROOT="$PROJECT_ROOT/firmware"
FIRMWARE_MODULES="$FIRMWARE_ROOT/src/modules"
MODULES_CPP="$FIRMWARE_MODULES/Modules.cpp"

echo "=== SideClique Integration ==="

# Remove any TinyBBS leftovers
echo "Cleaning TinyBBS leftovers..."
rm -f "$FIRMWARE_MODULES"/BBS*.h "$FIRMWARE_MODULES"/BBS*.cpp
rm -f "$FIRMWARE_MODULES"/Fallout*.h "$FIRMWARE_MODULES"/Fallout*.cpp
rm -f "$FIRMWARE_MODULES"/MudGame*.h "$FIRMWARE_MODULES"/MudGame*.cpp
rm -f "$FIRMWARE_MODULES"/MudWorld*.h
sed -i '' '/#include "BBSModule_v2.h"/d' "$MODULES_CPP" 2>/dev/null
sed -i '' '/bbsModule = new BBSModule/d' "$MODULES_CPP" 2>/dev/null

# Copy SideClique files
echo "Copying SideClique files..."
cp "$MODULE_SRC/SideClique.h" "$FIRMWARE_MODULES/"
cp "$MODULE_SRC/SideClique.cpp" "$FIRMWARE_MODULES/"

# External flash driver (for geo lookup + future data)
cp "$MODULE_SRC/BBSExtFlash.h" "$FIRMWARE_MODULES/"
cp "$MODULE_SRC/BBSExtFlash.cpp" "$FIRMWARE_MODULES/"
cp "$MODULE_SRC/BBSPlatform.h" "$FIRMWARE_MODULES/"

# Geo lookup (optional — for city names in check-in board)
cp "$MODULE_SRC/BBSGeoLookup.h" "$FIRMWARE_MODULES/"
cp "$MODULE_SRC/BBSGeoDB.h" "$FIRMWARE_MODULES/"

# Chess by Mail
cp "$MODULE_SRC/BBSChess.h" "$FIRMWARE_MODULES/" 2>/dev/null
cp "$MODULE_SRC/BBSChess.cpp" "$FIRMWARE_MODULES/" 2>/dev/null

# Wordle
cp "$MODULE_SRC/BBSWordle.h" "$FIRMWARE_MODULES/" 2>/dev/null

# Wastelad RPG
cp "$MODULE_SRC/FalloutWastelandRPG.h" "$FIRMWARE_MODULES/" 2>/dev/null
cp "$MODULE_SRC/FalloutWastelandRPG.cpp" "$FIRMWARE_MODULES/" 2>/dev/null
cp "$MODULE_SRC/SCDailyQuest.h" "$FIRMWARE_MODULES/" 2>/dev/null
cp "$MODULE_SRC/SCHacking.h" "$FIRMWARE_MODULES/" 2>/dev/null

echo "✓ Module files copied"

# Patch Modules.cpp
echo "Patching Modules.cpp..."

if grep -q "#include \"SideClique.h\"" "$MODULES_CPP"; then
    sed -i '' '/#include "SideClique.h"/d' "$MODULES_CPP"
fi
if grep -q "sideClique = new SideClique()" "$MODULES_CPP"; then
    sed -i '' '/sideClique = new SideClique()/d' "$MODULES_CPP"
fi

LAST_PLAIN_INCLUDE=$(grep -n "^#include" "$MODULES_CPP" | tail -1 | cut -d: -f1)
if [ -n "$LAST_PLAIN_INCLUDE" ]; then
    INSERT_LINE=$((LAST_PLAIN_INCLUDE + 1))
    while [ $INSERT_LINE -le $(wc -l <"$MODULES_CPP") ]; do
        LINE=$(sed -n "${INSERT_LINE}p" "$MODULES_CPP")
        if [[ "$LINE" =~ ^#(if|else|endif) ]]; then
            INSERT_LINE=$((INSERT_LINE + 1))
        else
            break
        fi
    done
    sed -i '' "${INSERT_LINE}i\\
#include \"SideClique.h\"
" "$MODULES_CPP"
    echo "✓ Added SideClique include"
fi

SETUP_MODULES_LINE=$(grep -n "void setupModules()" "$MODULES_CPP" | head -1 | cut -d: -f1)
if [ -n "$SETUP_MODULES_LINE" ]; then
    BRACE_LINE=$((SETUP_MODULES_LINE))
    while [ $BRACE_LINE -le $(wc -l <"$MODULES_CPP") ]; do
        LINE=$(sed -n "${BRACE_LINE}p" "$MODULES_CPP")
        if [[ "$LINE" == *"{"* ]]; then break; fi
        BRACE_LINE=$((BRACE_LINE + 1))
    done
    INSERTAFTER=$BRACE_LINE
    sed -i '' "${INSERTAFTER}a\\
    sideClique = new SideClique();
" "$MODULES_CPP"
    echo "✓ Added SideClique instantiation"
fi

echo ""
echo "=== SideClique Integration Complete ==="
echo "Files: SideClique.h/.cpp, BBSExtFlash.h/.cpp, BBSPlatform.h, BBSGeoLookup.h, BBSGeoDB.h"
echo "Next: cd firmware && pio run -e t-echo"
