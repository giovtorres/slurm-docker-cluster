"""
Configuration management for SLURM TUI Monitor.

Supports loading from:
- YAML configuration file
- Environment variables (override config file)
- Default values
"""

import os
import logging
from pathlib import Path
from typing import Optional
import yaml
from pydantic import BaseModel, Field


logger = logging.getLogger(__name__)


class SlurmConfig(BaseModel):
    """SLURM API configuration."""

    api_url: str = Field(default="http://localhost:6820", description="SLURM REST API URL")
    api_version: str = Field(default="v0.0.42", description="SLURM REST API version")
    auth_token: Optional[str] = Field(default=None, description="Authentication token")
    timeout: float = Field(default=10.0, description="Request timeout in seconds")


class AppConfig(BaseModel):
    """Application configuration."""

    refresh_interval: int = Field(default=5, description="Auto-refresh interval in seconds")
    theme: str = Field(default="dark", description="Color theme (dark or light)")
    log_level: str = Field(default="info", description="Logging level")


class DisplayConfig(BaseModel):
    """Display configuration."""

    show_completed_jobs: bool = Field(
        default=False, description="Show completed jobs in job list"
    )
    max_jobs_display: int = Field(default=100, description="Maximum jobs to display")
    time_format: str = Field(
        default="%Y-%m-%d %H:%M:%S", description="Timestamp format string"
    )
    memory_in_gb: bool = Field(default=True, description="Display memory in GB instead of MB")
    compact: bool = Field(default=False, description="Use compact display mode")


class AdvancedConfig(BaseModel):
    """Advanced configuration."""

    debug: bool = Field(default=False, description="Enable debug mode")
    cache_duration: int = Field(default=5, description="Cache duration in seconds")


class Config(BaseModel):
    """Complete application configuration."""

    slurm: SlurmConfig = Field(default_factory=SlurmConfig)
    app: AppConfig = Field(default_factory=AppConfig)
    display: DisplayConfig = Field(default_factory=DisplayConfig)
    advanced: AdvancedConfig = Field(default_factory=AdvancedConfig)

    @classmethod
    def load(cls, config_path: Optional[str] = None) -> "Config":
        """
        Load configuration from file and environment variables.

        Args:
            config_path: Path to YAML configuration file (optional)

        Returns:
            Config object

        The loading priority (highest to lowest):
        1. Environment variables
        2. Configuration file
        3. Default values
        """
        # Start with defaults
        config_data = {}

        # Load from file if provided
        if config_path:
            config_path_obj = Path(config_path)
            if config_path_obj.exists():
                logger.info(f"Loading configuration from {config_path}")
                try:
                    with open(config_path) as f:
                        file_config = yaml.safe_load(f) or {}
                        config_data.update(file_config)
                except Exception as e:
                    logger.error(f"Failed to load config file {config_path}: {e}")
            else:
                logger.warning(f"Config file not found: {config_path}")

        # Check for default config locations if no path specified
        else:
            default_locations = [
                Path("config.yaml"),
                Path("slurmtui/config.yaml"),
                Path.home() / ".config" / "slurmtui" / "config.yaml",
                Path("/etc/slurmtui/config.yaml"),
            ]

            for location in default_locations:
                if location.exists():
                    logger.info(f"Loading configuration from {location}")
                    try:
                        with open(location) as f:
                            file_config = yaml.safe_load(f) or {}
                            config_data.update(file_config)
                            break
                    except Exception as e:
                        logger.error(f"Failed to load config file {location}: {e}")

        # Override with environment variables
        config_data = cls._apply_env_overrides(config_data)

        # Create and validate config object
        try:
            config = cls(**config_data)
            logger.info("Configuration loaded successfully")
            return config
        except Exception as e:
            logger.error(f"Failed to validate configuration: {e}")
            # Return default config on validation failure
            return cls()

    @staticmethod
    def _apply_env_overrides(config_data: dict) -> dict:
        """
        Apply environment variable overrides to configuration.

        Environment variables follow the pattern:
        SLURM_API_URL -> slurm.api_url
        SLURMTUI_REFRESH -> app.refresh_interval

        Args:
            config_data: Current configuration dictionary

        Returns:
            Updated configuration dictionary
        """
        # SLURM API overrides
        if api_url := os.getenv("SLURM_API_URL"):
            config_data.setdefault("slurm", {})["api_url"] = api_url
            logger.debug(f"Environment override: SLURM_API_URL={api_url}")

        if api_version := os.getenv("SLURM_API_VERSION"):
            config_data.setdefault("slurm", {})["api_version"] = api_version
            logger.debug(f"Environment override: SLURM_API_VERSION={api_version}")

        if auth_token := os.getenv("SLURM_AUTH_TOKEN"):
            config_data.setdefault("slurm", {})["auth_token"] = auth_token
            logger.debug("Environment override: SLURM_AUTH_TOKEN=***")

        if timeout := os.getenv("SLURM_TIMEOUT"):
            try:
                config_data.setdefault("slurm", {})["timeout"] = float(timeout)
                logger.debug(f"Environment override: SLURM_TIMEOUT={timeout}")
            except ValueError:
                logger.warning(f"Invalid SLURM_TIMEOUT value: {timeout}")

        # App overrides
        if refresh := os.getenv("SLURMTUI_REFRESH"):
            try:
                config_data.setdefault("app", {})["refresh_interval"] = int(refresh)
                logger.debug(f"Environment override: SLURMTUI_REFRESH={refresh}")
            except ValueError:
                logger.warning(f"Invalid SLURMTUI_REFRESH value: {refresh}")

        if theme := os.getenv("SLURMTUI_THEME"):
            config_data.setdefault("app", {})["theme"] = theme
            logger.debug(f"Environment override: SLURMTUI_THEME={theme}")

        if log_level := os.getenv("SLURMTUI_LOG_LEVEL"):
            config_data.setdefault("app", {})["log_level"] = log_level
            logger.debug(f"Environment override: SLURMTUI_LOG_LEVEL={log_level}")

        # Config path override
        if config_path := os.getenv("SLURMTUI_CONFIG"):
            logger.debug(f"Environment override: SLURMTUI_CONFIG={config_path}")
            # This is handled in the load() method

        # Debug mode
        if debug := os.getenv("SLURMTUI_DEBUG"):
            config_data.setdefault("advanced", {})["debug"] = debug.lower() in ("1", "true", "yes")
            logger.debug(f"Environment override: SLURMTUI_DEBUG={debug}")

        return config_data

    def setup_logging(self):
        """Configure logging based on config settings."""
        log_level = getattr(logging, self.app.log_level.upper(), logging.INFO)

        # Override with debug if enabled
        if self.advanced.debug:
            log_level = logging.DEBUG

        logging.basicConfig(
            level=log_level,
            format="%(asctime)s - %(name)s - %(levelname)s - %(message)s",
            datefmt="%Y-%m-%d %H:%M:%S",
        )

        logger.info(f"Logging level set to {logging.getLevelName(log_level)}")

    def to_dict(self) -> dict:
        """Convert configuration to dictionary."""
        return self.model_dump()

    def __str__(self) -> str:
        """String representation of configuration (safe for logging)."""
        config_dict = self.to_dict()

        # Redact sensitive information
        if config_dict.get("slurm", {}).get("auth_token"):
            config_dict["slurm"]["auth_token"] = "***REDACTED***"

        return yaml.dump(config_dict, default_flow_style=False)


def load_config(config_path: Optional[str] = None) -> Config:
    """
    Convenience function to load configuration.

    Args:
        config_path: Path to configuration file (optional)

    Returns:
        Config object
    """
    return Config.load(config_path)
