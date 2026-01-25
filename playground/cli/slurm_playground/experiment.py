"""Experiment framework commands for Slurm Playground."""

import json
import subprocess
import time
from datetime import datetime
from pathlib import Path

import click
import yaml
from rich.console import Console
from rich.progress import Progress, SpinnerColumn, TextColumn, BarColumn, TaskProgressColumn
from rich.table import Table

console = Console()

# Find directories
SCRIPT_DIR = Path(__file__).parent
CLI_DIR = SCRIPT_DIR.parent
PLAYGROUND_DIR = CLI_DIR.parent
PROJECT_DIR = PLAYGROUND_DIR.parent
EXPERIMENTS_DIR = PLAYGROUND_DIR / "experiments"


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


def load_experiment(name: str) -> dict | None:
    """Load an experiment configuration."""
    exp_dir = EXPERIMENTS_DIR / name

    # Try different config file names
    for config_name in ["experiment.yml", "experiment.yaml", "experiment.json", "config.yml", "config.yaml"]:
        config_path = exp_dir / config_name
        if config_path.exists():
            with open(config_path) as f:
                if config_name.endswith(".json"):
                    return json.load(f)
                else:
                    return yaml.safe_load(f)

    return None


def save_experiment_results(name: str, results: dict) -> Path:
    """Save experiment results."""
    results_dir = EXPERIMENTS_DIR / name / "results"
    results_dir.mkdir(parents=True, exist_ok=True)

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    results_path = results_dir / f"run_{timestamp}.json"

    with open(results_path, "w") as f:
        json.dump(results, f, indent=2)

    return results_path


def collect_metrics() -> dict:
    """Collect current cluster metrics."""
    metrics = {
        "timestamp": datetime.now().isoformat(),
        "queue": {},
        "nodes": {},
        "cpus": {},
    }

    # Queue stats
    result = run_in_slurmctld("squeue -h | wc -l", check=False)
    if result.returncode == 0:
        metrics["queue"]["total"] = int(result.stdout.strip())

    result = run_in_slurmctld("squeue -h -t PENDING | wc -l", check=False)
    if result.returncode == 0:
        metrics["queue"]["pending"] = int(result.stdout.strip())

    result = run_in_slurmctld("squeue -h -t RUNNING | wc -l", check=False)
    if result.returncode == 0:
        metrics["queue"]["running"] = int(result.stdout.strip())

    # CPU stats
    result = run_in_slurmctld("sinfo -h -o '%C' | head -1", check=False)
    if result.returncode == 0 and result.stdout.strip():
        parts = result.stdout.strip().split("/")
        if len(parts) >= 4:
            metrics["cpus"]["allocated"] = int(parts[0])
            metrics["cpus"]["idle"] = int(parts[1])
            metrics["cpus"]["total"] = int(parts[3])

    return metrics


def wait_for_jobs_complete(timeout: int = 300, check_interval: int = 5) -> bool:
    """Wait for all jobs to complete."""
    start_time = time.time()

    while time.time() - start_time < timeout:
        result = run_in_slurmctld("squeue -h | wc -l", check=False)
        if result.returncode == 0:
            count = int(result.stdout.strip())
            if count == 0:
                return True
        time.sleep(check_interval)

    return False


