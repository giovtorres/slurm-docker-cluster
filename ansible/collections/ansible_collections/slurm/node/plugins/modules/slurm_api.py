#!/usr/bin/env python3
# -*- coding: utf-8 -*-

# Copyright: (c) 2024
# GNU General Public License v3.0+

DOCUMENTATION = r"""
---
module: slurm_api
short_description: Interact with Slurm via REST API (slurmrestd)
version_added: "1.1.0"
description:
    - Execute operations via Slurm REST API instead of CLI commands
    - Supports token-based JWT authentication
    - Enables async operations for long-running tasks
    - Better suited for automation and remote management
    - Requires slurmrestd to be running
options:
    endpoint:
        description:
            - API endpoint path (e.g., /slurm/v0.0.40/nodes)
            - Can be shorthand like 'nodes', 'jobs', 'partitions'
        required: true
        type: str
    method:
        description:
            - HTTP method for the request
        required: false
        type: str
        choices:
            - GET
            - POST
            - PUT
            - PATCH
            - DELETE
        default: GET
    data:
        description:
            - Request body for POST/PUT/PATCH operations
            - Should be a dict that will be converted to JSON
        required: false
        type: dict
    api_url:
        description:
            - Base URL for slurmrestd
            - Can also be set via SLURM_REST_URL environment variable
        required: false
        type: str
        default: http://localhost:6820
    api_version:
        description:
            - Slurm REST API version to use
        required: false
        type: str
        default: v0.0.40
    auth_token:
        description:
            - JWT authentication token
            - Can also be set via SLURM_JWT environment variable
        required: false
        type: str
        no_log: true
    auth_user:
        description:
            - Username for JWT token generation
            - Used with auth_key for automatic token generation
        required: false
        type: str
    auth_key:
        description:
            - Path to JWT key file for token generation
            - Used with auth_user for automatic token generation
        required: false
        type: str
        no_log: true
    timeout:
        description:
            - Request timeout in seconds
        required: false
        type: int
        default: 30
    async_operation:
        description:
            - Submit operation asynchronously
            - Returns immediately with operation ID
        required: false
        type: bool
        default: false
    poll_interval:
        description:
            - Seconds between polls for async operations
        required: false
        type: int
        default: 5
    poll_timeout:
        description:
            - Maximum seconds to wait for async operation
        required: false
        type: int
        default: 300
    validate_certs:
        description:
            - Validate SSL certificates for HTTPS
        required: false
        type: bool
        default: true
author:
    - Your Team
notes:
    - Requires slurmrestd running and accessible
    - JWT authentication requires proper Slurm configuration
    - API version must match your slurmrestd version
    - See Slurm REST API documentation for endpoint details
requirements:
    - requests (python library)
"""

EXAMPLES = r"""
- name: Get all nodes via REST API
  slurm.node.slurm_api:
    endpoint: nodes
    api_url: http://slurmctld:6820
    auth_token: "{{ slurm_jwt_token }}"
  register: nodes

- name: Get specific node info
  slurm.node.slurm_api:
    endpoint: nodes/compute01
    api_url: http://slurmctld:6820
  register: node_info

- name: Query running jobs
  slurm.node.slurm_api:
    endpoint: jobs
    api_url: http://slurmctld:6820
    auth_user: admin
    auth_key: /etc/slurm/jwt_hs256.key
  register: jobs

- name: Update node state via API
  slurm.node.slurm_api:
    endpoint: node/compute01
    method: POST
    data:
      node:
        state: drain
        reason: "Maintenance via Ansible"
    api_url: http://slurmctld:6820
    auth_token: "{{ slurm_jwt_token }}"

- name: Submit job via API
  slurm.node.slurm_api:
    endpoint: job/submit
    method: POST
    data:
      job:
        name: test_job
        partition: compute
        script: |
          #!/bin/bash
          echo "Hello from API"
          sleep 60
    api_url: http://slurmctld:6820
    auth_token: "{{ slurm_jwt_token }}"
  register: submitted_job

- name: Cancel job via API
  slurm.node.slurm_api:
    endpoint: "job/{{ job_id }}"
    method: DELETE
    api_url: http://slurmctld:6820

- name: Get partitions
  slurm.node.slurm_api:
    endpoint: partitions
    api_url: http://slurmctld:6820
  register: partitions

- name: Create reservation via API
  slurm.node.slurm_api:
    endpoint: reservation
    method: POST
    data:
      reservation:
        name: maint_api_test
        nodes: compute01
        start_time: now
        duration: 3600
        users: root
        flags: MAINT
    api_url: http://slurmctld:6820
    auth_token: "{{ slurm_jwt_token }}"

- name: Async job submission with polling
  slurm.node.slurm_api:
    endpoint: job/submit
    method: POST
    data:
      job:
        name: long_running_job
        partition: compute
        script: "#!/bin/bash\nsleep 3600"
    async_operation: true
    poll_timeout: 60
    api_url: http://slurmctld:6820
"""

