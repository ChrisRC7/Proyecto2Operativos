#!/usr/bin/env bash
# run_mpi_tailscale_4nodes.sh — Prueba MPI con 4 nodos físicos sobre Tailscale.
#
# Configuración:
#   Rank 0 (master):   máquina local (Raspberry Pi)
#   Rank 1 (worker 1): chroma-worker-1
#   Rank 2 (worker 2): chroma-worker-2
#   Rank 3 (worker 3): chroma-worker-3
#
# La imagen PPM se divide en 3 franjas horizontales (una por worker).
# Cada worker descifra, calcula histograma y aplica el filtro RGB a su franja.
# MPI_Reduce consolida los histogramas en el master.
#
# Uso:
#   ./run_mpi_tailscale_4nodes.sh [imagen.ppm]
#   Ejemplo: ./run_mpi_tailscale_4nodes.sh test_image.ppm

# set -e: abortar si cualquier comando falla.
set -e

# Limpiar variables de entorno gráfico para ejecución remota sin display.
unset DISPLAY
unset XAUTHORITY
unset WAYLAND_DISPLAY

# Archivo de imagen a procesar (argumento 1 o "test_image.ppm" por defecto).
IMAGE_FILE="${1:-test_image.ppm}"

# Array de hostnames de los 3 workers (deben ser accesibles por Tailscale SSH).
WORKERS=(
  "chroma-worker-1"   # primer worker: Rank 1 en MPI_COMM_WORLD
  "chroma-worker-2"   # segundo worker: Rank 2
  "chroma-worker-3"   # tercer worker: Rank 3
)

# Rutas de directorio temporal (iguales en todos los nodos para simplicidad).
LOCAL_TMP="/tmp/chromascope_test"
REMOTE_TMP="/tmp/chromascope_test"

EXECUTABLE="chromascope_mpi"               # binario MPI generado por make
HOSTFILE="${LOCAL_TMP}/hostfile"            # archivo de hosts para mpirun

echo "=========================================="
echo " ChromaScope MPI 4-node Tailscale test"
echo "=========================================="
echo "Image file: ${IMAGE_FILE}"
echo ""

# Verificar que la imagen de entrada existe antes de proceder.
if [ ! -f "${IMAGE_FILE}" ]; then
    echo "Error: image file not found: ${IMAGE_FILE}"
    exit 1
fi

# ── Paso 1/7: Compilar el proyecto en el nodo local ──────────────────────
echo "[1/7] Compiling project..."
make  # compila chromascope_mpi + tests (target 'all' del Makefile)

# Verificar que el ejecutable existe después de la compilación.
if [ ! -f "${EXECUTABLE}" ]; then
    echo "Error: executable not found after compilation: ${EXECUTABLE}"
    exit 1
fi

# ── Paso 2/7: Preparar el directorio temporal local ──────────────────────
echo "[2/7] Preparing local temp directory..."
rm -rf "${LOCAL_TMP}"                # limpiar sesión anterior
mkdir -p "${LOCAL_TMP}"              # crear directorio fresco
cp "${EXECUTABLE}" "${LOCAL_TMP}/"   # binario compilado
cp "${IMAGE_FILE}" "${LOCAL_TMP}/"   # imagen de prueba

# ── Paso 3/7: Verificar conexión SSH con cada worker ─────────────────────
echo "[3/7] Checking SSH connections..."
# Iterar sobre todos los workers y verificar que responden por SSH.
for worker in "${WORKERS[@]}"; do
    echo "Checking ${worker}..."
    # ssh -x: sin X11 forwarding
    # hostname: comando simple de verificación (devuelve el nombre del host)
    ssh -x "${worker}" "hostname"
done

# ── Paso 4/7: Preparar directorios remotos en cada worker ─────────────────
echo "[4/7] Preparing remote temp directories..."
for worker in "${WORKERS[@]}"; do
    # Limpiar y crear el directorio de trabajo en cada worker remoto.
    ssh "${worker}" "rm -rf ${REMOTE_TMP} && mkdir -p ${REMOTE_TMP}"
done

# ── Paso 5/7: Copiar el ejecutable a cada worker ──────────────────────────
echo "[5/7] Copying executable and image to workers..."
for worker in "${WORKERS[@]}"; do
    # scp copia el binario al directorio temporal del worker.
    # Cada worker necesita su propia copia del ejecutable para lanzarlo.
    scp "${EXECUTABLE}" "${worker}:${REMOTE_TMP}/"
done

# ── Paso 6/7: Crear el hostfile de MPI ───────────────────────────────────
echo "[6/7] Creating temporary hostfile..."
# El hostfile especifica la topología del clúster para mpirun:
# - localhost: el master (Rank 0) corre en esta máquina
# - chroma-worker-N: los workers (Rank 1,2,3) en las máquinas remotas
# "slots=1": un proceso MPI por máquina (total: 4 procesos = -np 4)
cat > "${HOSTFILE}" <<EOF
localhost slots=1
chroma-worker-1 slots=1
chroma-worker-2 slots=1
chroma-worker-3 slots=1
EOF

echo "Hostfile generated at: ${HOSTFILE}"
cat "${HOSTFILE}"  # mostrar para verificación
echo ""

# ── Paso 7/7: Lanzar el programa MPI en los 4 nodos ──────────────────────
echo "[7/7] Running MPI program..."
cd "${LOCAL_TMP}"  # cambiar al directorio con el ejecutable y la imagen

# env -u DISPLAY -u XAUTHORITY: limpiar variables de display en el entorno
# mpirun -np 4: exactamente 4 procesos (1 master + 3 workers)
# --hostfile: topología del clúster (definida arriba)
# --wdir: directorio de trabajo en cada nodo remoto
# --map-by slot: asignar procesos en el orden del hostfile (slot a slot)
# --mca plm_rsh_agent "ssh -x -o BatchMode=yes": SSH sin X11 y sin contraseña
# --mca routed direct: topología de comunicación MPI directa (sin árbol de procesos)
# --mca orte_base_help_aggregate 0: mostrar todos los mensajes de error (sin colapsar)
# --mca btl self,tcp: transportes de bajo nivel: memoria local y TCP/IP
# --mca btl_tcp_if_include tailscale0: forzar interfaz VPN para la comunicación TCP
# --mca oob_tcp_if_include tailscale0: canal de control OOB por Tailscale
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
