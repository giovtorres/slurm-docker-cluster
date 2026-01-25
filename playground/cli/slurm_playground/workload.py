"""Workload generation commands for Slurm Playground."""

import json
import subprocess
import tempfile
import time
from pathlib import Path

import click
from rich.console import Console
from rich.progress import Progress, SpinnerColumn, TextColumn
from rich.table import Table

console = Console()

# Find directories
SCRIPT_DIR = Path(__file__).parent
CLI_DIR = SCRIPT_DIR.parent
PLAYGROUND_DIR = CLI_DIR.parent
PROJECT_DIR = PLAYGROUND_DIR.parent
JOBS_DIR = PLAYGROUND_DIR / "jobs"
PROFILES_DIR = PLAYGROUND_DIR / "profiles"


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


def submit_job(script_content: str, job_name: str = "playground") -> str | None:
    """Submit a job script and return the job ID."""
    # Escape the script for bash
    escaped = script_content.replace("'", "'\\''")

    # Write script to temp file in container and submit
    cmd = f"""
cat > /tmp/job_$$.sh << 'JOBSCRIPT'
{script_content}
JOBSCRIPT
sbatch --parsable /tmp/job_$$.sh
rm -f /tmp/job_$$.sh
"""
    result = run_in_slurmctld(cmd, check=False)
    if result.returncode == 0 and result.stdout.strip():
        job_id = result.stdout.strip().split(";")[0]  # Handle array job IDs
        return job_id
    else:
        console.print(f"[red]Failed to submit job: {result.stderr}[/red]")
        return None


def generate_cpu_script(
    duration: int,
    cpus: int,
    intensity: str,
    job_name: str,
    partition: str | None,
) -> str:
    """Generate a CPU stress job script."""
    partition_line = f"#SBATCH --partition={partition}" if partition else ""

    # Pure bash CPU stress (no external tools needed)
    if intensity == "light":
        loops = 1000000
    elif intensity == "heavy":
        loops = 100000000
    else:  # medium
        loops = 10000000

    return f"""#!/bin/bash
#SBATCH --job-name={job_name}
#SBATCH --ntasks=1
#SBATCH --cpus-per-task={cpus}
#SBATCH --time={duration // 60 + 1}
#SBATCH --output=/data/cpu_stress_%j.out
{partition_line}

echo "Starting CPU stress test"
echo "Hostname: $(hostname)"
echo "CPUs requested: {cpus}"
echo "Duration: {duration}s"
echo "Start time: $(date)"

# Run CPU-intensive work on each CPU
for i in $(seq 1 {cpus}); do
    (
        end=$((SECONDS + {duration}))
        while [ $SECONDS -lt $end ]; do
            for j in $(seq 1 {loops}); do
                : # CPU work
            done
        done
    ) &
done

# Wait for all background jobs
wait

echo "End time: $(date)"
echo "CPU stress test completed"
"""


def generate_memory_script(
    memory_mb: int,
    duration: int,
    pattern: str,
    job_name: str,
    partition: str | None,
) -> str:
    """Generate a memory stress job script."""
    partition_line = f"#SBATCH --partition={partition}" if partition else ""

    return f"""#!/bin/bash
#SBATCH --job-name={job_name}
#SBATCH --ntasks=1
#SBATCH --mem={memory_mb}M
#SBATCH --time={duration // 60 + 1}
#SBATCH --output=/data/memory_stress_%j.out
{partition_line}

echo "Starting memory stress test"
echo "Hostname: $(hostname)"
echo "Memory requested: {memory_mb}MB"
echo "Duration: {duration}s"
echo "Pattern: {pattern}"
echo "Start time: $(date)"

# Allocate memory using Python (more reliable than bash)
python3 << 'PYTHON'
import time
import sys

memory_mb = {memory_mb}
duration = {duration}
pattern = "{pattern}"

# Allocate approximately the requested memory
# Each character is ~1 byte, so we need memory_mb * 1024 * 1024 bytes
# Use a list of strings to allocate memory
chunk_size = 1024 * 1024  # 1MB chunks
num_chunks = int(memory_mb * 0.8)  # Use 80% to leave room for overhead

print(f"Allocating {{num_chunks}} MB...")
data = []

try:
    for i in range(num_chunks):
        data.append('x' * chunk_size)
        if (i + 1) % 100 == 0:
            print(f"  Allocated {{i + 1}} MB")

    print(f"Memory allocated, holding for {{duration}} seconds...")

    if pattern == "random":
        import random
        for _ in range(duration):
            # Access random chunks
            for _ in range(100):
                idx = random.randint(0, len(data) - 1)
                _ = data[idx][0]
            time.sleep(1)
    else:
        # Sequential or hold pattern
        time.sleep(duration)

except MemoryError:
    print("Warning: Could not allocate all requested memory")
    time.sleep(duration)

print("Memory test completed")
PYTHON

echo "End time: $(date)"
"""


