"""Main CLI entry point for Slurm Playground."""

import subprocess
import sys
from pathlib import Path

import click
from rich.console import Console
from rich.table import Table

from . import __version__
from .experiment import experiment
from .metrics import metrics
from .scale import scale
from .workload import workload

console = Console()

# Find project root
SCRIPT_DIR = Path(__file__).parent
CLI_DIR = SCRIPT_DIR.parent
PLAYGROUND_DIR = CLI_DIR.parent
PROJECT_DIR = PLAYGROUND_DIR.parent


def get_docker_compose_cmd():
    """Get the docker compose command (docker compose or docker-compose)."""
    try:
        subprocess.run(
            ["docker", "compose", "version"],
            capture_output=True,
            check=True,
        )
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


@click.group()
@click.version_option(version=__version__, prog_name="playground")
@click.pass_context
def cli(ctx):
    """Slurm Playground - Generate workloads and explore cluster behavior.

    A comprehensive tool for learning Slurm, capacity planning,
    and configuration testing.

    \b
    Quick Start:
      playground status          Show cluster status
      playground workload cpu    Submit CPU-bound jobs
      playground scale set 10    Scale to 10 nodes
      playground metrics live    Real-time monitoring
    """
    ctx.ensure_object(dict)
    ctx.obj["project_dir"] = PROJECT_DIR
    ctx.obj["playground_dir"] = PLAYGROUND_DIR


@cli.command()
@click.pass_context
def status(ctx):
    """Show comprehensive cluster status."""
    if not is_cluster_running():
        console.print("[yellow]Cluster is not running.[/yellow]")
        console.print("Start with: [cyan]make playground-start[/cyan]")
        return

    console.print("\n[bold blue]=== Slurm Playground Status ===[/bold blue]\n")

    # Cluster info
    console.print("[bold]Cluster Information:[/bold]")
    result = run_in_slurmctld("scontrol show config | grep -E '^(ClusterName|SlurmctldHost)'", check=False)
    if result.returncode == 0:
        for line in result.stdout.strip().split("\n"):
            console.print(f"  {line}")
    console.print()

    # Node summary table
    console.print("[bold]Node Summary:[/bold]")
    result = run_in_slurmctld("sinfo -h -o '%P %a %D %t'", check=False)
    if result.returncode == 0 and result.stdout.strip():
        table = Table(show_header=True, header_style="bold cyan")
        table.add_column("Partition")
        table.add_column("Available")
        table.add_column("Nodes")
        table.add_column("State")

        for line in result.stdout.strip().split("\n"):
            parts = line.split()
            if len(parts) >= 4:
                table.add_row(*parts[:4])

        console.print(table)
    else:
        console.print("  [dim]No node information available[/dim]")
    console.print()

    # Queue summary
    console.print("[bold]Queue Summary:[/bold]")
    result = run_in_slurmctld("squeue -h -o '%i %j %u %t %M %D %R' | head -20", check=False)
    if result.returncode == 0 and result.stdout.strip():
        table = Table(show_header=True, header_style="bold cyan")
        table.add_column("JobID")
        table.add_column("Name")
        table.add_column("User")
        table.add_column("State")
        table.add_column("Time")
        table.add_column("Nodes")
        table.add_column("Reason")

        for line in result.stdout.strip().split("\n"):
            parts = line.split(None, 6)
            if len(parts) >= 6:
                table.add_row(*parts)

        console.print(table)

        # Count jobs
        result = run_in_slurmctld("squeue -h | wc -l", check=False)
        if result.returncode == 0:
            total = result.stdout.strip()
            console.print(f"  Total jobs in queue: {total}")
    else:
        console.print("  [dim]No jobs in queue[/dim]")
    console.print()

    # Resource utilization
    console.print("[bold]Resource Utilization:[/bold]")
    result = run_in_slurmctld(
        "sinfo -h -o '%C' | head -1", check=False
    )
    if result.returncode == 0 and result.stdout.strip():
        cpus = result.stdout.strip()
        console.print(f"  CPUs (allocated/idle/other/total): {cpus}")
    console.print()


