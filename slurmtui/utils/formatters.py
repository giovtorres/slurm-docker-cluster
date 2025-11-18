"""
Utility functions for formatting data for display.
"""

from datetime import datetime, timedelta
from typing import Optional


def format_bytes(bytes_value: int, unit: str = "auto") -> str:
    """
    Format bytes into human-readable string.

    Args:
        bytes_value: Value in bytes
        unit: Target unit (B, KB, MB, GB, TB, or auto)

    Returns:
        Formatted string (e.g., "1.5 GB")
    """
    units = ["B", "KB", "MB", "GB", "TB", "PB"]
    value = float(bytes_value)

    if unit != "auto":
        unit_index = units.index(unit.upper())
        value /= 1024**unit_index
        return f"{value:.2f} {unit.upper()}"

    for u in units:
        if value < 1024.0:
            return f"{value:.2f} {u}"
        value /= 1024.0

    return f"{value:.2f} PB"


def format_memory_mb(mb_value: int, in_gb: bool = True) -> str:
    """
    Format memory from MB to human-readable string.

    Args:
        mb_value: Memory value in MB
        in_gb: Display in GB if True, MB if False

    Returns:
        Formatted string (e.g., "2.5 GB" or "2500 MB")
    """
    if in_gb and mb_value >= 1024:
        gb_value = mb_value / 1024
        return f"{gb_value:.2f} GB"
    return f"{mb_value} MB"


def format_duration(seconds: Optional[int]) -> str:
    """
    Format duration in seconds to human-readable string.

    Args:
        seconds: Duration in seconds

    Returns:
        Formatted string (e.g., "2h 30m", "45s", "3d 2h")
    """
    if seconds is None or seconds < 0:
        return "N/A"

    if seconds == 0:
        return "0s"

    duration = timedelta(seconds=seconds)
    days = duration.days
    hours, remainder = divmod(duration.seconds, 3600)
    minutes, secs = divmod(remainder, 60)

    parts = []
    if days > 0:
        parts.append(f"{days}d")
    if hours > 0:
        parts.append(f"{hours}h")
    if minutes > 0:
        parts.append(f"{minutes}m")
    if secs > 0 and not parts:  # Only show seconds if nothing else
        parts.append(f"{secs}s")

    return " ".join(parts) if parts else "0s"


def format_timestamp(
    dt: Optional[datetime], time_format: str = "%Y-%m-%d %H:%M:%S"
) -> str:
    """
    Format datetime object to string.

    Args:
        dt: datetime object
        time_format: Format string (default: "%Y-%m-%d %H:%M:%S")

    Returns:
        Formatted timestamp or "N/A"
    """
    if dt is None:
        return "N/A"

    try:
        return dt.strftime(time_format)
    except Exception:
        return "Invalid"


def format_relative_time(dt: Optional[datetime]) -> str:
    """
    Format datetime as relative time (e.g., "5 minutes ago", "in 2 hours").

    Args:
        dt: datetime object

    Returns:
        Relative time string
    """
    if dt is None:
        return "N/A"

    now = datetime.now()
    diff = now - dt

    if diff.total_seconds() < 0:
        # Future time
        diff = -diff
        suffix = "from now"
    else:
        suffix = "ago"

    seconds = int(diff.total_seconds())

    if seconds < 60:
        return f"{seconds}s {suffix}"
    elif seconds < 3600:
        minutes = seconds // 60
        return f"{minutes}m {suffix}"
    elif seconds < 86400:
        hours = seconds // 3600
        minutes = (seconds % 3600) // 60
        if minutes > 0:
            return f"{hours}h {minutes}m {suffix}"
        return f"{hours}h {suffix}"
    else:
        days = seconds // 86400
        hours = (seconds % 86400) // 3600
        if hours > 0:
            return f"{days}d {hours}h {suffix}"
        return f"{days}d {suffix}"


def format_percentage(value: float, decimals: int = 1) -> str:
    """
    Format percentage value.

    Args:
        value: Percentage value (0-100)
        decimals: Number of decimal places

    Returns:
        Formatted percentage string (e.g., "45.5%")
    """
    return f"{value:.{decimals}f}%"


def format_cpu_allocation(allocated: int, total: int) -> str:
    """
    Format CPU allocation as "allocated/total (percentage%)".

    Args:
        allocated: Number of allocated CPUs
        total: Total number of CPUs

    Returns:
        Formatted string (e.g., "4/8 (50.0%)")
    """
    if total == 0:
        return f"{allocated}/{total} (0.0%)"

    percentage = (allocated / total) * 100
    return f"{allocated}/{total} ({percentage:.1f}%)"


