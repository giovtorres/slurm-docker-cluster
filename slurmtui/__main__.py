"""
Entry point for SLURM TUI Monitor.

Usage:
    python -m slurmtui
    slurmtui
"""

import argparse
import sys
import logging

from .config import load_config
from .app import run_app


def main():
    """Main entry point."""
    parser = argparse.ArgumentParser(
        description="SLURM TUI Monitor - Real-time cluster monitoring in your terminal",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Run with default configuration
  slurmtui

  # Run with custom config file
  slurmtui --config /path/to/config.yaml

  # Run with custom API URL
  SLURM_API_URL=http://slurm:6820 slurmtui

  # Run with debug logging
  SLURMTUI_DEBUG=1 slurmtui

Environment Variables:
  SLURM_API_URL        Override SLURM REST API URL
  SLURM_API_VERSION    Override API version (v0.0.41, v0.0.42)
  SLURMTUI_REFRESH     Override refresh interval in seconds
  SLURMTUI_CONFIG      Path to configuration file
  SLURMTUI_DEBUG       Enable debug mode (1, true, yes)
        """,
    )

    parser.add_argument(
        "--config",
        "-c",
        type=str,
        default=None,
        help="Path to configuration file (YAML)",
    )

    parser.add_argument(
        "--version",
        action="version",
        version="%(prog)s 0.1.0",
    )

    parser.add_argument(
        "--debug",
        action="store_true",
        help="Enable debug mode",
    )

    args = parser.parse_args()

    # Load configuration
    try:
        config = load_config(args.config)
    except Exception as e:
        print(f"Error loading configuration: {e}", file=sys.stderr)
        sys.exit(1)

    # Override debug if specified
    if args.debug:
        config.advanced.debug = True

    # Run the application
    try:
        run_app(config)
    except KeyboardInterrupt:
        print("\n\nApplication interrupted. Goodbye!")
        sys.exit(0)
    except Exception as e:
        logging.error(f"Fatal error: {e}", exc_info=True)
        print(f"\nFatal error: {e}", file=sys.stderr)
        print("Check logs for details or run with --debug for more information.")
        sys.exit(1)


if __name__ == "__main__":
    main()
