#!/usr/bin/env bash
# run_mpi_tailscale_2nodes.sh — Prueba MPI con 2 nodos físicos sobre Tailscale.
#
# Configuración:
#   Rank 0 (master): máquina local (la Raspberry Pi u otro nodo)
#   Rank 1 (worker): máquina remota accesible por Tailscale VPN
#
# La VPN Tailscale crea una interfaz "tailscale0" con IPs tipo 100.x.x.x.
# Se obliga a MPI a usar solo esa interfaz para garantizar la comunicación
# en entornos donde los nodos no están en la misma red local (LAN/WiFi).
#
# Uso:
#   ./run_mpi_tailscale_2nodes.sh [worker_host] [imagen.ppm]
#   Ejemplo: ./run_mpi_tailscale_2nodes.sh chroma-worker test_image.ppm

# set -e: abortar inmediatamente si cualquier comando falla (exit code ≠ 0).
set -e

# Limpiar variables de entorno gráfico que podrían causar errores en ejecución
# remota SSH (MPI lanza procesos en el worker via SSH).
# unset DISPLAY: sin servidor X11 en el nodo remoto
# unset XAUTHORITY: sin credenciales de X11
# unset WAYLAND_DISPLAY: sin servidor Wayland
unset DISPLAY
unset XAUTHORITY
unset WAYLAND_DISPLAY

# ==========================================
# ChromaScope - MPI test with 2 physical nodes
# Rank 0: local master
# Rank 1: remote worker through Tailscale
# ==========================================

# Leer argumentos con valores por defecto si no se proporcionan.
# ${1:-chroma-worker}: si $1 no está definido, usar "chroma-worker"
WORKER_HOST="${1:-chroma-worker}"
IMAGE_FILE="${2:-test_image.ppm}"

# Directorios temporales de trabajo (mismo path en ambas máquinas para simplificar).
LOCAL_TMP="/tmp/chromascope_test"
REMOTE_TMP="/tmp/chromascope_test"

EXECUTABLE="chromascope_mpi"               # nombre del binario MPI a ejecutar
HOSTFILE="${LOCAL_TMP}/hostfile"            # archivo de hosts para mpirun

echo "=========================================="
echo " ChromaScope MPI 2-node Tailscale test"
echo "=========================================="
echo "Worker host: ${WORKER_HOST}"
echo "Image file:  ${IMAGE_FILE}"
echo ""

# Verificar que el archivo de imagen existe antes de continuar.
if [ ! -f "${IMAGE_FILE}" ]; then
    echo "Error: image file not found: ${IMAGE_FILE}"
    exit 1
fi

# ── Paso 1/7: Compilar el proyecto ───────────────────────────────────────
echo "[1/7] Compiling project..."
# make sin target compila el 'all' por defecto (mpi + image_test + crypto_test).
make

# Verificar que el ejecutable fue generado correctamente.
if [ ! -f "${EXECUTABLE}" ]; then
    echo "Error: executable not found after compilation: ${EXECUTABLE}"
    exit 1
fi

# ── Paso 2/7: Preparar el directorio temporal local ──────────────────────
echo "[2/7] Preparing local temp directory..."
rm -rf "${LOCAL_TMP}"          # limpiar directorio anterior si existe
mkdir -p "${LOCAL_TMP}"        # crear directorio fresco
cp "${EXECUTABLE}" "${LOCAL_TMP}/"  # copiar el binario compilado
cp "${IMAGE_FILE}" "${LOCAL_TMP}/"  # copiar la imagen de prueba

# ── Paso 3/7: Verificar la conexión SSH con el worker ────────────────────
echo "[3/7] Checking SSH connection with worker..."
# ssh -x: deshabilitar X11 forwarding (no necesario en MPI)
# hostname: comando simple para verificar que SSH funciona
ssh -x "${WORKER_HOST}" "hostname"

# ── Paso 4/7: Preparar el directorio temporal en el worker ───────────────
echo "[4/7] Preparing remote temp directory..."
# Limpiar y crear el directorio de trabajo en el nodo remoto.
ssh "${WORKER_HOST}" "rm -rf ${REMOTE_TMP} && mkdir -p ${REMOTE_TMP}"

# ── Paso 5/7: Copiar el ejecutable al worker ──────────────────────────────
echo "[5/7] Copying executable and image to worker..."
# scp: secure copy; copiar el binario compilado para el worker (x86 o ARM).
# NOTA: en un clúster heterogéneo, el binario debe compilarse en el worker
#       con la misma arquitectura. Aquí se asume arquitecturas compatibles.
scp "${EXECUTABLE}" "${WORKER_HOST}:${REMOTE_TMP}/"

# ── Paso 6/7: Crear el hostfile de MPI ───────────────────────────────────
echo "[6/7] Creating temporary hostfile..."
# El hostfile le dice a mpirun cuántos procesos lanzar en cada máquina.
# "slots=1": exactamente 1 proceso MPI por máquina (master en local, worker en remoto).
# Heredoc (cat > file <<EOF): escribir múltiples líneas en el archivo directamente.
cat > "${HOSTFILE}" <<EOF
localhost slots=1
${WORKER_HOST} slots=1
EOF

echo "Hostfile generated at: ${HOSTFILE}"
cat "${HOSTFILE}"  # mostrar el contenido para verificación
echo ""

# ── Paso 7/7: Lanzar el programa MPI ──────────────────────────────────────
echo "[7/7] Running MPI program..."
cd "${LOCAL_TMP}"  # cambiar al directorio de trabajo donde están los archivos

# env -u DISPLAY -u XAUTHORITY: limpiar variables gráficas en el entorno de mpirun
# mpirun -np 2: lanzar exactamente 2 procesos (1 master + 1 worker)
# --hostfile: especificar dónde corre cada proceso
# --wdir: directorio de trabajo en cada nodo (debe existir en ambos)
# --mca plm_rsh_agent "ssh -x -o BatchMode=yes": usar SSH sin X11 y sin passwords
# --mca btl self,tcp: transportes disponibles: memoria compartida y TCP/IP
# --mca btl_tcp_if_include tailscale0: forzar uso de la interfaz VPN Tailscale
# --mca oob_tcp_if_include tailscale0: mismo para el canal de control MPI (OOB)
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
