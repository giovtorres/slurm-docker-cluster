"""Metrics and monitoring commands for Slurm Playground."""

import json
import subprocess
import time
from datetime import datetime
from pathlib import Path

import click
from rich.console import Console
from rich.live import Live
from rich.panel import Panel
from rich.table import Table
from rich.text import Text

console = Console()

# Find directories
SCRIPT_DIR = Path(__file__).parent
CLI_DIR = SCRIPT_DIR.parent
PLAYGROUND_DIR = CLI_DIR.parent
PROJECT_DIR = PLAYGROUND_DIR.parent


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


def get_queue_stats() -> dict:
    """Get queue statistics."""
    stats = {"pending": 0, "running": 0, "completed": 0, "failed": 0, "total": 0}

    result = run_in_slurmctld(
        "squeue -h -o '%T' | sort | uniq -c", check=False
    )
    if result.returncode == 0 and result.stdout.strip():
        for line in result.stdout.strip().split("\n"):
            parts = line.strip().split()
            if len(parts) >= 2:
                count = int(parts[0])
                state = parts[1].upper()
                if state in ("PENDING", "PD"):
                    stats["pending"] = count
                elif state in ("RUNNING", "R"):
                    stats["running"] = count
                elif state in ("COMPLETING", "CG"):
                    stats["running"] += count
                stats["total"] += count

    return stats


def get_node_stats() -> dict:
    """Get node statistics."""
    stats = {"total": 0, "idle": 0, "allocated": 0, "down": 0, "mixed": 0}

    result = run_in_slurmctld(
        "sinfo -h -o '%T' | sort | uniq -c", check=False
    )
    if result.returncode == 0 and result.stdout.strip():
        for line in result.stdout.strip().split("\n"):
            parts = line.strip().split()
            if len(parts) >= 2:
                count = int(parts[0])
                state = parts[1].lower()
                stats["total"] += count
                if "idle" in state:
                    stats["idle"] += count
                elif "alloc" in state:
                    stats["allocated"] += count
                elif "mix" in state:
                    stats["mixed"] += count
                elif "down" in state or "drain" in state:
                    stats["down"] += count

    return stats


def get_cpu_stats() -> dict:
    """Get CPU allocation statistics."""
    stats = {"allocated": 0, "idle": 0, "other": 0, "total": 0}

    result = run_in_slurmctld("sinfo -h -o '%C' | head -1", check=False)
    if result.returncode == 0 and result.stdout.strip():
        # Format: ALLOCATED/IDLE/OTHER/TOTAL
        parts = result.stdout.strip().split("/")
        if len(parts) >= 4:
            stats["allocated"] = int(parts[0])
            stats["idle"] = int(parts[1])
            stats["other"] = int(parts[2])
            stats["total"] = int(parts[3])

    return stats


def get_partition_stats() -> list[dict]:
    """Get per-partition statistics."""
    partitions = []

    result = run_in_slurmctld(
        "sinfo -h -o '%P|%a|%D|%c|%m|%T'", check=False
    )
    if result.returncode == 0 and result.stdout.strip():
        for line in result.stdout.strip().split("\n"):
            parts = line.split("|")
            if len(parts) >= 6:
                partitions.append({
                    "name": parts[0].rstrip("*"),
                    "available": parts[1],
                    "nodes": int(parts[2]),
                    "cpus_per_node": parts[3],
                    "memory_per_node": parts[4],
                    "state": parts[5],
                })

    return partitions


def get_job_throughput() -> dict:
    """Get job throughput stats from accounting."""
    stats = {"completed_1h": 0, "completed_24h": 0, "avg_wait_time": 0, "avg_run_time": 0}

    # Jobs completed in last hour
    result = run_in_slurmctld(
        "sacct -S now-1hour -E now -X -n --state=COMPLETED | wc -l",
        check=False
    )
    if result.returncode == 0:
        try:
            stats["completed_1h"] = int(result.stdout.strip())
        except ValueError:
            pass

    # Jobs completed in last 24 hours
    result = run_in_slurmctld(
        "sacct -S now-1day -E now -X -n --state=COMPLETED | wc -l",
        check=False
    )
    if result.returncode == 0:
        try:
            stats["completed_24h"] = int(result.stdout.strip())
        except ValueError:
            pass

    return stats


