# SLURM TUI Monitor - Architecture Design

## Overview
A modern, real-time terminal-based monitoring dashboard for SLURM clusters built with Textual.

## Technology Stack
- **Textual** (>=0.47.0) - TUI framework
- **httpx** - Async HTTP client for REST API
- **pydantic** (>=2.0) - Data validation and models
- **rich** - Enhanced terminal formatting
- **PyYAML** - Configuration file support

## Project Structure
```
slurmtui/
├── __init__.py
├── __main__.py              # Entry point: python -m slurmtui
├── app.py                   # Main Textual application
├── config.py                # Configuration management
├── api/
│   ├── __init__.py
│   ├── client.py            # SLURM REST API client
│   └── models.py            # Pydantic data models
├── widgets/
│   ├── __init__.py
│   ├── dashboard.py         # Main dashboard view
│   ├── nodes.py             # Node status panel
│   ├── jobs.py              # Job queue table
│   ├── job_detail.py        # Job detail modal
│   └── charts.py            # Historical charts/sparklines
├── utils/
│   ├── __init__.py
│   └── formatters.py        # Data formatting utilities
└── config.yaml.example      # Example configuration
```

## Data Models

### Core Models (api/models.py)
Based on SLURM REST API v0.0.42 responses:

```python
class NodeInfo(BaseModel):
    name: str
    state: str  # idle, allocated, down, etc.
    cpus: int
    cpus_allocated: int
    real_memory: int  # MB
    alloc_memory: int  # MB
    partitions: List[str]

class JobInfo(BaseModel):
    job_id: int
    user: str
    name: str
    state: str  # pending, running, completed, failed
    partition: str
    nodes: Optional[str]
    cpus: int
    memory_mb: int
    submit_time: datetime
    start_time: Optional[datetime]
    end_time: Optional[datetime]
    time_limit: Optional[int]  # minutes

class PartitionInfo(BaseModel):
    name: str
    state: str
    nodes: List[str]
    total_cpus: int
    total_nodes: int

class ClusterStats(BaseModel):
    total_nodes: int
    nodes_idle: int
    nodes_allocated: int
    nodes_down: int
    total_jobs: int
    jobs_pending: int
    jobs_running: int
    jobs_completed: int
    jobs_failed: int
    total_cpus: int
    cpus_allocated: int
```

## API Client Architecture

### SlurmAPIClient (api/client.py)
Async HTTP client for REST API interactions:

```python
class SlurmAPIClient:
    def __init__(self, base_url: str, api_version: str = "v0.0.42"):
        self.base_url = base_url
        self.api_version = api_version
        self.client = httpx.AsyncClient(timeout=10.0)

    # Core API methods
    async def get_nodes() -> List[NodeInfo]
    async def get_jobs() -> List[JobInfo]
    async def get_partitions() -> List[PartitionInfo]
    async def get_job(job_id: int) -> JobInfo
    async def get_diagnostics() -> dict
    async def ping() -> bool

    # Aggregated data methods
    async def get_cluster_stats() -> ClusterStats
    async def get_jobs_by_state(state: str) -> List[JobInfo]
```

## UI Architecture

### Main Application (app.py)
```python
class SlurmTUI(App):
    CSS_PATH = "styles.tcss"
    BINDINGS = [
        ("d", "switch_view('dashboard')", "Dashboard"),
        ("j", "switch_view('jobs')", "Jobs"),
        ("n", "switch_view('nodes')", "Nodes"),
        ("h", "switch_view('history')", "History"),
        ("r", "refresh", "Refresh"),
        ("q", "quit", "Quit"),
    ]

    # Auto-refresh timer
    auto_refresh_interval: int = 5  # seconds
```

### Dashboard Widget (widgets/dashboard.py)
Main overview screen showing:
- Cluster name and status
- Summary cards (total nodes, jobs, CPU usage)
- Resource utilization bars
- Quick stats grid

Layout:
```
┌─ SLURM Cluster: linux ──────────────────────────┐
│ ⚡ Status: Healthy                  🔄 Auto-refresh: 5s │
├──────────────────────────────────────────────────┤
│ ┌─ Nodes ─┐  ┌─ Jobs ──┐  ┌─ CPUs ──┐          │
│ │ 2/2 UP  │  │ 3 Total │  │ 2/4     │          │
│ │ 100%    │  │ 1 Run   │  │ 50%     │          │
│ └─────────┘  └─────────┘  └─────────┘          │
│                                                   │
│ Node Status:                                     │
│ ▓▓▓▓▓▓▓▓▓▓░░░░░░░░░░ 50% Allocated             │
│                                                   │
│ Recent Activity:                                 │
│ • Job 123 completed (user1)                     │
│ • Job 124 started on c1,c2 (user2)             │
└──────────────────────────────────────────────────┘
```

