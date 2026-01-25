"""Scaling commands for Slurm Playground."""

import subprocess
from datetime import datetime
from pathlib import Path

import click
import yaml
from jinja2 import Template
from rich.console import Console
from rich.table import Table

console = Console()

# Find directories
SCRIPT_DIR = Path(__file__).parent
CLI_DIR = SCRIPT_DIR.parent
PLAYGROUND_DIR = CLI_DIR.parent
PROJECT_DIR = PLAYGROUND_DIR.parent
CONFIGS_DIR = PLAYGROUND_DIR / "configs"


def get_docker_compose_cmd():
    """Get the docker compose command."""
    try:
        subprocess.run(["docker", "compose", "version"], capture_output=True, check=True)
        return ["docker", "compose"]
    except (subprocess.CalledProcessError, FileNotFoundError):
        return ["docker-compose"]


def run_in_slurmctld(cmd: str, check: bool = True) -> subprocess.CompletedProcess:
    """Run a command inside the slurmctld container."""
    compose_cmd = get_docker_compose_cmd()
    full_cmd = compose_cmd + [
        "-f",
        str(PROJECT_DIR / "docker-compose.yml"),
        "exec",
        "-T",
        "slurmctld",
        "bash",
        "-c",
        cmd,
    ]
    return subprocess.run(full_cmd, capture_output=True, text=True, check=check)


def is_cluster_running() -> bool:
    """Check if the Slurm cluster is running."""
    compose_cmd = get_docker_compose_cmd()
    result = subprocess.run(
        compose_cmd
        + [
            "-f",
            str(PROJECT_DIR / "docker-compose.yml"),
            "ps",
            "slurmctld",
            "--status",
            "running",
        ],
        capture_output=True,
        text=True,
    )
    return result.returncode == 0


def load_node_profiles() -> dict:
    """Load node profile definitions."""
    profiles_path = CONFIGS_DIR / "node_profiles.yml"
    if profiles_path.exists():
        with open(profiles_path) as f:
            return yaml.safe_load(f)
    return {"profiles": {}, "presets": {}}


def generate_slurm_conf(
    standard: int = 2,
    highmem: int = 0,
    highcpu: int = 0,
    gpu: int = 0,
    config_dir: Path | None = None,
) -> str:
    """Generate slurm.conf content for the specified node counts."""
    if config_dir is None:
        config_dir = PROJECT_DIR / "config" / "25.05"

    profiles = load_node_profiles()
    profile_specs = profiles.get("profiles", {})

    # Build node list
    nodes = []

    # Standard nodes
    if standard > 0:
        spec = profile_specs.get("standard", {"cpus": 2, "memory": 1000})
        for i in range(1, standard + 1):
            nodes.append({
                "name": f"c{i}",
                "cpus": spec.get("cpus", 2),
                "memory": spec.get("memory", 1000),
                "features": spec.get("features", []),
                "gres": spec.get("gres"),
            })

    # High-memory nodes
    if highmem > 0:
        spec = profile_specs.get("highmem", {"cpus": 2, "memory": 4000})
        for i in range(1, highmem + 1):
            nodes.append({
                "name": f"hm{i}",
                "cpus": spec.get("cpus", 2),
                "memory": spec.get("memory", 4000),
                "features": spec.get("features", ["highmem"]),
                "gres": spec.get("gres"),
            })

    # High-CPU nodes
    if highcpu > 0:
        spec = profile_specs.get("highcpu", {"cpus": 4, "memory": 2000})
        for i in range(1, highcpu + 1):
            nodes.append({
                "name": f"hc{i}",
                "cpus": spec.get("cpus", 4),
                "memory": spec.get("memory", 2000),
                "features": spec.get("features", ["highcpu"]),
                "gres": spec.get("gres"),
            })

    # GPU nodes
    if gpu > 0:
        spec = profile_specs.get("gpu", {"cpus": 2, "memory": 2000, "gres": "gpu:1"})
        for i in range(1, gpu + 1):
            nodes.append({
                "name": f"gpu{i}",
                "cpus": spec.get("cpus", 2),
                "memory": spec.get("memory", 2000),
                "features": spec.get("features", ["gpu"]),
                "gres": spec.get("gres", "gpu:1"),
            })

    # Build partitions
    partitions = []

    # Normal partition (standard nodes)
    if standard > 0:
        normal_nodes = [f"c{i}" for i in range(1, standard + 1)]
        partitions.append({
            "name": "normal",
            "nodes": normal_nodes,
            "default": True,
            "max_time": "INFINITE",
        })

    # Highmem partition
    if highmem > 0:
        hm_nodes = [f"hm{i}" for i in range(1, highmem + 1)]
        partitions.append({
            "name": "highmem",
            "nodes": hm_nodes,
            "default": False,
            "max_time": "24:00:00",
        })

    # Highcpu partition
    if highcpu > 0:
        hc_nodes = [f"hc{i}" for i in range(1, highcpu + 1)]
        partitions.append({
            "name": "highcpu",
            "nodes": hc_nodes,
            "default": False,
            "max_time": "12:00:00",
        })

    # GPU partition
    if gpu > 0:
        gpu_nodes = [f"gpu{i}" for i in range(1, gpu + 1)]
        partitions.append({
            "name": "gpu",
            "nodes": gpu_nodes,
            "default": False,
            "max_time": "8:00:00",
        })

    # GRES types
    gres_types = []
    if gpu > 0:
        gres_types.append("gpu")

    # Load template
    template_path = CONFIGS_DIR / "slurm.conf.j2"
    if template_path.exists():
        with open(template_path) as f:
            template = Template(f.read())

        return template.render(
            timestamp=datetime.utcnow().isoformat() + "Z",
            cluster_name="linux",
            nodes=nodes,
            partitions=partitions,
            gres_types=gres_types if gres_types else None,
        )
    else:
        # Fallback: generate simple config
        return generate_simple_slurm_conf(nodes, partitions, gres_types)


