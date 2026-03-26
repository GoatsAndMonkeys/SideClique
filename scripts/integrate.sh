#!/bin/bash
# SideClique Integration Script
# Copies module source files into the Meshtastic firmware tree

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
MODULE_SRC="$PROJECT_ROOT/module-src"
FIRMWARE_ROOT="$PROJECT_ROOT/firmware"
FIRMWARE_MODULES="$FIRMWARE_ROOT/src/modules"
MODULES_CPP="$FIRMWARE_MODULES/Modules.cpp"

echo "=== SideClique Integration ==="

# Copy module source files
echo "Copying module files..."
cp "$MODULE_SRC/SideClique.h" "$FIRMWARE_MODULES/"
cp "$MODULE_SRC/SideClique.cpp" "$FIRMWARE_MODULES/"
cp "$MODULE_SRC/BBSExtFlash.h" "$FIRMWARE_MODULES/"
cp "$MODULE_SRC/BBSExtFlash.cpp" "$FIRMWARE_MODULES/"
cp "$MODULE_SRC/BBSPlatform.h" "$FIRMWARE_MODULES/"
cp "$MODULE_SRC/BBSGeoLookup.h" "$FIRMWARE_MODULES/"
cp "$MODULE_SRC/BBSGeoDB.h" "$FIRMWARE_MODULES/"
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
echo "=== Integration Complete ==="
echo "Next: cd firmware && pio run -e t-echo"
