#!/bin/bash
# Installation script for SLURM TUI Monitor

set -e

echo "🚀 Installing SLURM TUI Monitor..."

# Check if we're in the right directory
if [ ! -f "pyproject.toml" ]; then
    echo "❌ Error: pyproject.toml not found!"
    echo "Please run this script from the slurmtui/ directory"
    exit 1
fi

# Check if virtual environment is activated
if [ -z "$VIRTUAL_ENV" ]; then
    echo "⚠️  Warning: No virtual environment detected"
    echo "Consider activating a virtual environment first:"
    echo "  python3 -m venv venv"
    echo "  source venv/bin/activate  # On macOS/Linux"
    echo "  venv\\Scripts\\activate     # On Windows"
    echo ""
    read -p "Continue anyway? (y/N) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        exit 1
    fi
fi

# Install the package in editable mode
echo "📦 Installing package and dependencies..."
pip install -e .

# Verify installation
echo ""
echo "✅ Installation complete!"
echo ""
echo "Verifying installation..."
# Run import check from parent directory to avoid path conflicts
if (cd .. && python -c "import slurmtui" 2>/dev/null); then
    echo "✅ Module 'slurmtui' successfully imported"
else
    echo "❌ Module import failed"
    echo "Note: This might be a path conflict. Try running from parent directory:"
    echo "  cd .. && python -c 'import slurmtui'"
fi

# Check if slurmtui command is available
if command -v slurmtui &> /dev/null; then
    echo "✅ Command 'slurmtui' is available"
    echo ""
    echo "🎉 Installation successful!"
    echo ""
    echo "Next steps:"
    echo "  1. Copy the example config: cp config.yaml.example config.yaml"
    echo "  2. Edit config.yaml if needed"
    echo "  3. Run the monitor: slurmtui"
else
    echo "⚠️  Command 'slurmtui' not found in PATH"
    echo "You may need to add the installation directory to your PATH"
    echo "or run directly: python -m slurmtui"
fi