def format_memory_allocation(allocated_mb: int, total_mb: int, in_gb: bool = True) -> str:
    """
    Format memory allocation as "allocated/total (percentage%)".

    Args:
        allocated_mb: Allocated memory in MB
        total_mb: Total memory in MB
        in_gb: Display in GB if True

    Returns:
        Formatted string (e.g., "2.5/8.0 GB (31.2%)")
    """
    if total_mb == 0:
        alloc_str = format_memory_mb(allocated_mb, in_gb)
        total_str = format_memory_mb(total_mb, in_gb)
        return f"{alloc_str}/{total_str} (0.0%)"

    percentage = (allocated_mb / total_mb) * 100
    alloc_str = format_memory_mb(allocated_mb, in_gb)
    total_str = format_memory_mb(total_mb, in_gb)

    # Extract just the numeric part for cleaner display
    if in_gb and total_mb >= 1024:
        alloc_num = allocated_mb / 1024
        total_num = total_mb / 1024
        return f"{alloc_num:.1f}/{total_num:.1f} GB ({percentage:.1f}%)"
    else:
        return f"{allocated_mb}/{total_mb} MB ({percentage:.1f}%)"


def format_job_state(state: str) -> str:
    """
    Format job state with consistent capitalization.

    Args:
        state: Raw job state string

    Returns:
        Formatted state string
    """
    state_map = {
        "PENDING": "Pending",
        "RUNNING": "Running",
        "COMPLETED": "Completed",
        "FAILED": "Failed",
        "CANCELLED": "Cancelled",
        "TIMEOUT": "Timeout",
        "NODE_FAIL": "Node Fail",
        "PREEMPTED": "Preempted",
        "SUSPENDED": "Suspended",
    }

    return state_map.get(state.upper(), state.capitalize())


def format_node_state(state: str) -> str:
    """
    Format node state with consistent capitalization.

    Args:
        state: Raw node state string

    Returns:
        Formatted state string
    """
    state_map = {
        "IDLE": "Idle",
        "ALLOCATED": "Allocated",
        "MIXED": "Mixed",
        "DOWN": "Down",
        "DRAIN": "Drain",
        "DRAINING": "Draining",
        "FAIL": "Fail",
        "FAILING": "Failing",
        "FUTURE": "Future",
        "MAINT": "Maintenance",
        "REBOOT": "Reboot",
        "RESERVED": "Reserved",
        "UNKNOWN": "Unknown",
    }

    # Handle compound states (e.g., "IDLE+DRAIN")
    if "+" in state:
        parts = state.split("+")
        formatted_parts = [state_map.get(p.upper(), p.capitalize()) for p in parts]
        return "+".join(formatted_parts)

    return state_map.get(state.upper(), state.capitalize())


def truncate_string(text: str, max_length: int, suffix: str = "...") -> str:
    """
    Truncate string to maximum length.

    Args:
        text: Input string
        max_length: Maximum length
        suffix: Suffix to add when truncated (default: "...")

    Returns:
        Truncated string
    """
    if len(text) <= max_length:
        return text

    return text[: max_length - len(suffix)] + suffix


def format_node_list(nodes: Optional[str], max_display: int = 3) -> str:
    """
    Format node list for display, truncating if too long.

    Args:
        nodes: Comma-separated node list (e.g., "c1,c2,c3")
        max_display: Maximum number of nodes to display

    Returns:
        Formatted node list (e.g., "c1, c2, +2 more")
    """
    if not nodes:
        return "N/A"

    node_list = [n.strip() for n in nodes.split(",")]

    if len(node_list) <= max_display:
        return ", ".join(node_list)

    displayed = node_list[:max_display]
    remaining = len(node_list) - max_display

    return f"{', '.join(displayed)}, +{remaining} more"


def create_progress_bar(
    current: float, total: float, width: int = 20, filled_char: str = "█", empty_char: str = "░"
) -> str:
    """
    Create a text-based progress bar.

    Args:
        current: Current value
        total: Total value
        width: Width of the progress bar in characters
        filled_char: Character for filled portion
        empty_char: Character for empty portion

    Returns:
        Progress bar string
    """
    if total == 0:
        percentage = 0.0
    else:
        percentage = min(1.0, current / total)

    filled_width = int(width * percentage)
    empty_width = width - filled_width

    return f"{filled_char * filled_width}{empty_char * empty_width}"


def colorize_state(state: str, state_type: str = "job") -> str:
    """
    Get color code for state (for use with Rich markup).

    Args:
        state: State string
        state_type: Type of state ("job" or "node")

    Returns:
        Color name for Rich markup
    """
    if state_type == "job":
        job_colors = {
            "RUNNING": "green",
            "PENDING": "yellow",
            "COMPLETED": "blue",
            "FAILED": "red",
            "CANCELLED": "red",
            "TIMEOUT": "red",
        }
        return job_colors.get(state.upper(), "white")

    elif state_type == "node":
        node_colors = {
            "IDLE": "green",
            "ALLOCATED": "cyan",
            "MIXED": "yellow",
            "DOWN": "red",
            "DRAIN": "magenta",
            "DRAINING": "magenta",
            "FAIL": "red",
        }
        return node_colors.get(state.upper().split("+")[0], "white")

    return "white"