def generate_io_script(
    file_size_mb: int,
    duration: int,
    pattern: str,
    job_name: str,
    partition: str | None,
) -> str:
    """Generate an I/O stress job script."""
    partition_line = f"#SBATCH --partition={partition}" if partition else ""

    return f"""#!/bin/bash
#SBATCH --job-name={job_name}
#SBATCH --ntasks=1
#SBATCH --time={duration // 60 + 1}
#SBATCH --output=/data/io_stress_%j.out
{partition_line}

echo "Starting I/O stress test"
echo "Hostname: $(hostname)"
echo "File size: {file_size_mb}MB"
echo "Duration: {duration}s"
echo "Pattern: {pattern}"
echo "Start time: $(date)"

TESTFILE="/tmp/io_test_$$"
FILESIZE={file_size_mb}

end=$((SECONDS + {duration}))
iteration=0

while [ $SECONDS -lt $end ]; do
    iteration=$((iteration + 1))
    echo "Iteration $iteration"

    # Write test
    echo "  Writing ${{FILESIZE}}MB..."
    dd if=/dev/zero of=$TESTFILE bs=1M count=$FILESIZE 2>/dev/null

    # Read test
    echo "  Reading ${{FILESIZE}}MB..."
    dd if=$TESTFILE of=/dev/null bs=1M 2>/dev/null

    # Cleanup
    rm -f $TESTFILE
done

echo "Completed $iteration iterations"
echo "End time: $(date)"
"""


def generate_sleep_script(
    duration: int,
    job_name: str,
    partition: str | None,
) -> str:
    """Generate a simple sleep job script."""
    partition_line = f"#SBATCH --partition={partition}" if partition else ""

    return f"""#!/bin/bash
#SBATCH --job-name={job_name}
#SBATCH --ntasks=1
#SBATCH --time={duration // 60 + 1}
#SBATCH --output=/data/sleep_%j.out
{partition_line}

echo "Sleep job started"
echo "Hostname: $(hostname)"
echo "Duration: {duration}s"
echo "Start time: $(date)"
sleep {duration}
echo "End time: $(date)"
echo "Sleep job completed"
"""


def generate_workflow_script(
    stages: int,
    stage_duration: int,
    job_name: str,
    partition: str | None,
) -> list[str]:
    """Generate workflow job scripts with dependencies."""
    partition_line = f"#SBATCH --partition={partition}" if partition else ""
    scripts = []

    for stage in range(1, stages + 1):
        script = f"""#!/bin/bash
#SBATCH --job-name={job_name}_stage{stage}
#SBATCH --ntasks=1
#SBATCH --time={stage_duration // 60 + 1}
#SBATCH --output=/data/workflow_stage{stage}_%j.out
{partition_line}

echo "Workflow Stage {stage}/{stages}"
echo "Hostname: $(hostname)"
echo "Start time: $(date)"
echo "Processing stage {stage}..."
sleep {stage_duration}
echo "Stage {stage} completed"
echo "End time: $(date)"
"""
        scripts.append(script)

    return scripts


@click.group()
def workload():
    """Generate and submit workloads to the cluster.

    \b
    Examples:
      playground workload cpu --count=10 --duration=60
      playground workload memory --count=5 --memory=2G
      playground workload burst --jobs=100 --interval=0.5
      playground workload workflow --stages=3
    """
    pass


