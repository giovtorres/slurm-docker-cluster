"""
Pydantic data models for SLURM REST API responses.
"""

from datetime import datetime
from typing import List, Optional
from pydantic import BaseModel, Field, field_validator


class NodeInfo(BaseModel):
    """Information about a SLURM compute node."""

    name: str
    state: str
    cpus: int = 0
    cpus_allocated: int = Field(default=0, alias="alloc_cpus")
    real_memory: int = Field(default=0, alias="real_memory")  # MB
    alloc_memory: int = 0  # MB
    partitions: List[str] = Field(default_factory=list)
    reason: Optional[str] = None
    architecture: Optional[str] = None
    operating_system: Optional[str] = None

    class Config:
        populate_by_name = True

    @property
    def cpus_idle(self) -> int:
        """Calculate idle CPUs."""
        return max(0, self.cpus - self.cpus_allocated)

    @property
    def cpu_utilization(self) -> float:
        """Calculate CPU utilization percentage."""
        if self.cpus == 0:
            return 0.0
        return (self.cpus_allocated / self.cpus) * 100

    @property
    def memory_utilization(self) -> float:
        """Calculate memory utilization percentage."""
        if self.real_memory == 0:
            return 0.0
        return (self.alloc_memory / self.real_memory) * 100

    @property
    def is_up(self) -> bool:
        """Check if node is in an UP state."""
        return "IDLE" in self.state.upper() or "ALLOC" in self.state.upper() or "MIX" in self.state.upper()

    @property
    def is_down(self) -> bool:
        """Check if node is down."""
        return "DOWN" in self.state.upper() or "DRAIN" in self.state.upper() or "FAIL" in self.state.upper()


class JobInfo(BaseModel):
    """Information about a SLURM job."""

    job_id: int
    user_name: str = Field(alias="user_name")
    name: str
    job_state: str
    partition: str
    nodes: Optional[str] = None
    node_count: int = Field(default=0, alias="node_count")
    cpus: int = Field(default=0, alias="cpus")
    tasks: int = 0
    time_limit: Optional[int] = None  # minutes
    submit_time: Optional[datetime] = None
    start_time: Optional[datetime] = None
    end_time: Optional[datetime] = None
    eligible_time: Optional[datetime] = None
    work_dir: Optional[str] = Field(default=None, alias="current_working_directory")
    standard_error: Optional[str] = None
    standard_output: Optional[str] = None
    priority: Optional[int] = None
    command: Optional[str] = None

    class Config:
        populate_by_name = True

    @property
    def state(self) -> str:
        """Normalized job state."""
        return self.job_state

    @property
    def is_running(self) -> bool:
        """Check if job is currently running."""
        return "RUNNING" in self.job_state.upper()

    @property
    def is_pending(self) -> bool:
        """Check if job is pending."""
        return "PENDING" in self.job_state.upper()

    @property
    def is_completed(self) -> bool:
        """Check if job is completed."""
        return "COMPLETED" in self.job_state.upper()

    @property
    def is_failed(self) -> bool:
        """Check if job failed."""
        return "FAILED" in self.job_state.upper() or "CANCELLED" in self.job_state.upper() or "TIMEOUT" in self.job_state.upper()

    @property
    def runtime_seconds(self) -> Optional[int]:
        """Calculate job runtime in seconds."""
        if self.start_time and self.is_running:
            return int((datetime.now() - self.start_time).total_seconds())
        elif self.start_time and self.end_time:
            return int((self.end_time - self.start_time).total_seconds())
        return None

    @property
    def wait_time_seconds(self) -> Optional[int]:
        """Calculate wait time in seconds for pending jobs."""
        if self.submit_time and self.is_pending:
            return int((datetime.now() - self.submit_time).total_seconds())
        elif self.submit_time and self.start_time:
            return int((self.start_time - self.submit_time).total_seconds())
        return None


class PartitionInfo(BaseModel):
    """Information about a SLURM partition."""

    name: str
    state: str
    total_nodes: int = 0
    total_cpus: int = 0
    nodes: Optional[str] = None
    max_time: Optional[int] = None  # minutes
    default: bool = False
    max_nodes: Optional[int] = Field(default=None, alias="max_nodes_per_job")

    class Config:
        populate_by_name = True

    @property
    def is_up(self) -> bool:
        """Check if partition is up."""
        return "UP" in self.state.upper()


class ClusterStats(BaseModel):
    """Aggregated cluster statistics."""

    cluster_name: str = "unknown"
    total_nodes: int = 0
    nodes_idle: int = 0
    nodes_allocated: int = 0
    nodes_mixed: int = 0
    nodes_down: int = 0
    total_jobs: int = 0
    jobs_pending: int = 0
    jobs_running: int = 0
    jobs_completed: int = 0
    jobs_failed: int = 0
    total_cpus: int = 0
    cpus_allocated: int = 0
    cpus_idle: int = 0
    total_memory_mb: int = 0
    memory_allocated_mb: int = 0

    @property
    def cpu_utilization(self) -> float:
        """Calculate overall CPU utilization percentage."""
        if self.total_cpus == 0:
            return 0.0
        return (self.cpus_allocated / self.total_cpus) * 100

    @property
    def memory_utilization(self) -> float:
        """Calculate overall memory utilization percentage."""
        if self.total_memory_mb == 0:
            return 0.0
        return (self.memory_allocated_mb / self.total_memory_mb) * 100

    @property
    def nodes_up(self) -> int:
        """Calculate number of nodes that are up."""
        return self.nodes_idle + self.nodes_allocated + self.nodes_mixed

    @property
    def node_availability(self) -> float:
        """Calculate node availability percentage."""
        if self.total_nodes == 0:
            return 0.0
        return (self.nodes_up / self.total_nodes) * 100


class DiagnosticsInfo(BaseModel):
    """SLURM diagnostics information."""

    statistics: dict = Field(default_factory=dict)
    server_thread_count: Optional[int] = None
    agent_queue_size: Optional[int] = None
    agent_count: Optional[int] = None
    jobs_submitted: Optional[int] = None
    jobs_started: Optional[int] = None
    jobs_completed: Optional[int] = None
    jobs_canceled: Optional[int] = None
    jobs_failed: Optional[int] = None

    class Config:
        populate_by_name = True


class PingResponse(BaseModel):
    """Response from SLURM ping endpoint."""

    hostname: str = "unknown"
    ping: str = "UP"
    mode: str = "primary"

    @property
    def is_up(self) -> bool:
        """Check if SLURM controller is up."""
        return self.ping.upper() == "UP"