def generate_simple_slurm_conf(nodes: list, partitions: list, gres_types: list) -> str:
    """Generate a simple slurm.conf without Jinja2 template."""
    timestamp = datetime.utcnow().isoformat() + "Z"

    lines = [
        "# slurm.conf - Slurm configuration file",
        f"# Generated by: playground scale command",
        f"# Timestamp: {timestamp}",
        "#",
        "# Cluster Identity",
        "ClusterName=linux",
        "SlurmctldHost=slurmctld",
        "",
        "# Authentication",
        "AuthType=auth/munge",
        "",
        "# Paths",
        "SlurmctldPidFile=/var/run/slurm/slurmctld.pid",
        "SlurmctldPort=6817",
        "SlurmdPidFile=/var/run/slurm/slurmd.pid",
        "SlurmdPort=6818",
        "SlurmdSpoolDir=/var/spool/slurm",
        "SlurmUser=slurm",
        "StateSaveLocation=/var/lib/slurm",
        "",
        "# Process Tracking",
        "ProctrackType=proctrack/linuxproc",
        "TaskPlugin=task/affinity",
        "",
        "# Timers",
        "InactiveLimit=0",
        "KillWait=30",
        "MinJobAge=300",
        "SlurmctldTimeout=120",
        "SlurmdTimeout=300",
        "Waittime=0",
        "",
        "# Scheduling",
        "SchedulerType=sched/backfill",
        "SelectType=select/cons_tres",
        "",
        "# Service Behavior",
        "ReturnToService=1",
        "",
        "# Accounting",
        "AccountingStorageHost=slurmdbd",
        "AccountingStorageType=accounting_storage/slurmdbd",
        "JobCompLoc=/var/log/slurm/jobcomp.log",
        "JobCompType=jobcomp/filetxt",
        "JobAcctGatherType=jobacct_gather/linux",
        "JobAcctGatherFrequency=30",
        "",
        "# Logging",
        "SlurmctldDebug=info",
        "SlurmctldLogFile=/var/log/slurm/slurmctld.log",
        "SlurmdDebug=info",
        "SlurmdLogFile=/var/log/slurm/slurmd.log",
        "",
    ]

    # GRES types
    if gres_types:
        lines.append(f"GresTypes={','.join(gres_types)}")
        lines.append("")

    # Nodes
    lines.append("# Compute Nodes")
    for node in nodes:
        line = f"NodeName={node['name']} CPUs={node['cpus']} RealMemory={node['memory']}"
        if node.get("features"):
            line += f" Features={','.join(node['features'])}"
        if node.get("gres"):
            line += f" Gres={node['gres']}"
        line += " State=UNKNOWN"
        lines.append(line)

    lines.append("")

    # Partitions
    lines.append("# Partitions")
    for partition in partitions:
        line = f"PartitionName={partition['name']} Nodes={','.join(partition['nodes'])}"
        if partition.get("default"):
            line += " Default=YES"
        line += f" MaxTime={partition.get('max_time', 'INFINITE')}"
        line += " State=UP"
        lines.append(line)

    return "\n".join(lines)