def submit_job_batch(jobs: list[dict]) -> list[str]:
    """Submit a batch of jobs and return job IDs."""
    job_ids = []

    for job in jobs:
        job_type = job.get("type", "sleep")
        count = job.get("count", 1)
        duration = job.get("duration", 30)
        cpus = job.get("cpus", 1)
        memory = job.get("memory", "500M")

        for i in range(count):
            if job_type == "sleep":
                script = f"""#!/bin/bash
#SBATCH --job-name=exp_sleep
#SBATCH --time={duration // 60 + 1}
#SBATCH --output=/data/exp_%j.out
sleep {duration}
"""
            elif job_type == "cpu":
                script = f"""#!/bin/bash
#SBATCH --job-name=exp_cpu
#SBATCH --cpus-per-task={cpus}
#SBATCH --time={duration // 60 + 1}
#SBATCH --output=/data/exp_%j.out
end=$((SECONDS + {duration}))
while [ $SECONDS -lt $end ]; do : ; done
"""
            elif job_type == "memory":
                mem_mb = int(memory.rstrip("MG")) * (1024 if memory.endswith("G") else 1)
                script = f"""#!/bin/bash
#SBATCH --job-name=exp_memory
#SBATCH --mem={memory}
#SBATCH --time={duration // 60 + 1}
#SBATCH --output=/data/exp_%j.out
python3 -c "import time; d=['x'*1024*1024 for _ in range({int(mem_mb * 0.8)})]; time.sleep({duration})"
"""
            else:
                continue

            # Submit job
            cmd = f"""
cat > /tmp/exp_job_$$.sh << 'JOBSCRIPT'
{script}
JOBSCRIPT
sbatch --parsable /tmp/exp_job_$$.sh
rm -f /tmp/exp_job_$$.sh
"""
            result = run_in_slurmctld(cmd, check=False)
            if result.returncode == 0 and result.stdout.strip():
                job_ids.append(result.stdout.strip().split(";")[0])

    return job_ids


@click.group()
def experiment():
    """Run and analyze cluster experiments.

    \b
    Examples:
      playground experiment list           # List available experiments
      playground experiment run burst      # Run burst workload experiment
      playground experiment results burst  # View experiment results
    """
    pass


@experiment.command(name="list")
def list_experiments():
    """List available experiments."""
    console.print("\n[bold blue]=== Available Experiments ===[/bold blue]\n")

    if not EXPERIMENTS_DIR.exists():
        console.print("[yellow]No experiments directory found[/yellow]")
        return

    table = Table(show_header=True, header_style="bold cyan")
    table.add_column("Name")
    table.add_column("Description")
    table.add_column("Phases")

    for exp_dir in sorted(EXPERIMENTS_DIR.iterdir()):
        if exp_dir.is_dir() and not exp_dir.name.startswith("."):
            config = load_experiment(exp_dir.name)
            if config:
                description = config.get("description", "No description")
                phases = len(config.get("phases", []))
                table.add_row(exp_dir.name, description, str(phases))
            else:
                table.add_row(exp_dir.name, "[dim]No config[/dim]", "-")

    console.print(table)