@cli.command()
@click.pass_context
def info(ctx):
    """Show detailed node information."""
    if not is_cluster_running():
        console.print("[yellow]Cluster is not running.[/yellow]")
        return

    console.print("\n[bold blue]=== Node Details ===[/bold blue]\n")

    result = run_in_slurmctld("sinfo -N -l", check=False)
    if result.returncode == 0:
        console.print(result.stdout)
    else:
        console.print("[red]Failed to get node information[/red]")


@cli.command()
@click.argument("job_id", required=False)
@click.option("-u", "--user", default=None, help="Filter by user")
@click.option("-a", "--all", "show_all", is_flag=True, help="Show all jobs")
@click.pass_context
def jobs(ctx, job_id, user, show_all):
    """Show job queue or specific job details."""
    if not is_cluster_running():
        console.print("[yellow]Cluster is not running.[/yellow]")
        return

    if job_id:
        # Show specific job
        result = run_in_slurmctld(f"scontrol show job {job_id}", check=False)
        if result.returncode == 0:
            console.print(result.stdout)
        else:
            console.print(f"[red]Job {job_id} not found[/red]")
    else:
        # Show queue
        cmd = "squeue"
        if user:
            cmd += f" -u {user}"
        if show_all:
            cmd += " -a"

        result = run_in_slurmctld(cmd, check=False)
        if result.returncode == 0:
            console.print(result.stdout)
        else:
            console.print("[red]Failed to get job queue[/red]")


@cli.command()
@click.option("-j", "--job", "job_id", default=None, help="Show history for specific job")
@click.option("-n", "--lines", default=20, help="Number of lines to show")
@click.pass_context
def history(ctx, job_id, lines):
    """Show job history from accounting."""
    if not is_cluster_running():
        console.print("[yellow]Cluster is not running.[/yellow]")
        return

    if job_id:
        result = run_in_slurmctld(f"sacct -j {job_id} --format=JobID,JobName,Partition,Account,AllocCPUS,State,ExitCode,Elapsed", check=False)
    else:
        result = run_in_slurmctld(
            f"sacct --format=JobID,JobName,Partition,Account,AllocCPUS,State,ExitCode,Elapsed | head -{lines}",
            check=False,
        )

    if result.returncode == 0:
        console.print(result.stdout)
    else:
        console.print("[red]Failed to get job history[/red]")


@cli.command()
@click.argument("job_ids", nargs=-1, required=True)
@click.pass_context
def cancel(ctx, job_ids):
    """Cancel one or more jobs."""
    if not is_cluster_running():
        console.print("[yellow]Cluster is not running.[/yellow]")
        return

    for job_id in job_ids:
        result = run_in_slurmctld(f"scancel {job_id}", check=False)
        if result.returncode == 0:
            console.print(f"[green]Cancelled job {job_id}[/green]")
        else:
            console.print(f"[red]Failed to cancel job {job_id}[/red]")


@cli.command(name="cancel-all")
@click.option("-u", "--user", default="root", help="User whose jobs to cancel")
@click.confirmation_option(prompt="Are you sure you want to cancel all jobs?")
@click.pass_context
def cancel_all(ctx, user):
    """Cancel all jobs (optionally for a specific user)."""
    if not is_cluster_running():
        console.print("[yellow]Cluster is not running.[/yellow]")
        return

    result = run_in_slurmctld(f"scancel -u {user}", check=False)
    if result.returncode == 0:
        console.print(f"[green]Cancelled all jobs for user {user}[/green]")
    else:
        console.print("[red]Failed to cancel jobs[/red]")


@cli.command()
@click.pass_context
def partitions(ctx):
    """Show partition information."""
    if not is_cluster_running():
        console.print("[yellow]Cluster is not running.[/yellow]")
        return

    console.print("\n[bold blue]=== Partitions ===[/bold blue]\n")

    result = run_in_slurmctld("scontrol show partition", check=False)
    if result.returncode == 0:
        console.print(result.stdout)
    else:
        console.print("[red]Failed to get partition information[/red]")


# Register subcommand groups
cli.add_command(workload)
cli.add_command(scale)
cli.add_command(metrics)
cli.add_command(experiment)


def main():
    """Main entry point."""
    cli(obj={})


if __name__ == "__main__":
    main()
