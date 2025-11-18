"""
Dashboard widget showing cluster overview.
"""

from textual.app import ComposeResult
from textual.containers import Container, Horizontal, Vertical
from textual.widgets import Static, Label
from rich.text import Text

from ..api.models import ClusterStats
from ..utils.formatters import (
    format_percentage,
    format_cpu_allocation,
    format_memory_allocation,
    create_progress_bar,
)


class StatCard(Static):
    """A card displaying a single statistic."""

    def __init__(self, title: str, value: str, subtitle: str = "", **kwargs):
        super().__init__(**kwargs)
        self.title_text = title
        self.value_text = value
        self.subtitle_text = subtitle

    def compose(self) -> ComposeResult:
        yield Label(self.title_text, classes="stat-title")
        yield Label(self.value_text, classes="stat-value")
        if self.subtitle_text:
            yield Label(self.subtitle_text, classes="stat-subtitle")

    def update_value(self, value: str, subtitle: str = ""):
        """Update the card's value."""
        self.value_text = value
        self.subtitle_text = subtitle
        self.refresh()


class DashboardWidget(Container):
    """Main dashboard showing cluster overview."""

    DEFAULT_CSS = """
    DashboardWidget {
        layout: vertical;
        padding: 1;
    }

    .dashboard-header {
        height: 3;
        background: $boost;
        padding: 1;
        margin-bottom: 1;
    }

    .stats-row {
        layout: horizontal;
        height: auto;
        margin-bottom: 1;
    }

    .stat-card {
        border: solid $primary;
        padding: 1;
        margin-right: 1;
        width: 1fr;
        height: auto;
    }

    .stat-title {
        color: $text-muted;
        text-style: bold;
    }

    .stat-value {
        color: $primary;
        text-style: bold;
        text-align: center;
    }

    .stat-subtitle {
        color: $text-muted;
        text-align: center;
    }

    .progress-section {
        padding: 1;
        border: solid $primary;
        margin-bottom: 1;
    }

    .progress-label {
        color: $text;
        margin-bottom: 1;
    }

    .progress-bar {
        color: $success;
    }
    """

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.stats: ClusterStats | None = None

    def compose(self) -> ComposeResult:
        """Create child widgets."""
        with Vertical(classes="dashboard-header"):
            yield Label("🖥️  SLURM Cluster Dashboard", classes="title")

        with Horizontal(classes="stats-row"):
            yield StatCard("Nodes", "0/0", "0% Available", classes="stat-card", id="nodes-card")
            yield StatCard("Jobs", "0", "0 Running", classes="stat-card", id="jobs-card")
            yield StatCard("CPUs", "0/0", "0% Used", classes="stat-card", id="cpus-card")
            yield StatCard(
                "Memory", "0/0 GB", "0% Used", classes="stat-card", id="memory-card"
            )

        with Vertical(classes="progress-section"):
            yield Label("Resource Utilization", classes="progress-label")
            yield Static("", id="cpu-progress", classes="progress-bar")
            yield Static("", id="memory-progress", classes="progress-bar")

    def update_stats(self, stats: ClusterStats):
        """Update dashboard with new cluster statistics."""
        self.stats = stats

        # Update stat cards
        nodes_card = self.query_one("#nodes-card", StatCard)
        nodes_card.value_text = f"{stats.nodes_up}/{stats.total_nodes}"
        nodes_card.subtitle_text = f"{format_percentage(stats.node_availability)} Available"

        jobs_card = self.query_one("#jobs-card", StatCard)
        jobs_card.value_text = str(stats.total_jobs)
        jobs_card.subtitle_text = f"{stats.jobs_running} Running, {stats.jobs_pending} Pending"

        cpus_card = self.query_one("#cpus-card", StatCard)
        cpus_card.value_text = f"{stats.cpus_allocated}/{stats.total_cpus}"
        cpus_card.subtitle_text = f"{format_percentage(stats.cpu_utilization)} Used"

        memory_card = self.query_one("#memory-card", StatCard)
        memory_gb_alloc = stats.memory_allocated_mb / 1024
        memory_gb_total = stats.total_memory_mb / 1024
        memory_card.value_text = f"{memory_gb_alloc:.1f}/{memory_gb_total:.1f} GB"
        memory_card.subtitle_text = f"{format_percentage(stats.memory_utilization)} Used"

        # Update progress bars
        cpu_bar = create_progress_bar(stats.cpus_allocated, stats.total_cpus, width=40)
        cpu_text = Text()
        cpu_text.append("CPUs:   ")
        cpu_text.append(cpu_bar)
        cpu_text.append(
            f"  {stats.cpus_allocated}/{stats.total_cpus} ({format_percentage(stats.cpu_utilization)})"
        )
        self.query_one("#cpu-progress", Static).update(cpu_text)

        memory_bar = create_progress_bar(
            stats.memory_allocated_mb, stats.total_memory_mb, width=40
        )
        memory_text = Text()
        memory_text.append("Memory: ")
        memory_text.append(memory_bar)
        memory_text.append(
            f"  {memory_gb_alloc:.1f}/{memory_gb_total:.1f} GB ({format_percentage(stats.memory_utilization)})"
        )
        self.query_one("#memory-progress", Static).update(memory_text)

        self.refresh()
