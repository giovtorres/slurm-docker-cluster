"""
Node status widget showing cluster nodes.
"""

from textual.app import ComposeResult
from textual.containers import Container
from textual.widgets import DataTable, Label
from rich.text import Text

from ..api.models import NodeInfo
from ..utils.formatters import (
    format_node_state,
    format_cpu_allocation,
    format_memory_mb,
    format_percentage,
    colorize_state,
)


class NodesWidget(Container):
    """Widget displaying cluster nodes in a table."""

    DEFAULT_CSS = """
    NodesWidget {
        layout: vertical;
        padding: 1;
    }

    .nodes-header {
        height: 3;
        background: $boost;
        padding: 1;
        margin-bottom: 1;
    }

    .nodes-table {
        height: 1fr;
    }
    """

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.nodes = []

    def compose(self) -> ComposeResult:
        """Create child widgets."""
        yield Label("📡 Cluster Nodes", classes="nodes-header")
        yield DataTable(id="nodes-table", classes="nodes-table")

    def on_mount(self):
        """Initialize the data table."""
        table = self.query_one("#nodes-table", DataTable)
        table.cursor_type = "row"

        # Add columns
        table.add_column("Node", width=12)
        table.add_column("State", width=15)
        table.add_column("CPUs", width=18)
        table.add_column("Memory", width=20)
        table.add_column("Partitions", width=15)

    def update_nodes(self, nodes: list[NodeInfo]):
        """Update the nodes table with new data."""
        self.nodes = nodes
        table = self.query_one("#nodes-table", DataTable)

        # Clear existing rows
        table.clear()

        # Add node rows
        for node in nodes:
            # Format state with color
            state_color = colorize_state(node.state, "node")
            state_text = Text(format_node_state(node.state), style=state_color)

            # Format CPU allocation
            cpu_text = f"{node.cpus_allocated}/{node.cpus} ({format_percentage(node.cpu_utilization)})"

            # Format memory allocation
            memory_alloc_gb = node.alloc_memory / 1024
            memory_total_gb = node.real_memory / 1024
            memory_text = f"{memory_alloc_gb:.1f}/{memory_total_gb:.1f} GB ({format_percentage(node.memory_utilization)})"

            # Format partitions
            partitions = ", ".join(node.partitions) if node.partitions else "N/A"

            # Add row
            table.add_row(
                node.name,
                state_text,
                cpu_text,
                memory_text,
                partitions,
            )

        self.refresh()
