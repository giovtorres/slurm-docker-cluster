# Installation & Quick Start Guide

## Installation Steps

### 1. Navigate to the slurmtui directory

```bash
cd slurm-docker-cluster/slurmtui
```

### 2. Activate your virtual environment (if using one)

```bash
# If you don't have a venv yet, create one:
python3 -m venv venv

# Activate it:
source venv/bin/activate  # On macOS/Linux
# or
venv\Scripts\activate     # On Windows
```

### 3. Install the package

```bash
# Option 1: Use the install script
./install.sh

# Option 2: Manual installation
pip install -e .

# Option 3: Install just the dependencies
pip install -r requirements.txt
```

### 4. Verify installation

```bash
# Check if module is importable
python -c "import slurmtui; print('✅ slurmtui installed successfully')"

# Check if command is available
which slurmtui  # Should show the path to the executable
```

## Troubleshooting Installation Issues

### Error: "ModuleNotFoundError: No module named 'slurmtui'"

**Solution 1: Reinstall in editable mode**
```bash
cd slurm-docker-cluster/slurmtui
pip uninstall slurmtui -y
pip install -e .
```

**Solution 2: Run directly as a module**
```bash
cd slurm-docker-cluster
python -m slurmtui
```

**Solution 3: Check Python path**
```bash
# Ensure you're using the right Python
which python
python --version

# Check if slurmtui is in the path
python -c "import sys; print('\n'.join(sys.path))"
```

### Error: "command not found: slurmtui"

This means the script wasn't added to your PATH during installation.

**Solution 1: Reinstall with pip**
```bash
cd slurm-docker-cluster/slurmtui
pip install --force-reinstall -e .
```

**Solution 2: Run as a Python module**
```bash
python -m slurmtui
```

**Solution 3: Add to PATH manually**
```bash
# Find where pip installed the script
pip show -f slurmtui | grep -E "Location|slurmtui$"

# Add the bin directory to your PATH in ~/.bashrc or ~/.zshrc
export PATH="$PATH:/path/to/venv/bin"
```

### Error: "No module named 'textual'" or missing dependencies

**Solution:**
```bash
cd slurm-docker-cluster/slurmtui
pip install -r requirements.txt
```

### Virtual Environment Issues

If you created the venv in the wrong location:

```bash
# Remove old venv
rm -rf venv/

# Create new venv in the slurmtui directory
cd slurm-docker-cluster/slurmtui
python3 -m venv venv

# Activate it
source venv/bin/activate

# Install
pip install -e .
```

## Quick Start After Installation

```bash
# 1. Copy example config
cp config.yaml.example config.yaml

# 2. Run the monitor
slurmtui
# or
python -m slurmtui

# 3. With custom config
slurmtui --config /path/to/config.yaml

# 4. With environment variables
SLURM_API_URL=http://localhost:6820 slurmtui

# 5. With debug mode
slurmtui --debug
```

## Running Without Installation

If you just want to test without installing:

```bash
cd slurm-docker-cluster

# Install dependencies globally or in venv
pip install textual httpx pydantic pyyaml python-dateutil rich

# Run directly
python -m slurmtui
```

## Docker Installation

For Docker environments, dependencies are installed on-the-fly:

```bash
# Copy to container
docker cp slurmtui/ slurmctld:/opt/slurmtui/

# Enter container
docker exec -it slurmctld bash

# Install and run
cd /opt/slurmtui
pip3 install -e .
slurmtui
```

## Verifying Everything Works

Run these checks to ensure proper installation:

```bash
# 1. Check Python can import the module
python -c "import slurmtui; print(slurmtui.__version__)"

# 2. Check the command is available
which slurmtui

# 3. Check dependencies
python -c "import textual, httpx, pydantic, yaml; print('All dependencies OK')"

# 4. Run help
slurmtui --help

# 5. Check version
slurmtui --version
```

## Next Steps

Once installed, see:
- **README.md** - Full usage guide
- **DOCKER.md** - Docker integration
- **config.yaml.example** - Configuration options

Happy monitoring! 🎉
