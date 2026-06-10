#!/usr/bin/env bash

set -e

unset DISPLAY
unset XAUTHORITY
unset WAYLAND_DISPLAY

IMAGE_FILE="${1:-test_image.ppm}"

WORKERS=(
  "chroma-worker-1"
  "chroma-worker-2"
  "chroma-worker-3"
)

LOCAL_TMP="/tmp/chromascope_test"
REMOTE_TMP="/tmp/chromascope_test"
EXECUTABLE="chromascope_mpi"
HOSTFILE="${LOCAL_TMP}/hostfile"

echo "=========================================="
echo " ChromaScope MPI 4-node Tailscale test"
echo "=========================================="
echo "Image file: ${IMAGE_FILE}"
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

echo "[3/7] Checking SSH connections..."
for worker in "${WORKERS[@]}"; do
    echo "Checking ${worker}..."
    ssh -x "${worker}" "hostname"
done

echo "[4/7] Preparing remote temp directories..."
for worker in "${WORKERS[@]}"; do
    ssh "${worker}" "rm -rf ${REMOTE_TMP} && mkdir -p ${REMOTE_TMP}"
done

echo "[5/7] Copying executable and image to workers..."
for worker in "${WORKERS[@]}"; do
    scp "${EXECUTABLE}" "${worker}:${REMOTE_TMP}/"
done

echo "[6/7] Creating temporary hostfile..."
cat > "${HOSTFILE}" <<EOF
localhost slots=1
chroma-worker-1 slots=1
chroma-worker-2 slots=1
chroma-worker-3 slots=1
EOF

echo "Hostfile generated at: ${HOSTFILE}"
cat "${HOSTFILE}"
echo ""

echo "[7/7] Running MPI program..."
cd "${LOCAL_TMP}"

env -u DISPLAY -u XAUTHORITY mpirun -np 4 \
  --hostfile "${HOSTFILE}" \
  --wdir "${REMOTE_TMP}" \
  --map-by slot \
  --mca plm_rsh_agent "ssh -x -o BatchMode=yes" \
  --mca routed direct \
  --mca orte_base_help_aggregate 0 \
  --mca btl self,tcp \
  --mca btl_tcp_if_include tailscale0 \
  --mca oob_tcp_if_include tailscale0 \
  "./${EXECUTABLE}" "${IMAGE_FILE}"

echo ""
echo "=========================================="
echo " MPI 4-node test finished"
echo "=========================================="