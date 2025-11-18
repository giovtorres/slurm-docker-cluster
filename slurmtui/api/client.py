"""
SLURM REST API client using httpx for async operations.
"""

import logging
from typing import List, Optional, Dict, Any
from datetime import datetime
import httpx

from .models import (
    NodeInfo,
    JobInfo,
    PartitionInfo,
    ClusterStats,
    DiagnosticsInfo,
    PingResponse,
)


logger = logging.getLogger(__name__)


class SlurmAPIError(Exception):
    """Base exception for SLURM API errors."""

    pass


class SlurmAPIClient:
    """Async HTTP client for SLURM REST API."""

    def __init__(
        self,
        base_url: str = "http://localhost:6820",
        api_version: str = "v0.0.42",
        auth_token: Optional[str] = None,
        timeout: float = 10.0,
    ):
        """
        Initialize SLURM API client.

        Args:
            base_url: Base URL for SLURM REST API (default: http://localhost:6820)
            api_version: API version (default: v0.0.42)
            auth_token: Optional authentication token
            timeout: Request timeout in seconds
        """
        self.base_url = base_url.rstrip("/")
        self.api_version = api_version
        self.timeout = timeout

        # Setup headers
        headers = {
            "Content-Type": "application/json",
            "Accept": "application/json",
        }
        if auth_token:
            headers["X-SLURM-USER-TOKEN"] = auth_token

        self.client = httpx.AsyncClient(
            base_url=self.base_url, headers=headers, timeout=timeout
        )

        # Cache for reducing API calls
        self._cache: Dict[str, tuple[Any, datetime]] = {}
        self._cache_duration = 5  # seconds

    def _build_url(self, endpoint: str) -> str:
        """Build full URL for API endpoint."""
        endpoint = endpoint.lstrip("/")
        return f"/slurm/{self.api_version}/{endpoint}"

    def _get_from_cache(self, key: str) -> Optional[Any]:
        """Get data from cache if not expired."""
        if key in self._cache:
            data, timestamp = self._cache[key]
            age = (datetime.now() - timestamp).total_seconds()
            if age < self._cache_duration:
                logger.debug(f"Cache hit for {key} (age: {age:.1f}s)")
                return data
        return None

    def _set_cache(self, key: str, data: Any) -> None:
        """Store data in cache with timestamp."""
        self._cache[key] = (data, datetime.now())

    async def _request(self, method: str, endpoint: str, **kwargs) -> Dict[str, Any]:
        """
        Make HTTP request to SLURM API.

        Args:
            method: HTTP method (GET, POST, etc.)
            endpoint: API endpoint path
            **kwargs: Additional arguments for httpx request

        Returns:
            JSON response as dictionary

        Raises:
            SlurmAPIError: If request fails
        """
        url = self._build_url(endpoint)
        try:
            logger.debug(f"{method} {url}")
            response = await self.client.request(method, url, **kwargs)
            response.raise_for_status()

            data = response.json()
            logger.debug(f"Response: {len(str(data))} bytes")
            return data

        except httpx.HTTPStatusError as e:
            error_msg = f"HTTP {e.response.status_code}: {e.response.text}"
            logger.error(f"API request failed: {error_msg}")
            raise SlurmAPIError(error_msg) from e

        except httpx.RequestError as e:
            error_msg = f"Request error: {str(e)}"
            logger.error(f"API request failed: {error_msg}")
            raise SlurmAPIError(error_msg) from e

        except Exception as e:
            error_msg = f"Unexpected error: {str(e)}"
            logger.error(f"API request failed: {error_msg}")
            raise SlurmAPIError(error_msg) from e

    async def ping(self) -> PingResponse:
        """
        Ping SLURM controller to check connectivity.

        Returns:
            PingResponse with controller status
        """
        cache_key = "ping"
        cached = self._get_from_cache(cache_key)
        if cached:
            return cached

        try:
            data = await self._request("GET", "ping")
            # Parse the response - structure varies by version
            ping_data = data.get("pings", [{}])[0] if "pings" in data else {}

            result = PingResponse(
                hostname=ping_data.get("hostname", "unknown"),
                ping=ping_data.get("ping", "UNKNOWN"),
                mode=ping_data.get("mode", "unknown"),
            )
            self._set_cache(cache_key, result)
            return result

        except Exception as e:
            logger.error(f"Ping failed: {e}")
            # Return default response on error
            return PingResponse(ping="DOWN")

    async def get_nodes(self) -> List[NodeInfo]:
        """
        Get list of all nodes in the cluster.

        Returns:
            List of NodeInfo objects
        """
        cache_key = "nodes"
        cached = self._get_from_cache(cache_key)
        if cached:
            return cached

        data = await self._request("GET", "nodes")
        nodes_data = data.get("nodes", [])

        nodes = []
        for node_data in nodes_data:
            try:
                # Extract relevant fields and handle variations in API response
                node = NodeInfo(
                    name=node_data.get("name", "unknown"),
                    state=node_data.get("state", "UNKNOWN"),
                    cpus=node_data.get("cpus", 0),
                    alloc_cpus=node_data.get("alloc_cpus", 0),
                    real_memory=node_data.get("real_memory", 0),
                    alloc_memory=node_data.get("alloc_memory", 0),
                    partitions=node_data.get("partitions", []),
                    reason=node_data.get("reason"),
                    architecture=node_data.get("architecture"),
                    operating_system=node_data.get("operating_system"),
                )
                nodes.append(node)
            except Exception as e:
                logger.warning(f"Failed to parse node data: {e}")
                continue

        self._set_cache(cache_key, nodes)
        return nodes

    async def get_node(self, node_name: str) -> Optional[NodeInfo]:
        """
        Get information about a specific node.

        Args:
            node_name: Name of the node

        Returns:
            NodeInfo object or None if not found
        """
        try:
            data = await self._request("GET", f"node/{node_name}")
            nodes_data = data.get("nodes", [])

            if nodes_data:
                node_data = nodes_data[0]
                return NodeInfo(
                    name=node_data.get("name", node_name),
                    state=node_data.get("state", "UNKNOWN"),
                    cpus=node_data.get("cpus", 0),
                    alloc_cpus=node_data.get("alloc_cpus", 0),
                    real_memory=node_data.get("real_memory", 0),
                    alloc_memory=node_data.get("alloc_memory", 0),
                    partitions=node_data.get("partitions", []),
                    reason=node_data.get("reason"),
                    architecture=node_data.get("architecture"),
                    operating_system=node_data.get("operating_system"),
                )
        except Exception as e:
            logger.error(f"Failed to get node {node_name}: {e}")

        return None

    async def get_jobs(self) -> List[JobInfo]:
        """
        Get list of all jobs in the cluster.

        Returns:
            List of JobInfo objects
        """
        cache_key = "jobs"
        cached = self._get_from_cache(cache_key)
        if cached:
            return cached

        data = await self._request("GET", "jobs")
        jobs_data = data.get("jobs", [])

        jobs = []
        for job_data in jobs_data:
            try:
                # Parse timestamps
                submit_time = self._parse_timestamp(job_data.get("submit_time"))
                start_time = self._parse_timestamp(job_data.get("start_time"))
                end_time = self._parse_timestamp(job_data.get("end_time"))
                eligible_time = self._parse_timestamp(job_data.get("eligible_time"))

                job = JobInfo(
                    job_id=job_data.get("job_id", 0),
                    user_name=job_data.get("user_name", "unknown"),
                    name=job_data.get("name", "unknown"),
                    job_state=job_data.get("job_state", "UNKNOWN"),
                    partition=job_data.get("partition", "unknown"),
                    nodes=job_data.get("nodes"),
                    node_count=job_data.get("node_count", 0),
                    cpus=job_data.get("cpus", 0),
                    tasks=job_data.get("tasks", 0),
                    time_limit=job_data.get("time_limit"),
                    submit_time=submit_time,
                    start_time=start_time,
                    end_time=end_time,
                    eligible_time=eligible_time,
                    current_working_directory=job_data.get("current_working_directory"),
                    standard_error=job_data.get("standard_error"),
                    standard_output=job_data.get("standard_output"),
                    priority=job_data.get("priority"),
                    command=job_data.get("command"),
                )
                jobs.append(job)
            except Exception as e:
                logger.warning(f"Failed to parse job data: {e}")
                continue

        self._set_cache(cache_key, jobs)
        return jobs

    async def get_job(self, job_id: int) -> Optional[JobInfo]:
        """
        Get information about a specific job.

        Args:
            job_id: Job ID

        Returns:
            JobInfo object or None if not found
        """
        try:
            data = await self._request("GET", f"job/{job_id}")
            jobs_data = data.get("jobs", [])

            if jobs_data:
                job_data = jobs_data[0]
                submit_time = self._parse_timestamp(job_data.get("submit_time"))
                start_time = self._parse_timestamp(job_data.get("start_time"))
                end_time = self._parse_timestamp(job_data.get("end_time"))

                return JobInfo(
                    job_id=job_data.get("job_id", job_id),
                    user_name=job_data.get("user_name", "unknown"),
                    name=job_data.get("name", "unknown"),
                    job_state=job_data.get("job_state", "UNKNOWN"),
                    partition=job_data.get("partition", "unknown"),
                    nodes=job_data.get("nodes"),
                    node_count=job_data.get("node_count", 0),
                    cpus=job_data.get("cpus", 0),
                    tasks=job_data.get("tasks", 0),
                    time_limit=job_data.get("time_limit"),
                    submit_time=submit_time,
                    start_time=start_time,
                    end_time=end_time,
                    current_working_directory=job_data.get("current_working_directory"),
                    standard_error=job_data.get("standard_error"),
                    standard_output=job_data.get("standard_output"),
                    priority=job_data.get("priority"),
                    command=job_data.get("command"),
                )
        except Exception as e:
            logger.error(f"Failed to get job {job_id}: {e}")

        return None

    async def get_partitions(self) -> List[PartitionInfo]:
        """
        Get list of all partitions in the cluster.

        Returns:
            List of PartitionInfo objects
        """
        cache_key = "partitions"
        cached = self._get_from_cache(cache_key)
        if cached:
            return cached

        data = await self._request("GET", "partitions")
        partitions_data = data.get("partitions", [])

        partitions = []
        for partition_data in partitions_data:
            try:
                partition = PartitionInfo(
                    name=partition_data.get("name", "unknown"),
                    state=partition_data.get("state", "UNKNOWN"),
                    total_nodes=partition_data.get("total_nodes", 0),
                    total_cpus=partition_data.get("total_cpus", 0),
                    nodes=partition_data.get("nodes"),
                    max_time=partition_data.get("max_time"),
                    default=partition_data.get("default", False),
                    max_nodes_per_job=partition_data.get("max_nodes_per_job"),
                )
                partitions.append(partition)
            except Exception as e:
                logger.warning(f"Failed to parse partition data: {e}")
                continue

        self._set_cache(cache_key, partitions)
        return partitions

    async def get_diagnostics(self) -> DiagnosticsInfo:
        """
        Get SLURM diagnostics information.

        Returns:
            DiagnosticsInfo object
        """
        try:
            data = await self._request("GET", "diag")
            stats = data.get("statistics", {})

            return DiagnosticsInfo(
                statistics=stats,
                server_thread_count=stats.get("server_thread_count"),
                agent_queue_size=stats.get("agent_queue_size"),
                agent_count=stats.get("agent_count"),
                jobs_submitted=stats.get("jobs_submitted"),
                jobs_started=stats.get("jobs_started"),
                jobs_completed=stats.get("jobs_completed"),
                jobs_canceled=stats.get("jobs_canceled"),
                jobs_failed=stats.get("jobs_failed"),
            )
        except Exception as e:
            logger.error(f"Failed to get diagnostics: {e}")
            return DiagnosticsInfo()

    async def get_cluster_stats(self) -> ClusterStats:
        """
        Get aggregated cluster statistics.

        Returns:
            ClusterStats object with computed statistics
        """
        # Fetch data in parallel
        nodes, jobs = await asyncio.gather(self.get_nodes(), self.get_jobs(), return_exceptions=True)

        # Handle errors
        if isinstance(nodes, Exception):
            logger.error(f"Failed to get nodes: {nodes}")
            nodes = []
        if isinstance(jobs, Exception):
            logger.error(f"Failed to get jobs: {jobs}")
            jobs = []

        # Calculate node statistics
        total_nodes = len(nodes)
        nodes_idle = sum(1 for n in nodes if "IDLE" in n.state.upper())
        nodes_allocated = sum(1 for n in nodes if "ALLOC" in n.state.upper() and "MIX" not in n.state.upper())
        nodes_mixed = sum(1 for n in nodes if "MIX" in n.state.upper())
        nodes_down = sum(1 for n in nodes if n.is_down)

        total_cpus = sum(n.cpus for n in nodes)
        cpus_allocated = sum(n.cpus_allocated for n in nodes)
        cpus_idle = total_cpus - cpus_allocated

        total_memory_mb = sum(n.real_memory for n in nodes)
        memory_allocated_mb = sum(n.alloc_memory for n in nodes)

        # Calculate job statistics
        total_jobs = len(jobs)
        jobs_pending = sum(1 for j in jobs if j.is_pending)
        jobs_running = sum(1 for j in jobs if j.is_running)
        jobs_completed = sum(1 for j in jobs if j.is_completed)
        jobs_failed = sum(1 for j in jobs if j.is_failed)

        return ClusterStats(
            cluster_name="slurm-cluster",
            total_nodes=total_nodes,
            nodes_idle=nodes_idle,
            nodes_allocated=nodes_allocated,
            nodes_mixed=nodes_mixed,
            nodes_down=nodes_down,
            total_jobs=total_jobs,
            jobs_pending=jobs_pending,
            jobs_running=jobs_running,
            jobs_completed=jobs_completed,
            jobs_failed=jobs_failed,
            total_cpus=total_cpus,
            cpus_allocated=cpus_allocated,
            cpus_idle=cpus_idle,
            total_memory_mb=total_memory_mb,
            memory_allocated_mb=memory_allocated_mb,
        )

    async def get_jobs_by_state(self, state: str) -> List[JobInfo]:
        """
        Get jobs filtered by state.

        Args:
            state: Job state to filter (PENDING, RUNNING, COMPLETED, etc.)

        Returns:
            List of JobInfo objects matching the state
        """
        all_jobs = await self.get_jobs()
        return [job for job in all_jobs if state.upper() in job.job_state.upper()]

    def _parse_timestamp(self, timestamp: Any) -> Optional[datetime]:
        """
        Parse timestamp from API response.

        Args:
            timestamp: Timestamp value (int, str, or dict)

        Returns:
            datetime object or None
        """
        if timestamp is None:
            return None

        try:
            # Handle Unix timestamp (int or number field)
            if isinstance(timestamp, (int, float)):
                if timestamp > 0:
                    return datetime.fromtimestamp(timestamp)

            # Handle timestamp object with 'number' field
            if isinstance(timestamp, dict):
                ts_value = timestamp.get("number", 0)
                if ts_value > 0:
                    return datetime.fromtimestamp(ts_value)

            # Handle ISO format string
            if isinstance(timestamp, str):
                return datetime.fromisoformat(timestamp.replace("Z", "+00:00"))

        except Exception as e:
            logger.debug(f"Failed to parse timestamp {timestamp}: {e}")

        return None

    async def close(self):
        """Close the HTTP client."""
        await self.client.aclose()

    async def __aenter__(self):
        """Async context manager entry."""
        return self

    async def __aexit__(self, exc_type, exc_val, exc_tb):
        """Async context manager exit."""
        await self.close()


# Import asyncio at module level for get_cluster_stats
import asyncio