@workload.command()
@click.option("-c", "--count", default=1, help="Number of jobs to submit")
@click.option("--cpus", default=2, help="CPUs per job")
@click.option("-d", "--duration", default=60, help="Duration in seconds")
@click.option(
    "--intensity",
    type=click.Choice(["light", "medium", "heavy"]),
    default="medium",
    help="CPU intensity",
)
@click.option("-p", "--partition", default=None, help="Target partition")
@click.option("--name", default="cpu_stress", help="Job name prefix")
def cpu(count, cpus, duration, intensity, partition, name):
    """Submit CPU-bound stress jobs."""
    console.print(f"[bold]Submitting {count} CPU stress job(s)[/bold]")
    console.print(f"  CPUs: {cpus}, Duration: {duration}s, Intensity: {intensity}")

    job_ids = []
    with Progress(
        SpinnerColumn(),
        TextColumn("[progress.description]{task.description}"),
        console=console,
    ) as progress:
        task = progress.add_task(f"Submitting jobs...", total=count)

        for i in range(count):
            script = generate_cpu_script(duration, cpus, intensity, f"{name}_{i+1}", partition)
            job_id = submit_job(script, name)
            if job_id:
                job_ids.append(job_id)
            progress.update(task, advance=1)

    console.print(f"\n[green]Submitted {len(job_ids)} jobs:[/green] {', '.join(job_ids)}")


@workload.command()
@click.option("-c", "--count", default=1, help="Number of jobs to submit")
@click.option("-m", "--memory", default="1G", help="Memory per job (e.g., 500M, 2G)")
@click.option("-d", "--duration", default=60, help="Duration in seconds")
@click.option(
    "--pattern",
    type=click.Choice(["hold", "sequential", "random"]),
    default="hold",
    help="Memory access pattern",
)
@click.option("-p", "--partition", default=None, help="Target partition")
@click.option("--name", default="mem_stress", help="Job name prefix")
def memory(count, memory, duration, pattern, partition, name):
    """Submit memory-bound stress jobs."""
    # Parse memory string
    memory_str = memory.upper()
    if memory_str.endswith("G"):
        memory_mb = int(float(memory_str[:-1]) * 1024)
    elif memory_str.endswith("M"):
        memory_mb = int(memory_str[:-1])
    else:
        memory_mb = int(memory)

    console.print(f"[bold]Submitting {count} memory stress job(s)[/bold]")
    console.print(f"  Memory: {memory_mb}MB, Duration: {duration}s, Pattern: {pattern}")

    job_ids = []
    with Progress(
        SpinnerColumn(),
        TextColumn("[progress.description]{task.description}"),
        console=console,
    ) as progress:
        task = progress.add_task(f"Submitting jobs...", total=count)

        for i in range(count):
            script = generate_memory_script(
                memory_mb, duration, pattern, f"{name}_{i+1}", partition
            )
            job_id = submit_job(script, name)
            if job_id:
                job_ids.append(job_id)
            progress.update(task, advance=1)

    console.print(f"\n[green]Submitted {len(job_ids)} jobs:[/green] {', '.join(job_ids)}")


@workload.command()
@click.option("-c", "--count", default=1, help="Number of jobs to submit")
@click.option("-s", "--size", default="100M", help="File size for I/O (e.g., 100M, 1G)")
@click.option("-d", "--duration", default=60, help="Duration in seconds")
@click.option(
    "--pattern",
    type=click.Choice(["sequential", "random"]),
    default="sequential",
    help="I/O pattern",
)
@click.option("-p", "--partition", default=None, help="Target partition")
@click.option("--name", default="io_stress", help="Job name prefix")
def io(count, size, duration, pattern, partition, name):
    """Submit I/O-bound stress jobs."""
    # Parse size string
    size_str = size.upper()
    if size_str.endswith("G"):
        size_mb = int(float(size_str[:-1]) * 1024)
    elif size_str.endswith("M"):
        size_mb = int(size_str[:-1])
    else:
        size_mb = int(size)

    console.print(f"[bold]Submitting {count} I/O stress job(s)[/bold]")
    console.print(f"  File size: {size_mb}MB, Duration: {duration}s, Pattern: {pattern}")

    job_ids = []
    with Progress(
        SpinnerColumn(),
        TextColumn("[progress.description]{task.description}"),
        console=console,
    ) as progress:
        task = progress.add_task(f"Submitting jobs...", total=count)

        for i in range(count):
            script = generate_io_script(size_mb, duration, pattern, f"{name}_{i+1}", partition)
            job_id = submit_job(script, name)
            if job_id:
                job_ids.append(job_id)
            progress.update(task, advance=1)

    console.print(f"\n[green]Submitted {len(job_ids)} jobs:[/green] {', '.join(job_ids)}")