RETURN = r"""
changed:
    description: Whether the API call modified state
    type: bool
    returned: always
status_code:
    description: HTTP status code from API
    type: int
    returned: always
response:
    description: Parsed JSON response from API
    type: dict
    returned: on success
headers:
    description: Response headers
    type: dict
    returned: always
url:
    description: Full URL that was called
    type: str
    returned: always
method:
    description: HTTP method used
    type: str
    returned: always
elapsed:
    description: Request duration in seconds
    type: float
    returned: always
async_id:
    description: Async operation ID if async_operation=true
    type: str
    returned: when async_operation=true
message:
    description: Human-readable result message
    type: str
    returned: always
"""

import json
import os
import time
import subprocess
from ansible.module_utils.basic import AnsibleModule

# Try to import requests, but handle missing dependency gracefully
try:
    import requests
    HAS_REQUESTS = True
except ImportError:
    HAS_REQUESTS = False


def generate_jwt_token(user: str, key_path: str) -> str | None:
    """Generate JWT token using scontrol."""
    try:
        # Use scontrol to generate token
        result = subprocess.run(
            ["scontrol", "token", f"username={user}"],
            capture_output=True,
            text=True,
            env={**os.environ, "SLURM_JWT": ""}
        )
        if result.returncode == 0:
            # Parse token from output
            for line in result.stdout.split("\n"):
                if "SLURM_JWT=" in line:
                    return line.split("=", 1)[1].strip()
        return None
    except Exception:
        return None


def build_url(base_url: str, api_version: str, endpoint: str) -> str:
    """Build full API URL from components."""
    # Normalize base URL
    base_url = base_url.rstrip("/")

    # Handle shorthand endpoints
    endpoint_map = {
        "nodes": f"/slurm/{api_version}/nodes",
        "jobs": f"/slurm/{api_version}/jobs",
        "partitions": f"/slurm/{api_version}/partitions",
        "reservations": f"/slurm/{api_version}/reservations",
        "accounts": f"/slurm/{api_version}/accounts",
        "diag": f"/slurm/{api_version}/diag",
        "ping": f"/slurm/{api_version}/ping",
    }

    # Check for shorthand
    if endpoint in endpoint_map:
        return f"{base_url}{endpoint_map[endpoint]}"

    # Handle partial paths
    if not endpoint.startswith("/"):
        # Check if it's a resource-specific shorthand (e.g., nodes/compute01)
        for short, full in endpoint_map.items():
            if endpoint.startswith(f"{short}/"):
                rest = endpoint[len(short):]
                return f"{base_url}{full}{rest}"
        # Default: prepend slurm API path
        return f"{base_url}/slurm/{api_version}/{endpoint}"

    return f"{base_url}{endpoint}"


def make_api_request(
    url: str,
    method: str,
    data: dict | None,
    token: str | None,
    timeout: int,
    validate_certs: bool
) -> dict:
    """Make HTTP request to Slurm REST API."""
    headers = {
        "Content-Type": "application/json",
        "Accept": "application/json",
    }

    if token:
        headers["X-SLURM-USER-TOKEN"] = token

    start_time = time.time()

    try:
        response = requests.request(
            method=method,
            url=url,
            headers=headers,
            json=data if data else None,
            timeout=timeout,
            verify=validate_certs
        )

        elapsed = time.time() - start_time

        result = {
            "status_code": response.status_code,
            "headers": dict(response.headers),
            "elapsed": round(elapsed, 3),
            "success": 200 <= response.status_code < 300,
        }

        # Parse JSON response
        try:
            result["response"] = response.json()
        except json.JSONDecodeError:
            result["response"] = {"raw": response.text}

        return result

    except requests.exceptions.Timeout:
        return {
            "status_code": 0,
            "success": False,
            "error": f"Request timeout after {timeout}s",
            "elapsed": timeout,
        }
    except requests.exceptions.ConnectionError as e:
        return {
            "status_code": 0,
            "success": False,
            "error": f"Connection error: {str(e)}",
            "elapsed": time.time() - start_time,
        }
    except Exception as e:
        return {
            "status_code": 0,
            "success": False,
            "error": str(e),
            "elapsed": time.time() - start_time,
        }