@experiment.command()
@click.argument("name")
@click.option("--dry-run", is_flag=True, help="Show what would be done without executing")
@click.option("--no-wait", is_flag=True, help="Don't wait for jobs to complete")
@click.option("--timeout", default=600, help="Timeout for job completion (seconds)")
def run(name, dry_run, no_wait, timeout):
    """Run an experiment by name."""
    if not is_cluster_running():
        console.print("[yellow]Cluster is not running.[/yellow]")
        console.print("Start with: [cyan]make playground-start[/cyan]")
        return

    config = load_experiment(name)
    if not config:
        console.print(f"[red]Experiment not found: {name}[/red]")
        console.print(f"\nLooking in: {EXPERIMENTS_DIR / name}")
        console.print("\nUse 'playground experiment list' to see available experiments.")
        return

    console.print(f"\n[bold blue]=== Running Experiment: {name} ===[/bold blue]")
    console.print(f"[dim]{config.get('description', '')}[/dim]\n")

    if dry_run:
        console.print("[yellow]DRY RUN - No jobs will be submitted[/yellow]\n")

    # Results collection
    results = {
        "experiment": name,
        "config": config,
        "start_time": datetime.now().isoformat(),
        "phases": [],
        "metrics_samples": [],
    }

    # Initial metrics
    results["initial_metrics"] = collect_metrics()

    # Execute phases
    phases = config.get("phases", [])

    with Progress(
        SpinnerColumn(),
        TextColumn("[progress.description]{task.description}"),
        BarColumn(),
        TaskProgressColumn(),
        console=console,
    ) as progress:
        for i, phase in enumerate(phases, 1):
            phase_name = phase.get("name", f"Phase {i}")
            phase_result = {
                "name": phase_name,
                "start_time": datetime.now().isoformat(),
                "job_ids": [],
            }

            task = progress.add_task(f"[cyan]{phase_name}[/cyan]", total=100)

            console.print(f"\n[bold]Phase {i}: {phase_name}[/bold]")

            # Submit jobs for this phase
            jobs = phase.get("jobs", [])
            if jobs and not dry_run:
                total_jobs = sum(j.get("count", 1) for j in jobs)
                console.print(f"  Submitting {total_jobs} jobs...")

                job_ids = submit_job_batch(jobs)
                phase_result["job_ids"] = job_ids
                console.print(f"  Submitted: {len(job_ids)} jobs")

            progress.update(task, advance=50)

            # Phase delay
            delay = phase.get("delay", 0)
            if delay > 0 and not dry_run:
                console.print(f"  Waiting {delay}s...")
                time.sleep(delay)

            progress.update(task, advance=25)

            # Collect phase metrics
            if not dry_run:
                phase_result["metrics"] = collect_metrics()
                results["metrics_samples"].append(collect_metrics())

            phase_result["end_time"] = datetime.now().isoformat()
            results["phases"].append(phase_result)

            progress.update(task, advance=25)

    # Wait for completion if requested
    if not no_wait and not dry_run:
        console.print("\n[bold]Waiting for jobs to complete...[/bold]")

        with Progress(SpinnerColumn(), TextColumn("[progress.description]{task.description}"), console=console) as progress:
            task = progress.add_task("Waiting for job completion...", total=None)

            start_wait = time.time()
            while time.time() - start_wait < timeout:
                result = run_in_slurmctld("squeue -h | wc -l", check=False)
                if result.returncode == 0:
                    count = int(result.stdout.strip())
                    progress.update(task, description=f"Jobs remaining: {count}")
                    if count == 0:
                        break

                # Collect periodic metrics
                results["metrics_samples"].append(collect_metrics())
                time.sleep(5)

        console.print("[green]All jobs completed[/green]")

    # Final metrics
    if not dry_run:
        results["final_metrics"] = collect_metrics()
        results["end_time"] = datetime.now().isoformat()

        # Save results
        results_path = save_experiment_results(name, results)
        console.print(f"\n[green]Results saved to:[/green] {results_path}")

    # Summary
    console.print(f"\n[bold blue]=== Experiment Summary ===[/bold blue]")
    console.print(f"  Phases executed: {len(phases)}")
    if not dry_run:
        total_jobs = sum(len(p.get("job_ids", [])) for p in results["phases"])
        console.print(f"  Total jobs submitted: {total_jobs}")
        console.print(f"  Duration: {results.get('end_time', 'N/A')}")


@experiment.command()
@click.argument("name")
@click.option("-n", "--limit", default=5, help="Number of recent results to show")
def results(name, limit):
    """View results from previous experiment runs."""
    results_dir = EXPERIMENTS_DIR / name / "results"

    if not results_dir.exists():
        console.print(f"[yellow]No results found for experiment: {name}[/yellow]")
        return

    console.print(f"\n[bold blue]=== Results for: {name} ===[/bold blue]\n")

    # List recent results
    result_files = sorted(results_dir.glob("run_*.json"), reverse=True)[:limit]

    if not result_files:
        console.print("[dim]No result files found[/dim]")
        return

    for result_file in result_files:
        with open(result_file) as f:
            data = json.load(f)

        console.print(f"[bold]{result_file.name}[/bold]")
        console.print(f"  Start: {data.get('start_time', 'N/A')}")
        console.print(f"  End: {data.get('end_time', 'N/A')}")
        console.print(f"  Phases: {len(data.get('phases', []))}")

        # Initial vs final metrics
        initial = data.get("initial_metrics", {})
        final = data.get("final_metrics", {})

        if initial and final:
            console.print("  Metrics:")
            console.print(f"    Queue: {initial.get('queue', {}).get('total', 0)} -> {final.get('queue', {}).get('total', 0)}")
            console.print(f"    CPUs allocated: {initial.get('cpus', {}).get('allocated', 0)} -> {final.get('cpus', {}).get('allocated', 0)}")

        console.print()