@workload.command()
@click.option("-c", "--count", default=1, help="Number of jobs to submit")
@click.option("-d", "--duration", default=60, help="Sleep duration in seconds")
@click.option("-p", "--partition", default=None, help="Target partition")
@click.option("--name", default="sleep", help="Job name prefix")
def sleep(count, duration, partition, name):
    """Submit simple sleep jobs for queue testing."""
    console.print(f"[bold]Submitting {count} sleep job(s)[/bold]")
    console.print(f"  Duration: {duration}s")

    job_ids = []
    with Progress(
        SpinnerColumn(),
        TextColumn("[progress.description]{task.description}"),
        console=console,
    ) as progress:
        task = progress.add_task(f"Submitting jobs...", total=count)

        for i in range(count):
            script = generate_sleep_script(duration, f"{name}_{i+1}", partition)
            job_id = submit_job(script, name)
            if job_id:
                job_ids.append(job_id)
            progress.update(task, advance=1)

    console.print(f"\n[green]Submitted {len(job_ids)} jobs:[/green] {', '.join(job_ids)}")


@workload.command()
@click.option("-j", "--jobs", default=50, help="Total number of jobs to submit")
@click.option("-i", "--interval", default=0.5, help="Interval between submissions (seconds)")
@click.option("-d", "--duration", default=30, help="Job duration in seconds")
@click.option(
    "-t",
    "--type",
    "job_type",
    type=click.Choice(["sleep", "cpu", "mixed"]),
    default="sleep",
    help="Type of jobs",
)
@click.option("-p", "--partition", default=None, help="Target partition")
def burst(jobs, interval, duration, job_type, partition):
    """Submit a burst of jobs with configurable interval.

    Useful for testing queue behavior under sudden load.
    """
    console.print(f"[bold]Submitting burst of {jobs} {job_type} jobs[/bold]")
    console.print(f"  Interval: {interval}s, Duration: {duration}s")

    job_ids = []
    with Progress(
        SpinnerColumn(),
        TextColumn("[progress.description]{task.description}"),
        console=console,
    ) as progress:
        task = progress.add_task(f"Submitting jobs...", total=jobs)

        for i in range(jobs):
            if job_type == "sleep":
                script = generate_sleep_script(duration, f"burst_{i+1}", partition)
            elif job_type == "cpu":
                script = generate_cpu_script(duration, 1, "light", f"burst_{i+1}", partition)
            else:  # mixed
                if i % 3 == 0:
                    script = generate_cpu_script(duration, 1, "light", f"burst_{i+1}", partition)
                elif i % 3 == 1:
                    script = generate_memory_script(256, duration, "hold", f"burst_{i+1}", partition)
                else:
                    script = generate_sleep_script(duration, f"burst_{i+1}", partition)

            job_id = submit_job(script, "burst")
            if job_id:
                job_ids.append(job_id)
            progress.update(task, advance=1)

            if interval > 0 and i < jobs - 1:
                time.sleep(interval)

    console.print(f"\n[green]Submitted {len(job_ids)} jobs[/green]")


@workload.command()
@click.option("-s", "--stages", default=3, help="Number of workflow stages")
@click.option("-d", "--duration", default=30, help="Duration per stage (seconds)")
@click.option("-p", "--partition", default=None, help="Target partition")
@click.option("--name", default="workflow", help="Workflow name")
def workflow(stages, duration, partition, name):
    """Submit a workflow with dependent stages.

    Creates a chain of jobs where each stage depends on the previous.
    """
    console.print(f"[bold]Submitting {stages}-stage workflow[/bold]")
    console.print(f"  Stage duration: {duration}s")

    scripts = generate_workflow_script(stages, duration, name, partition)
    job_ids = []

    prev_job_id = None
    for i, script in enumerate(scripts, 1):
        if prev_job_id:
            # Add dependency to script
            script = script.replace(
                "#SBATCH --ntasks=1",
                f"#SBATCH --ntasks=1\n#SBATCH --dependency=afterok:{prev_job_id}",
            )

        job_id = submit_job(script, f"{name}_stage{i}")
        if job_id:
            job_ids.append(job_id)
            prev_job_id = job_id
            console.print(f"  Stage {i}: Job {job_id}" + (f" (depends on {job_ids[-2]})" if len(job_ids) > 1 else ""))
        else:
            console.print(f"[red]Failed to submit stage {i}[/red]")
            break

    console.print(f"\n[green]Workflow submitted:[/green] {' -> '.join(job_ids)}")