def write_slurm_conf(content: str) -> Path:
    """Write slurm.conf to the config directory."""
    config_path = PROJECT_DIR / "config" / "25.05" / "slurm.conf"
    config_path.parent.mkdir(parents=True, exist_ok=True)
    with open(config_path, "w") as f:
        f.write(content)
    return config_path


def apply_config():
    """Apply configuration changes to running cluster."""
    if is_cluster_running():
        result = run_in_slurmctld("scontrol reconfigure", check=False)
        return result.returncode == 0
    return False


@click.group()
def scale():
    """Scale the cluster and manage node topology.

    \b
    Examples:
      playground scale set 10        # Scale to 10 standard nodes
      playground scale add highmem 2 # Add 2 high-memory nodes
      playground scale status        # Show current topology
      playground scale preset medium # Apply medium preset
    """
    pass


@scale.command()
@click.argument("count", type=int)
@click.option("--apply/--no-apply", default=True, help="Apply changes to running cluster")
def set(count, apply):
    """Set cluster to N standard nodes (1-20)."""
    if count < 1 or count > 20:
        console.print("[red]Node count must be between 1 and 20[/red]")
        return

    console.print(f"[bold]Scaling to {count} standard nodes[/bold]")

    # Generate and write config
    content = generate_slurm_conf(standard=count)
    config_path = write_slurm_conf(content)
    console.print(f"  Generated: {config_path}")

    # Apply if requested
    if apply:
        if apply_config():
            console.print("[green]Configuration applied[/green]")
        else:
            console.print("[yellow]Could not apply configuration (cluster may not be running)[/yellow]")
            console.print("  Start cluster with: make playground-start")


@scale.command()
@click.argument("profile", type=click.Choice(["standard", "highmem", "highcpu", "gpu"]))
@click.argument("count", type=int)
@click.option("--apply/--no-apply", default=True, help="Apply changes to running cluster")
def add(profile, count, apply):
    """Add nodes of a specific profile type.

    Note: This currently requires regenerating the entire configuration.
    Use 'preset' for common configurations.
    """
    console.print(f"[bold]Adding {count} {profile} nodes[/bold]")

    # Get current node counts (simplified - just add to defaults)
    kwargs = {"standard": 2, "highmem": 0, "highcpu": 0, "gpu": 0}
    kwargs[profile] = count

    # Generate and write config
    content = generate_slurm_conf(**kwargs)
    config_path = write_slurm_conf(content)
    console.print(f"  Generated: {config_path}")

    if apply:
        if apply_config():
            console.print("[green]Configuration applied[/green]")
        else:
            console.print("[yellow]Could not apply configuration[/yellow]")


@scale.command()
def status():
    """Show current cluster topology."""
    console.print("\n[bold blue]=== Cluster Topology ===[/bold blue]\n")

    if not is_cluster_running():
        console.print("[yellow]Cluster is not running[/yellow]")

        # Show configuration from file
        config_path = PROJECT_DIR / "config" / "25.05" / "slurm.conf"
        if config_path.exists():
            console.print("\n[dim]Configuration from file:[/dim]")
            with open(config_path) as f:
                for line in f:
                    if line.startswith("NodeName=") or line.startswith("PartitionName="):
                        console.print(f"  {line.strip()}")
        return

    # Node table
    console.print("[bold]Nodes:[/bold]")
    result = run_in_slurmctld("sinfo -N -h -o '%N %c %m %f %T'", check=False)
    if result.returncode == 0 and result.stdout.strip():
        table = Table(show_header=True, header_style="bold cyan")
        table.add_column("Node")
        table.add_column("CPUs")
        table.add_column("Memory (MB)")
        table.add_column("Features")
        table.add_column("State")

        for line in result.stdout.strip().split("\n"):
            parts = line.split()
            if len(parts) >= 5:
                table.add_row(*parts[:5])
            elif len(parts) >= 4:
                table.add_row(*parts[:4], "")

        console.print(table)
    else:
        console.print("  [dim]No node information available[/dim]")

    console.print()

    # Partition summary
    console.print("[bold]Partitions:[/bold]")
    result = run_in_slurmctld("sinfo -h -o '%P %a %l %D %N'", check=False)
    if result.returncode == 0 and result.stdout.strip():
        table = Table(show_header=True, header_style="bold cyan")
        table.add_column("Partition")
        table.add_column("Available")
        table.add_column("Time Limit")
        table.add_column("Nodes")
        table.add_column("Node List")

        for line in result.stdout.strip().split("\n"):
            parts = line.split(None, 4)
            if len(parts) >= 4:
                table.add_row(*parts)

        console.print(table)

    console.print()

    # Resource summary
    console.print("[bold]Resource Summary:[/bold]")
    result = run_in_slurmctld("sinfo -h -o '%C'", check=False)
    if result.returncode == 0:
        cpus = result.stdout.strip().split("\n")[0]
        console.print(f"  CPUs (A/I/O/T): {cpus}")

    result = run_in_slurmctld("sinfo -h -o '%D' | head -1", check=False)
    if result.returncode == 0:
        nodes = result.stdout.strip().split("\n")[0]
        console.print(f"  Total nodes: {nodes}")


