#!/usr/bin/env bash

set -e

unset DISPLAY
unset XAUTHORITY
unset WAYLAND_DISPLAY

# ==========================================
# ChromaScope - MPI test with 2 physical nodes
# Rank 0: local master
# Rank 1: remote worker through Tailscale
# ==========================================

WORKER_HOST="${1:-chroma-worker}"
IMAGE_FILE="${2:-test_image.ppm}"

LOCAL_TMP="/tmp/chromascope_test"
REMOTE_TMP="/tmp/chromascope_test"
EXECUTABLE="chromascope_mpi"
HOSTFILE="${LOCAL_TMP}/hostfile"

echo "=========================================="
echo " ChromaScope MPI 2-node Tailscale test"
echo "=========================================="
echo "Worker host: ${WORKER_HOST}"
echo "Image file:  ${IMAGE_FILE}"
echo ""

if [ ! -f "${IMAGE_FILE}" ]; then
    echo "Error: image file not found: ${IMAGE_FILE}"
    exit 1
fi

echo "[1/7] Compiling project..."
make

if [ ! -f "${EXECUTABLE}" ]; then
    echo "Error: executable not found after compilation: ${EXECUTABLE}"
    exit 1
fi

echo "[2/7] Preparing local temp directory..."
rm -rf "${LOCAL_TMP}"
mkdir -p "${LOCAL_TMP}"
cp "${EXECUTABLE}" "${LOCAL_TMP}/"
cp "${IMAGE_FILE}" "${LOCAL_TMP}/"

echo "[3/7] Checking SSH connection with worker..."
ssh -x "${WORKER_HOST}" "hostname"

echo "[4/7] Preparing remote temp directory..."
ssh "${WORKER_HOST}" "rm -rf ${REMOTE_TMP} && mkdir -p ${REMOTE_TMP}"

echo "[5/7] Copying executable and image to worker..."
scp "${EXECUTABLE}" "${WORKER_HOST}:${REMOTE_TMP}/"

echo "[6/7] Creating temporary hostfile..."
cat > "${HOSTFILE}" <<EOF
localhost slots=1
${WORKER_HOST} slots=1
EOF

echo "Hostfile generated at: ${HOSTFILE}"
cat "${HOSTFILE}"
echo ""

echo "[7/7] Running MPI program..."
cd "${LOCAL_TMP}"

env -u DISPLAY -u XAUTHORITY mpirun -np 2 \
  --hostfile "${HOSTFILE}" \
  --wdir "${REMOTE_TMP}" \
  --mca plm_rsh_agent "ssh -x -o BatchMode=yes" \
  --mca btl self,tcp \
  --mca btl_tcp_if_include tailscale0 \
  --mca oob_tcp_if_include tailscale0 \
  "./${EXECUTABLE}" "${IMAGE_FILE}"

echo ""
echo "=========================================="
echo " MPI 2-node test finished"
echo "=========================================="