@experiment.command()
@click.argument("exp1")
@click.argument("exp2")
def compare(exp1, exp2):
    """Compare results from two experiment runs.

    Arguments can be experiment names (uses latest run) or
    full paths to result files.
    """
    def load_latest_result(name_or_path: str) -> dict | None:
        path = Path(name_or_path)
        if path.exists() and path.is_file():
            with open(path) as f:
                return json.load(f)

        # Treat as experiment name
        results_dir = EXPERIMENTS_DIR / name_or_path / "results"
        if results_dir.exists():
            result_files = sorted(results_dir.glob("run_*.json"), reverse=True)
            if result_files:
                with open(result_files[0]) as f:
                    return json.load(f)

        return None

    data1 = load_latest_result(exp1)
    data2 = load_latest_result(exp2)

    if not data1:
        console.print(f"[red]Could not load results for: {exp1}[/red]")
        return
    if not data2:
        console.print(f"[red]Could not load results for: {exp2}[/red]")
        return

    console.print(f"\n[bold blue]=== Experiment Comparison ===[/bold blue]\n")

    table = Table(show_header=True, header_style="bold cyan")
    table.add_column("Metric")
    table.add_column(exp1)
    table.add_column(exp2)
    table.add_column("Diff")

    # Compare key metrics
    metrics_to_compare = [
        ("Phases", lambda d: len(d.get("phases", []))),
        ("Total Jobs", lambda d: sum(len(p.get("job_ids", [])) for p in d.get("phases", []))),
        ("Final Queue", lambda d: d.get("final_metrics", {}).get("queue", {}).get("total", 0)),
        ("Final CPUs Alloc", lambda d: d.get("final_metrics", {}).get("cpus", {}).get("allocated", 0)),
    ]

    for name, extractor in metrics_to_compare:
        v1 = extractor(data1)
        v2 = extractor(data2)
        diff = v2 - v1 if isinstance(v1, (int, float)) and isinstance(v2, (int, float)) else "N/A"
        diff_str = f"+{diff}" if isinstance(diff, (int, float)) and diff > 0 else str(diff)
        table.add_row(name, str(v1), str(v2), diff_str)

    console.print(table)


@experiment.command()
@click.argument("name")
@click.option("--force", is_flag=True, help="Overwrite existing experiment")
def create(name, force):
    """Create a new experiment from template."""
    exp_dir = EXPERIMENTS_DIR / name

    if exp_dir.exists() and not force:
        console.print(f"[red]Experiment already exists: {name}[/red]")
        console.print("Use --force to overwrite")
        return

    exp_dir.mkdir(parents=True, exist_ok=True)

    # Create template config
    config = {
        "name": name,
        "description": f"Custom experiment: {name}",
        "version": "1.0",
        "phases": [
            {
                "name": "warmup",
                "description": "Initial warmup phase",
                "jobs": [
                    {"type": "sleep", "count": 5, "duration": 10}
                ],
                "delay": 5,
            },
            {
                "name": "main",
                "description": "Main workload phase",
                "jobs": [
                    {"type": "cpu", "count": 10, "duration": 30, "cpus": 2},
                    {"type": "sleep", "count": 20, "duration": 60},
                ],
                "delay": 0,
            },
            {
                "name": "cooldown",
                "description": "Cooldown phase",
                "jobs": [
                    {"type": "sleep", "count": 5, "duration": 10}
                ],
                "delay": 0,
            },
        ],
    }

    config_path = exp_dir / "experiment.yml"
    with open(config_path, "w") as f:
        yaml.dump(config, f, default_flow_style=False, sort_keys=False)

    console.print(f"[green]Created experiment:[/green] {name}")
    console.print(f"  Config: {config_path}")
    console.print("\nEdit the config file to customize your experiment.")
    console.print(f"Run with: [cyan]playground experiment run {name}[/cyan]")