@scale.command()
@click.option("--apply/--no-apply", default=True, help="Apply changes to running cluster")
def reset(apply):
    """Reset to default 2-node cluster."""
    console.print("[bold]Resetting to default configuration[/bold]")

    content = generate_slurm_conf(standard=2)
    config_path = write_slurm_conf(content)
    console.print(f"  Generated: {config_path}")

    if apply:
        if apply_config():
            console.print("[green]Configuration applied[/green]")
        else:
            console.print("[yellow]Could not apply configuration[/yellow]")


@scale.command()
@click.argument("preset_name")
@click.option("--apply/--no-apply", default=True, help="Apply changes to running cluster")
def preset(preset_name, apply):
    """Apply a predefined scaling preset.

    \b
    Available presets:
      minimal  - 2 standard nodes
      small    - 5 standard nodes
      medium   - 10 standard + 2 highmem + 2 highcpu
      large    - 20 standard + 2 highmem + 2 highcpu + 2 gpu
      gpu      - 4 standard + 4 gpu nodes
    """
    presets = {
        "minimal": {"standard": 2, "highmem": 0, "highcpu": 0, "gpu": 0},
        "small": {"standard": 5, "highmem": 0, "highcpu": 0, "gpu": 0},
        "medium": {"standard": 10, "highmem": 2, "highcpu": 2, "gpu": 0},
        "large": {"standard": 20, "highmem": 2, "highcpu": 2, "gpu": 2},
        "gpu": {"standard": 4, "highmem": 0, "highcpu": 0, "gpu": 4},
    }

    if preset_name not in presets:
        console.print(f"[red]Unknown preset: {preset_name}[/red]")
        console.print("\nAvailable presets:")
        for name, spec in presets.items():
            console.print(f"  {name}: {spec}")
        return

    spec = presets[preset_name]
    console.print(f"[bold]Applying preset: {preset_name}[/bold]")
    console.print(f"  Nodes: {spec}")

    content = generate_slurm_conf(**spec)
    config_path = write_slurm_conf(content)
    console.print(f"  Generated: {config_path}")

    if apply:
        if apply_config():
            console.print("[green]Configuration applied[/green]")
        else:
            console.print("[yellow]Could not apply configuration[/yellow]")


@scale.command(name="list-presets")
def list_presets():
    """List available scaling presets."""
    console.print("\n[bold]Available Scaling Presets:[/bold]\n")

    presets = load_node_profiles().get("presets", {})

    table = Table(show_header=True, header_style="bold cyan")
    table.add_column("Name")
    table.add_column("Standard")
    table.add_column("Highmem")
    table.add_column("Highcpu")
    table.add_column("GPU")
    table.add_column("Description")

    default_presets = {
        "minimal": {"standard": 2, "highmem": 0, "highcpu": 0, "gpu": 0, "description": "Basic testing"},
        "small": {"standard": 5, "highmem": 0, "highcpu": 0, "gpu": 0, "description": "Small cluster"},
        "medium": {"standard": 10, "highmem": 2, "highcpu": 2, "gpu": 0, "description": "With specialized nodes"},
        "large": {"standard": 20, "highmem": 2, "highcpu": 2, "gpu": 2, "description": "Full configuration"},
        "gpu": {"standard": 4, "highmem": 0, "highcpu": 0, "gpu": 4, "description": "GPU focused"},
    }

    # Merge with config file presets
    all_presets = {**default_presets, **presets}

    for name, spec in sorted(all_presets.items()):
        table.add_row(
            name,
            str(spec.get("standard", 0)),
            str(spec.get("highmem", 0)),
            str(spec.get("highcpu", 0)),
            str(spec.get("gpu", 0)),
            spec.get("description", ""),
        )

    console.print(table)
