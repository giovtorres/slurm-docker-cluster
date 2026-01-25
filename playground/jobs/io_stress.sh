#!/bin/bash
#SBATCH --job-name=io_stress
#SBATCH --ntasks=1
#SBATCH --time=${TIME:-10}
#SBATCH --output=/data/io_stress_%j.out
#SBATCH --error=/data/io_stress_%j.err

# I/O Stress Test Job
# Performs read/write operations to test I/O performance
#
# Environment variables:
#   FILESIZE  - Size of test file in MB (default: 100)
#   DURATION  - Duration in seconds (default: 60)
#   PATTERN   - I/O pattern: sequential, random (default: sequential)

FILESIZE=${FILESIZE:-100}
DURATION=${DURATION:-60}
PATTERN=${PATTERN:-sequential}

TESTFILE="/tmp/io_test_${SLURM_JOB_ID}"

echo "=========================================="
echo "I/O Stress Test"
echo "=========================================="
echo "Hostname:   $(hostname)"
echo "Job ID:     $SLURM_JOB_ID"
echo "File size:  ${FILESIZE}MB"
echo "Duration:   ${DURATION}s"
echo "Pattern:    $PATTERN"
echo "Test file:  $TESTFILE"
echo "Start time: $(date)"
echo "=========================================="

# Cleanup function
cleanup() {
    rm -f "$TESTFILE" "${TESTFILE}."*
    echo "Cleanup completed"
}
trap cleanup EXIT

end=$((SECONDS + DURATION))
iteration=0
total_written=0
total_read=0

while [ $SECONDS -lt $end ]; do
    iteration=$((iteration + 1))
    echo "--- Iteration $iteration ---"

    # Write test
    echo "Writing ${FILESIZE}MB..."
    write_start=$(date +%s.%N)
    dd if=/dev/zero of="$TESTFILE" bs=1M count=$FILESIZE conv=fdatasync 2>&1 | grep -E "copied|bytes"
    write_end=$(date +%s.%N)
    write_time=$(echo "$write_end - $write_start" | bc)
    write_speed=$(echo "scale=2; $FILESIZE / $write_time" | bc)
    echo "  Write: ${write_speed} MB/s"
    total_written=$((total_written + FILESIZE))

    # Sync to ensure data is on disk
    sync

    # Read test
    echo "Reading ${FILESIZE}MB..."
    # Clear page cache if possible (requires privileges)
    echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true

    read_start=$(date +%s.%N)
    dd if="$TESTFILE" of=/dev/null bs=1M 2>&1 | grep -E "copied|bytes"
    read_end=$(date +%s.%N)
    read_time=$(echo "$read_end - $read_start" | bc)
    read_speed=$(echo "scale=2; $FILESIZE / $read_time" | bc)
    echo "  Read: ${read_speed} MB/s"
    total_read=$((total_read + FILESIZE))

    if [ "$PATTERN" = "random" ]; then
        # Random I/O pattern - small random reads/writes
        echo "Random I/O operations..."
        for i in $(seq 1 100); do
            offset=$((RANDOM % (FILESIZE * 1024 * 1024 - 4096)))
            dd if="$TESTFILE" of=/dev/null bs=4096 count=1 skip=$((offset / 4096)) 2>/dev/null
        done
    fi

    # Brief pause between iterations
    sleep 1
done

echo "=========================================="
echo "Summary"
echo "=========================================="
echo "Iterations:    $iteration"
echo "Total written: ${total_written}MB"
echo "Total read:    ${total_read}MB"
echo "End time:      $(date)"
echo "I/O stress test completed successfully"
echo "=========================================="
