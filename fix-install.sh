#!/bin/bash
# Quick fix script for slurmtui installation issues

echo "🔧 Fixing slurmtui installation..."

# Detect where we are
if [ -f "pyproject.toml" ]; then
    # We're in slurmtui directory
    INSTALL_DIR="."
    cd ..
elif [ -d "slurmtui" ] && [ -f "slurmtui/pyproject.toml" ]; then
    # We're in parent directory
    INSTALL_DIR="slurmtui"
else
    echo "❌ Error: Cannot find slurmtui directory"
    exit 1
fi

echo "📍 Working directory: $(pwd)"
echo "📦 Installing from: $INSTALL_DIR"

# Uninstall old version
echo "🗑️  Removing old installation..."
pip uninstall slurmtui -y 2>/dev/null || true

# Reinstall from the correct path
echo "📦 Installing slurmtui..."
pip install -e "$INSTALL_DIR"

# Verify
echo ""
echo "✅ Verifying installation..."
if python -c "import slurmtui" 2>/dev/null; then
    echo "✅ Module import: OK"

    if command -v slurmtui &> /dev/null; then
        echo "✅ Command available: OK"
        echo ""
        echo "🎉 Installation successful!"
        echo ""
        echo "Run with: slurmtui"
    else
        echo "⚠️  Command not in PATH, but module works"
        echo ""
        echo "Run with: python -m slurmtui"
    fi
else
    echo "❌ Module import failed"
    echo ""
    echo "Try running: python -m slurmtui"
fi
