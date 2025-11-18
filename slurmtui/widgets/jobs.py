"""
Job queue widget showing SLURM jobs.
"""

from textual.app import ComposeResult
from textual.containers import Container
from textual.widgets import DataTable, Label
from rich.text import Text

from ..api.models import JobInfo
from ..utils.formatters import (
    format_job_state,
    format_duration,
    format_timestamp,
    format_node_list,
    colorize_state,
)


class JobsWidget(Container):
    """Widget displaying SLURM jobs in a table."""

    DEFAULT_CSS = """
    JobsWidget {
        layout: vertical;
        padding: 1;
    }

    .jobs-header {
        height: 3;
        background: $boost;
        padding: 1;
        margin-bottom: 1;
    }

    .jobs-table {
        height: 1fr;
    }
    """

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.jobs = []

    def compose(self) -> ComposeResult:
        """Create child widgets."""
        yield Label("⚙️  Job Queue", classes="jobs-header")
        yield DataTable(id="jobs-table", classes="jobs-table")

    def on_mount(self):
        """Initialize the data table."""
        table = self.query_one("#jobs-table", DataTable)
        table.cursor_type = "row"

        # Add columns
        table.add_column("Job ID", width=10)
        table.add_column("User", width=12)
        table.add_column("Name", width=20)
        table.add_column("State", width=12)
        table.add_column("Partition", width=12)
        table.add_column("Nodes", width=15)
        table.add_column("CPUs", width=6)
        table.add_column("Time", width=12)

    def update_jobs(self, jobs: list[JobInfo], show_completed: bool = False):
        """Update the jobs table with new data."""
        # Filter jobs if needed
        if not show_completed:
            jobs = [j for j in jobs if not j.is_completed]

        self.jobs = jobs
        table = self.query_one("#jobs-table", DataTable)

        # Clear existing rows
        table.clear()

        # Add job rows
        for job in jobs:
            # Format state with color
            state_color = colorize_state(job.job_state, "job")
            state_text = Text(format_job_state(job.job_state), style=state_color)

            # Format runtime or wait time
            if job.is_running:
                time_text = format_duration(job.runtime_seconds)
            elif job.is_pending:
                time_text = f"Wait: {format_duration(job.wait_time_seconds)}"
            else:
                time_text = format_duration(job.runtime_seconds)

            # Format nodes
            nodes_text = format_node_list(job.nodes, max_display=2)

            # Add row
            table.add_row(
                str(job.job_id),
                job.user_name,
                job.name[:18] + ".." if len(job.name) > 20 else job.name,
                state_text,
                job.partition,
                nodes_text,
                str(job.cpus),
                time_text,
            )

        self.refresh()