def build_dashboard() -> Panel:
    """Build the live dashboard panel."""
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    # Get all stats
    queue = get_queue_stats()
    nodes = get_node_stats()
    cpus = get_cpu_stats()
    throughput = get_job_throughput()
    partitions = get_partition_stats()

    # Build output
    output = Text()

    # Header
    output.append(f"Last updated: {timestamp}\n\n", style="dim")

    # Queue summary
    output.append("Queue Summary\n", style="bold cyan")
    output.append(f"  Pending:  {queue['pending']:>5}\n")
    output.append(f"  Running:  {queue['running']:>5}\n")
    output.append(f"  Total:    {queue['total']:>5}\n\n")

    # Node summary
    output.append("Node Summary\n", style="bold cyan")
    output.append(f"  Total:     {nodes['total']:>4}\n")
    output.append(f"  Idle:      {nodes['idle']:>4}\n")
    output.append(f"  Allocated: {nodes['allocated']:>4}\n")
    output.append(f"  Mixed:     {nodes['mixed']:>4}\n")
    if nodes['down'] > 0:
        output.append(f"  Down:      {nodes['down']:>4}\n", style="red")
    output.append("\n")

    # CPU utilization
    output.append("CPU Utilization\n", style="bold cyan")
    if cpus['total'] > 0:
        util_pct = (cpus['allocated'] / cpus['total']) * 100
        bar_len = 20
        filled = int(bar_len * cpus['allocated'] / cpus['total'])
        bar = "█" * filled + "░" * (bar_len - filled)
        output.append(f"  [{bar}] {util_pct:.1f}%\n")
        output.append(f"  {cpus['allocated']}/{cpus['total']} CPUs allocated\n\n")
    else:
        output.append("  No CPU data available\n\n")

    # Throughput
    output.append("Job Throughput\n", style="bold cyan")
    output.append(f"  Last hour: {throughput['completed_1h']} completed\n")
    output.append(f"  Last 24h:  {throughput['completed_24h']} completed\n\n")

    # Partitions table
    if partitions:
        output.append("Partitions\n", style="bold cyan")
        for p in partitions:
            state_style = "green" if p["state"] == "idle" else "yellow" if "mix" in p["state"] else ""
            output.append(f"  {p['name']:<12} {p['nodes']:>3} nodes  {p['state']}\n", style=state_style)

    return Panel(output, title="[bold]Slurm Playground Dashboard[/bold]", border_style="blue")


@click.group()
def metrics():
    """Monitor cluster metrics and performance.

    \b
    Examples:
      playground metrics live      # Real-time dashboard
      playground metrics report    # Generate summary report
      playground metrics export    # Export metrics to JSON
    """
    pass


@metrics.command()
@click.option("-r", "--refresh", default=2, help="Refresh interval in seconds")
def live(refresh):
    """Display real-time cluster metrics dashboard."""
    if not is_cluster_running():
        console.print("[yellow]Cluster is not running.[/yellow]")
        return

    console.print("[dim]Press Ctrl+C to exit[/dim]\n")

    try:
        with Live(build_dashboard(), console=console, refresh_per_second=1) as live:
            while True:
                time.sleep(refresh)
                live.update(build_dashboard())
    except KeyboardInterrupt:
        console.print("\n[dim]Dashboard stopped[/dim]")