### Node Status Panel (widgets/nodes.py)
Real-time node monitoring table:

Columns:
- Node Name
- State (with color coding)
- CPUs (allocated/total)
- Memory (allocated/total)
- Partitions

Features:
- Sortable columns
- Color-coded states (green=idle, yellow=allocated, red=down)
- Selection to view node details

### Job Queue Widget (widgets/jobs.py)
Interactive job list with DataTable:

Columns:
- Job ID
- User
- Name
- State
- Partition
- Nodes
- CPUs
- Time Running/Waiting
- Submit Time

Features:
- Filter by state (pending/running/all)
- Sort by any column
- Select job to view details
- Color-coded states

### Job Detail Modal (widgets/job_detail.py)
Modal overlay showing detailed job information:
- Full job metadata
- Resource usage (CPU, memory, time)
- Job script/command
- Standard output/error (if available)
- Accounting information

### Historical Charts (widgets/charts.py)
Sparklines and simple visualizations:
- Job completion trend (last hour/day)
- CPU utilization over time
- Job wait time trend
- User activity distribution

## Configuration System (config.py)

### Config File (config.yaml)
```yaml
slurm:
  api_url: "http://localhost:6820"
  api_version: "v0.0.42"
  # Optional: auth token if required
  # auth_token: "xxx"

app:
  refresh_interval: 5  # seconds
  theme: "dark"  # dark or light

display:
  show_completed_jobs: false
  max_jobs_display: 100
  time_format: "%Y-%m-%d %H:%M:%S"
```

### Environment Variable Overrides
- `SLURM_API_URL` - Override API URL
- `SLURM_API_VERSION` - Override API version
- `SLURMTUI_REFRESH` - Override refresh interval

## Key Features

### 1. Real-time Updates
- Auto-refresh timer (configurable interval)
- Async data fetching (non-blocking UI)
- Visual indicators for data age
- Manual refresh with 'r' key

### 2. Keyboard Navigation
- Vim-style bindings (j/k for navigation)
- Tab switching (d/j/n/h for views)
- Enter to select/view details
- ESC to close modals
- q to quit

### 3. Responsive Layout
- Adapts to terminal size
- Collapsible panels
- Scrollable content areas
- Smart column sizing

### 4. Error Handling
- Graceful API failures (show cached data)
- Connection status indicator
- User-friendly error messages
- Automatic reconnection attempts

### 5. Performance
- Efficient data caching
- Incremental updates
- Minimal API calls
- Async operations

## Implementation Phases

### Phase 1: Core Foundation
1. Project structure and dependencies
2. API client with basic endpoints
3. Data models
4. Configuration system

### Phase 2: Basic UI
1. Main app skeleton
2. Dashboard widget (simple version)
3. Node status table
4. Job queue table
5. View switching

### Phase 3: Enhanced Features
1. Job detail modal
2. Auto-refresh mechanism
3. Filtering and sorting
4. Color-coded states
5. Keyboard shortcuts

### Phase 4: Advanced Features
1. Historical charts
2. Advanced filtering
3. Search functionality
4. Export capabilities
5. Theming support

### Phase 5: Polish & Testing
1. Error handling
2. Performance optimization
3. Documentation
4. Docker integration
5. Testing with real cluster

## Testing Strategy

### Unit Tests
- API client with mocked responses
- Data model validation
- Utility functions

### Integration Tests
- Full app with test API
- View switching
- Data refresh cycles

### Manual Testing
- Real SLURM cluster
- Various screen sizes
- Error scenarios
- Performance under load

## Docker Integration

### Running Inside Container
```bash
# Install in slurmctld or separate monitoring container
docker exec -it slurmctld bash
cd /opt/slurmtui
python -m slurmtui
```

### Standalone Monitoring Container
```dockerfile
FROM python:3.11-slim
WORKDIR /app
COPY slurmtui/ /app/slurmtui/
RUN pip install -e .
CMD ["python", "-m", "slurmtui"]
```

## Future Enhancements
- Database integration for historical data
- Alert notifications
- User/account statistics
- Custom dashboard layouts
- Export to CSV/JSON
- Multi-cluster support
- Web-based version (Textual can export to web)