@workload.command()
@click.argument("array_spec")
@click.option("-d", "--duration", default=30, help="Duration per task (seconds)")
@click.option("-p", "--partition", default=None, help="Target partition")
@click.option("--name", default="array", help="Job name")
def array(array_spec, duration, partition, name):
    """Submit a job array.

    ARRAY_SPEC is the array specification (e.g., "1-100", "1-50%10").
    """
    partition_line = f"#SBATCH --partition={partition}" if partition else ""

    script = f"""#!/bin/bash
#SBATCH --job-name={name}
#SBATCH --array={array_spec}
#SBATCH --ntasks=1
#SBATCH --time={duration // 60 + 1}
#SBATCH --output=/data/array_%A_%a.out
{partition_line}

echo "Array job task $SLURM_ARRAY_TASK_ID of $SLURM_ARRAY_JOB_ID"
echo "Hostname: $(hostname)"
echo "Start time: $(date)"
sleep {duration}
echo "End time: $(date)"
echo "Task completed"
"""

    console.print(f"[bold]Submitting array job: {array_spec}[/bold]")

    job_id = submit_job(script, name)
    if job_id:
        console.print(f"[green]Submitted array job:[/green] {job_id}")
    else:
        console.print("[red]Failed to submit array job[/red]")


@workload.command()
@click.argument("profile_name")
def profile(profile_name):
    """Submit jobs from a predefined profile.

    Load and execute a workload profile from playground/profiles/.
    """
    profile_path = PROFILES_DIR / f"{profile_name}.json"

    if not profile_path.exists():
        console.print(f"[red]Profile not found: {profile_path}[/red]")
        console.print("\nAvailable profiles:")
        for p in PROFILES_DIR.glob("*.json"):
            console.print(f"  - {p.stem}")
        return

    with open(profile_path) as f:
        profile_data = json.load(f)

    console.print(f"[bold]Loading profile: {profile_name}[/bold]")
    console.print(f"  Description: {profile_data.get('description', 'N/A')}")

    # Execute each phase
    for phase in profile_data.get("phases", []):
        phase_name = phase.get("name", "unnamed")
        console.print(f"\n[cyan]Phase: {phase_name}[/cyan]")

        for job_spec in phase.get("jobs", []):
            job_type = job_spec.get("type", "sleep")
            count = job_spec.get("count", 1)
            duration = job_spec.get("duration", 30)

            if job_type == "sleep":
                script = generate_sleep_script(duration, job_spec.get("name", "profile_job"), None)
            elif job_type == "cpu":
                script = generate_cpu_script(
                    duration,
                    job_spec.get("cpus", 1),
                    job_spec.get("intensity", "medium"),
                    job_spec.get("name", "profile_job"),
                    None,
                )
            elif job_type == "memory":
                script = generate_memory_script(
                    job_spec.get("memory_mb", 512),
                    duration,
                    job_spec.get("pattern", "hold"),
                    job_spec.get("name", "profile_job"),
                    None,
                )
            else:
                console.print(f"  [yellow]Unknown job type: {job_type}[/yellow]")
                continue

            for i in range(count):
                job_id = submit_job(script, f"{phase_name}_{job_type}")
                if job_id:
                    console.print(f"  Submitted: {job_id}")

        # Delay between phases
        delay = phase.get("delay", 0)
        if delay > 0:
            console.print(f"  Waiting {delay}s before next phase...")
            time.sleep(delay)

    console.print("\n[green]Profile execution complete[/green]")


@workload.command(name="list-profiles")
def list_profiles():
    """List available workload profiles."""
    console.print("[bold]Available Workload Profiles:[/bold]\n")

    if not PROFILES_DIR.exists():
        console.print("[yellow]No profiles directory found[/yellow]")
        return

    for profile_path in sorted(PROFILES_DIR.glob("*.json")):
        try:
            with open(profile_path) as f:
                data = json.load(f)
            description = data.get("description", "No description")
            console.print(f"  [cyan]{profile_path.stem}[/cyan]: {description}")
        except json.JSONDecodeError:
            console.print(f"  [red]{profile_path.stem}[/red]: Invalid JSON")
