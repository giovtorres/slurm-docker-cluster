"""
Main Textual application for SLURM TUI Monitor.
"""

import asyncio
import logging
from typing import Optional

from textual.app import App, ComposeResult
from textual.containers import Container, Vertical
from textual.widgets import Header, Footer, TabbedContent, TabPane, Label
from textual.binding import Binding

from .config import Config
from .api.client import SlurmAPIClient, SlurmAPIError
from .widgets.dashboard import DashboardWidget
from .widgets.nodes import NodesWidget
from .widgets.jobs import JobsWidget


logger = logging.getLogger(__name__)


class ErrorDisplay(Container):
    """Widget to display errors."""

    DEFAULT_CSS = """
    ErrorDisplay {
        background: $error;
        color: $text;
        padding: 1;
        margin: 1;
    }
    """

    def __init__(self, message: str, **kwargs):
        super().__init__(**kwargs)
        self.message = message

    def compose(self) -> ComposeResult:
        yield Label(f"❌ Error: {self.message}")


class SlurmTUI(App):
    """SLURM TUI Monitor - Real-time cluster monitoring."""

    CSS = """
    TabbedContent {
        height: 1fr;
    }

    TabPane {
        padding: 0;
    }

    .title {
        text-style: bold;
        color: $accent;
    }
    """

    BINDINGS = [
        Binding("d", "switch_tab('dashboard')", "Dashboard", show=True),
        Binding("j", "switch_tab('jobs')", "Jobs", show=True),
        Binding("n", "switch_tab('nodes')", "Nodes", show=True),
        Binding("r", "refresh_data", "Refresh", show=True),
        Binding("q", "quit", "Quit", show=True),
    ]

    def __init__(self, config: Config):
        super().__init__()
        self.config = config
        self.api_client: Optional[SlurmAPIClient] = None
        self.refresh_timer: Optional[asyncio.Task] = None
        self.is_refreshing = False

        # Configure theme
        if config.app.theme == "light":
            self.theme = "textual-light"
        else:
            self.theme = "textual-dark"

        # Title
        self.title = "SLURM TUI Monitor"
        self.sub_title = "Real-time Cluster Monitoring"

    def compose(self) -> ComposeResult:
        """Create child widgets."""
        yield Header()

        with TabbedContent(initial="dashboard"):
            with TabPane("Dashboard", id="dashboard"):
                yield DashboardWidget(id="dashboard-widget")

            with TabPane("Nodes", id="nodes"):
                yield NodesWidget(id="nodes-widget")

            with TabPane("Jobs", id="jobs"):
                yield JobsWidget(id="jobs-widget")

        yield Footer()

    async def on_mount(self):
        """Initialize the application."""
        try:
            # Initialize API client
            self.api_client = SlurmAPIClient(
                base_url=self.config.slurm.api_url,
                api_version=self.config.slurm.api_version,
                auth_token=self.config.slurm.auth_token,
                timeout=self.config.slurm.timeout,
            )

            # Set cache duration
            self.api_client._cache_duration = self.config.advanced.cache_duration

            # Test connection
            await self.api_client.ping()
            logger.info("Successfully connected to SLURM API")

            # Initial data fetch
            await self.refresh_data()

            # Start auto-refresh timer if enabled
            if self.config.app.refresh_interval > 0:
                self.refresh_timer = asyncio.create_task(self._auto_refresh_loop())
                logger.info(
                    f"Auto-refresh enabled (interval: {self.config.app.refresh_interval}s)"
                )

        except Exception as e:
            logger.error(f"Failed to initialize: {e}")
            self.notify(f"Connection failed: {e}", severity="error", timeout=10)

    async def on_unmount(self):
        """Cleanup on application exit."""
        # Cancel refresh timer
        if self.refresh_timer:
            self.refresh_timer.cancel()
            try:
                await self.refresh_timer
            except asyncio.CancelledError:
                pass

        # Close API client
        if self.api_client:
            await self.api_client.close()

        logger.info("Application shutdown complete")

    async def _auto_refresh_loop(self):
        """Automatic refresh loop."""
        try:
            while True:
                await asyncio.sleep(self.config.app.refresh_interval)
                if not self.is_refreshing:
                    await self.refresh_data()
        except asyncio.CancelledError:
            logger.info("Auto-refresh loop cancelled")
        except Exception as e:
            logger.error(f"Auto-refresh loop error: {e}")

    async def action_refresh_data(self):
        """Action to manually refresh data."""
        await self.refresh_data()
        self.notify("Data refreshed", timeout=2)

    async def refresh_data(self):
        """Fetch fresh data from SLURM API and update widgets."""
        if not self.api_client:
            logger.warning("API client not initialized")
            return

        if self.is_refreshing:
            logger.debug("Refresh already in progress, skipping")
            return

        self.is_refreshing = True

        try:
            logger.debug("Fetching cluster data...")

            # Fetch all data in parallel
            stats, nodes, jobs = await asyncio.gather(
                self.api_client.get_cluster_stats(),
                self.api_client.get_nodes(),
                self.api_client.get_jobs(),
                return_exceptions=True,
            )

            # Handle errors
            if isinstance(stats, Exception):
                logger.error(f"Failed to get cluster stats: {stats}")
                self.notify(f"Error fetching stats: {stats}", severity="warning")
                return

            if isinstance(nodes, Exception):
                logger.error(f"Failed to get nodes: {nodes}")
                nodes = []

            if isinstance(jobs, Exception):
                logger.error(f"Failed to get jobs: {jobs}")
                jobs = []

            # Update dashboard
            try:
                dashboard = self.query_one("#dashboard-widget", DashboardWidget)
                dashboard.update_stats(stats)
            except Exception as e:
                logger.error(f"Failed to update dashboard: {e}")

            # Update nodes widget
            try:
                nodes_widget = self.query_one("#nodes-widget", NodesWidget)
                nodes_widget.update_nodes(nodes)
            except Exception as e:
                logger.error(f"Failed to update nodes widget: {e}")

            # Update jobs widget
            try:
                jobs_widget = self.query_one("#jobs-widget", JobsWidget)
                jobs_widget.update_jobs(
                    jobs, show_completed=self.config.display.show_completed_jobs
                )
            except Exception as e:
                logger.error(f"Failed to update jobs widget: {e}")

            logger.debug(
                f"Data refresh complete: {len(nodes)} nodes, {len(jobs)} jobs"
            )

        except SlurmAPIError as e:
            logger.error(f"API error during refresh: {e}")
            self.notify(f"API Error: {e}", severity="error", timeout=5)

        except Exception as e:
            logger.error(f"Unexpected error during refresh: {e}", exc_info=True)
            self.notify(f"Error: {e}", severity="error", timeout=5)

        finally:
            self.is_refreshing = False

    def action_switch_tab(self, tab_id: str):
        """Switch to a specific tab."""
        try:
            tabbed_content = self.query_one(TabbedContent)
            tabbed_content.active = tab_id
            logger.debug(f"Switched to tab: {tab_id}")
        except Exception as e:
            logger.error(f"Failed to switch tab: {e}")


def run_app(config: Config):
    """
    Run the SLURM TUI Monitor application.

    Args:
        config: Application configuration
    """
    # Setup logging
    config.setup_logging()

    logger.info("Starting SLURM TUI Monitor")
    logger.info(f"API URL: {config.slurm.api_url}")
    logger.info(f"API Version: {config.slurm.api_version}")
    logger.info(f"Refresh interval: {config.app.refresh_interval}s")

    # Create and run app
    app = SlurmTUI(config)

    try:
        app.run()
    except KeyboardInterrupt:
        logger.info("Application interrupted by user")
    except Exception as e:
        logger.error(f"Application error: {e}", exc_info=True)
        raise