@metrics.command()
def report():
    """Generate a summary metrics report."""
    if not is_cluster_running():
        console.print("[yellow]Cluster is not running.[/yellow]")
        return

    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    console.print(f"\n[bold blue]=== Slurm Metrics Report ===[/bold blue]")
    console.print(f"[dim]Generated: {timestamp}[/dim]\n")

    # Queue statistics
    queue = get_queue_stats()
    console.print("[bold]Queue Statistics:[/bold]")
    console.print(f"  Jobs pending:    {queue['pending']}")
    console.print(f"  Jobs running:    {queue['running']}")
    console.print(f"  Total in queue:  {queue['total']}")
    console.print()

    # Node statistics
    nodes = get_node_stats()
    console.print("[bold]Node Statistics:[/bold]")
    console.print(f"  Total nodes:     {nodes['total']}")
    console.print(f"  Idle:            {nodes['idle']}")
    console.print(f"  Allocated:       {nodes['allocated']}")
    console.print(f"  Mixed:           {nodes['mixed']}")
    console.print(f"  Down/Drain:      {nodes['down']}")
    console.print()

    # CPU statistics
    cpus = get_cpu_stats()
    console.print("[bold]CPU Statistics:[/bold]")
    console.print(f"  Total CPUs:      {cpus['total']}")
    console.print(f"  Allocated:       {cpus['allocated']}")
    console.print(f"  Idle:            {cpus['idle']}")
    if cpus['total'] > 0:
        util = (cpus['allocated'] / cpus['total']) * 100
        console.print(f"  Utilization:     {util:.1f}%")
    console.print()

    # Throughput
    throughput = get_job_throughput()
    console.print("[bold]Job Throughput:[/bold]")
    console.print(f"  Completed (1h):  {throughput['completed_1h']}")
    console.print(f"  Completed (24h): {throughput['completed_24h']}")
    console.print()

    # Partition breakdown
    partitions = get_partition_stats()
    if partitions:
        console.print("[bold]Partition Details:[/bold]")
        table = Table(show_header=True, header_style="bold cyan")
        table.add_column("Partition")
        table.add_column("Nodes", justify="right")
        table.add_column("CPUs/Node")
        table.add_column("Mem/Node")
        table.add_column("State")

        for p in partitions:
            table.add_row(
                p["name"],
                str(p["nodes"]),
                p["cpus_per_node"],
                p["memory_per_node"],
                p["state"],
            )

        console.print(table)
    console.print()

    # Recent job summary
    console.print("[bold]Recent Job Summary:[/bold]")
    result = run_in_slurmctld(
        "sacct -S now-1hour -X --format=State -n | sort | uniq -c | sort -rn | head -5",
        check=False,
    )
    if result.returncode == 0 and result.stdout.strip():
        for line in result.stdout.strip().split("\n"):
            console.print(f"  {line.strip()}")
    else:
        console.print("  [dim]No recent job data[/dim]")


@metrics.command()
@click.option("-o", "--output", default="metrics.json", help="Output file path")
@click.option("--pretty/--no-pretty", default=True, help="Pretty print JSON")
def export(output, pretty):
    """Export current metrics to JSON file."""
    if not is_cluster_running():
        console.print("[yellow]Cluster is not running.[/yellow]")
        return

    timestamp = datetime.now().isoformat()

    data = {
        "timestamp": timestamp,
        "queue": get_queue_stats(),
        "nodes": get_node_stats(),
        "cpus": get_cpu_stats(),
        "throughput": get_job_throughput(),
        "partitions": get_partition_stats(),
    }

    # Get additional raw data
    result = run_in_slurmctld("squeue -h -o '%i|%j|%u|%t|%M|%D|%R'", check=False)
    if result.returncode == 0 and result.stdout.strip():
        data["jobs"] = []
        for line in result.stdout.strip().split("\n"):
            parts = line.split("|")
            if len(parts) >= 7:
                data["jobs"].append({
                    "job_id": parts[0],
                    "name": parts[1],
                    "user": parts[2],
                    "state": parts[3],
                    "time": parts[4],
                    "nodes": parts[5],
                    "reason": parts[6],
                })

    # Write output
    output_path = Path(output)
    with open(output_path, "w") as f:
        if pretty:
            json.dump(data, f, indent=2)
        else:
            json.dump(data, f)

    console.print(f"[green]Metrics exported to:[/green] {output_path}")