def poll_async_operation(
    base_url: str,
    api_version: str,
    operation_id: str,
    token: str,
    poll_interval: int,
    poll_timeout: int,
    validate_certs: bool
) -> dict:
    """Poll for async operation completion."""
    url = build_url(base_url, api_version, f"jobs/{operation_id}")
    start_time = time.time()

    while time.time() - start_time < poll_timeout:
        result = make_api_request(
            url=url,
            method="GET",
            data=None,
            token=token,
            timeout=30,
            validate_certs=validate_certs
        )

        if not result.get("success"):
            return result

        # Check job state
        response = result.get("response", {})
        jobs = response.get("jobs", [])
        if jobs:
            job_state = jobs[0].get("job_state", "UNKNOWN")
            if job_state in ["COMPLETED", "FAILED", "CANCELLED", "TIMEOUT"]:
                result["final_state"] = job_state
                result["async_completed"] = True
                return result

        time.sleep(poll_interval)

    return {
        "success": False,
        "async_completed": False,
        "error": f"Async operation timeout after {poll_timeout}s",
        "elapsed": poll_timeout,
    }


def main():
    module = AnsibleModule(
        argument_spec=dict(
            endpoint=dict(type="str", required=True),
            method=dict(type="str", default="GET",
                       choices=["GET", "POST", "PUT", "PATCH", "DELETE"]),
            data=dict(type="dict"),
            api_url=dict(type="str", default="http://localhost:6820"),
            api_version=dict(type="str", default="v0.0.40"),
            auth_token=dict(type="str", no_log=True),
            auth_user=dict(type="str"),
            auth_key=dict(type="str", no_log=True),
            timeout=dict(type="int", default=30),
            async_operation=dict(type="bool", default=False),
            poll_interval=dict(type="int", default=5),
            poll_timeout=dict(type="int", default=300),
            validate_certs=dict(type="bool", default=True),
        ),
        supports_check_mode=True,
    )

    if not HAS_REQUESTS:
        module.fail_json(
            msg="The 'requests' Python library is required. Install with: pip install requests"
        )

    endpoint = module.params["endpoint"]
    method = module.params["method"]
    data = module.params["data"]
    api_url = module.params["api_url"]
    api_version = module.params["api_version"]
    timeout = module.params["timeout"]
    async_operation = module.params["async_operation"]
    validate_certs = module.params["validate_certs"]

    # Handle authentication
    token = module.params["auth_token"]
    if not token:
        token = os.environ.get("SLURM_JWT")

    if not token and module.params["auth_user"] and module.params["auth_key"]:
        token = generate_jwt_token(
            module.params["auth_user"],
            module.params["auth_key"]
        )

    # Build URL
    url = build_url(api_url, api_version, endpoint)

    result = {
        "changed": False,
        "url": url,
        "method": method,
        "message": "",
    }

    # Check mode - don't make modifying requests
    if module.check_mode and method != "GET":
        result["message"] = f"Would {method} {url}"
        result["changed"] = True
        module.exit_json(**result)

    # Make the request
    api_result = make_api_request(
        url=url,
        method=method,
        data=data,
        token=token,
        timeout=timeout,
        validate_certs=validate_certs
    )

    result["status_code"] = api_result.get("status_code", 0)
    result["headers"] = api_result.get("headers", {})
    result["elapsed"] = api_result.get("elapsed", 0)
    result["response"] = api_result.get("response", {})

    if not api_result.get("success"):
        error_msg = api_result.get("error", "Unknown error")
        result["message"] = f"API request failed: {error_msg}"
        module.fail_json(msg=result["message"], **result)

    # Determine if state was changed (non-GET requests that succeeded)
    if method != "GET":
        result["changed"] = True

    # Handle async operations
    if async_operation and method in ["POST", "PUT", "PATCH"]:
        # Extract job/operation ID from response
        response = api_result.get("response", {})
        job_id = None

        # Try to find job ID in response
        if "job_id" in response:
            job_id = str(response["job_id"])
        elif "jobs" in response and response["jobs"]:
            job_id = str(response["jobs"][0].get("job_id", ""))

        if job_id:
            result["async_id"] = job_id

            # Poll for completion if requested
            poll_result = poll_async_operation(
                base_url=api_url,
                api_version=api_version,
                operation_id=job_id,
                token=token,
                poll_interval=module.params["poll_interval"],
                poll_timeout=module.params["poll_timeout"],
                validate_certs=validate_certs
            )

            if poll_result.get("async_completed"):
                result["response"] = poll_result.get("response", result["response"])
                result["final_state"] = poll_result.get("final_state")
                result["message"] = f"Async operation {job_id} completed"
            else:
                result["message"] = f"Async operation {job_id} submitted (not waiting for completion)"
    else:
        result["message"] = f"Successfully executed {method} {endpoint}"

    module.exit_json(**result)


if __name__ == "__main__":
    main()