@metrics.command()
@click.option("-n", "--lines", default=50, help="Number of lines to show")
@click.option("--follow", "-f", is_flag=True, help="Follow log output")
@click.option(
    "--service",
    type=click.Choice(["slurmctld", "slurmdbd", "slurmd"]),
    default="slurmctld",
    help="Service to show logs for",
)
def logs(lines, follow, service):
    """View Slurm daemon logs."""
    compose_cmd = get_docker_compose_cmd()

    cmd = compose_cmd + [
        "-f",
        str(PROJECT_DIR / "docker-compose.yml"),
        "logs",
        f"--tail={lines}",
    ]

    if follow:
        cmd.append("-f")

    cmd.append(service)

    try:
        subprocess.run(cmd)
    except KeyboardInterrupt:
        pass


@metrics.command()
@click.option("-j", "--job", "job_id", required=True, help="Job ID to analyze")
def job_stats(job_id):
    """Show detailed statistics for a specific job."""
    if not is_cluster_running():
        console.print("[yellow]Cluster is not running.[/yellow]")
        return

    console.print(f"\n[bold blue]=== Job {job_id} Statistics ===[/bold blue]\n")

    # Job details
    result = run_in_slurmctld(
        f"sacct -j {job_id} --format=JobID,JobName,Partition,Account,AllocCPUS,State,ExitCode,"
        f"Elapsed,TotalCPU,MaxRSS,MaxVMSize,AveCPU,AveRSS -P",
        check=False,
    )

    if result.returncode == 0 and result.stdout.strip():
        lines = result.stdout.strip().split("\n")
        if len(lines) > 1:
            headers = lines[0].split("|")
            for line in lines[1:]:
                values = line.split("|")
                console.print(f"[bold]{values[0]}[/bold]")
                for h, v in zip(headers[1:], values[1:]):
                    if v:
                        console.print(f"  {h}: {v}")
                console.print()
    else:
        console.print(f"[red]Job {job_id} not found[/red]")


@metrics.command()
@click.option("-n", "--top", default=10, help="Number of users to show")
def user_stats(top):
    """Show per-user job statistics."""
    if not is_cluster_running():
        console.print("[yellow]Cluster is not running.[/yellow]")
        return

    console.print("\n[bold blue]=== User Statistics ===[/bold blue]\n")

    # Current queue by user
    console.print("[bold]Current Queue by User:[/bold]")
    result = run_in_slurmctld(
        f"squeue -h -o '%u' | sort | uniq -c | sort -rn | head -{top}",
        check=False,
    )
    if result.returncode == 0 and result.stdout.strip():
        table = Table(show_header=True, header_style="bold cyan")
        table.add_column("Jobs", justify="right")
        table.add_column("User")

        for line in result.stdout.strip().split("\n"):
            parts = line.strip().split()
            if len(parts) >= 2:
                table.add_row(parts[0], parts[1])

        console.print(table)
    else:
        console.print("  [dim]No queue data[/dim]")

    console.print()

    # Recent job history by user
    console.print("[bold]Jobs Completed (24h) by User:[/bold]")
    result = run_in_slurmctld(
        f"sacct -S now-1day -E now -X -n --format=User --state=COMPLETED | "
        f"sort | uniq -c | sort -rn | head -{top}",
        check=False,
    )
    if result.returncode == 0 and result.stdout.strip():
        table = Table(show_header=True, header_style="bold cyan")
        table.add_column("Completed", justify="right")
        table.add_column("User")

        for line in result.stdout.strip().split("\n"):
            parts = line.strip().split()
            if len(parts) >= 2:
                table.add_row(parts[0], parts[1])

        console.print(table)
    else:
        console.print("  [dim]No completion data[/dim]")
