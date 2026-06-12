#!/bin/bash
# run_cluster.sh — Orquestador principal del clúster ChromaScope.
#
# Se ejecuta DESDE la Raspberry Pi (nodo master fijo, Rank 0).
# Compila el binario en la rasp, lo compila también en cada worker disponible,
# construye el hostfile de MPI y lanza el programa distribuido.
#
# Modos de uso:
#   ./run_cluster.sh                  → compila y corre con todos los workers online
#   ./run_cluster.sh "chris yerik"    → solo esos dos workers
#   ./run_cluster.sh --bench [reps]   → benchmark 1..N workers + tabla de speedup
#
# Prerequisito: código fuente actualizado en workers → usar sync_sources.sh

# ── Cargar drivers de kernel (GPIO y LCD) antes de correr MPI ────────────
# insmod: carga el módulo .ko en el kernel en ejecución.
# 2>/dev/null || true: si ya está cargado, insmod falla → ignorar el error
# silenciosamente para que el script no se detenga.
sudo insmod /home/beto/Proyecto2Operativos/ChromaScope/driver/chromascope_gpio.ko 2>/dev/null || true
sudo insmod /home/beto/Proyecto2Operativos/ChromaScope/driver/chromascope_lcd.ko 2>/dev/null || true

# set -u: tratar variables no definidas como error → detectar typos en nombres.
set -u

# ── Variables de configuración del clúster ───────────────────────────────
SRCDIR="/home/beto/Proyecto2Operativos/ChromaScope"       # directorio fuente en master
REMOTE_SRCDIR="/home/beto/Proyecto2Operativos/ChromaScope" # mismo path en workers
BENCH_DIR="/home/beto/Proyecto2Operativos/benchmark"       # donde se guardan los CSV
BINARY_NAME="chromascope_mpi"                              # nombre del ejecutable MPI
IMAGE="test_image.ppm"                                     # imagen de prueba
ALL="chris randall yerik gabriel"                          # todos los workers conocidos
TIMEOUT=6                                                  # timeout SSH en segundos

# Ruta completa a mpirun para evitar ambigüedad si hay varias versiones.
MPIRUN=/usr/local/bin/mpirun

# ── Función: construir el array de flags de mpirun ───────────────────────
# printf '%s\0': separar cada flag con un byte nulo (safe con espacios en paths)
# Se usa luego con: mapfile -d '' -t F < <(mpi_flags)
# SSH sin X11 forwarding
# transporte: memoria compartida + TCP
# usar solo la interfaz VPN Tailscale
# deshabilitar IPv6 (solo IPv4 en Tailscale)
# control MPI también por Tailscale
# exportar la variable de carga al entorno
# directorio de trabajo en cada proceso
mpi_flags() {
  printf '%s\0' --prefix /usr/local \
    --mca plm_rsh_agent "ssh -x -o BatchMode=yes" \
    --mca btl self,tcp \
    --mca btl_tcp_if_include tailscale0 \
    --mca btl_tcp_disable_family 6 \
    --mca oob_tcp_if_include tailscale0 \
    -x CHROMA_WORK \
    --wdir "$SRCDIR"                                 
}

# ── Parseo de argumentos de línea de comandos ────────────────────────────
BENCH=0; REPS=3; WORK=40; CAND="$ALL"  # valores por defecto

if [ "${1:-}" = "--bench" ]; then
  # Modo benchmark: activar BENCH=1, tomar parámetros opcionales
  BENCH=1
  REPS="${2:-3}"    # número de repeticiones por configuración (defecto 3)
  WORK="${3:-40}"   # carga de trabajo CHROMA_WORK (defecto 40)
  [ -n "${4:-}" ] && CAND="$4"  # candidatos opcionales
elif [ $# -ge 1 ]; then
  # Modo normal con lista específica de workers
  CAND="$1"
fi

# ── COMPILACIÓN ──────────────────────────────────────────────────────────

echo ">> Compilando binario ARM en la rasp..."
# Compilar en la Raspberry Pi (arquitectura ARM64).
# make -C "$SRCDIR" mpi: solo el target 'mpi', no tests ni crypto.
# sed 's/^/   /': indentar la salida para mejor legibilidad.
if make -C "$SRCDIR" mpi 2>&1 | sed 's/^/   /'; then
  echo "   ARM OK: $SRCDIR/$BINARY_NAME"
else
  echo "ERROR: fallo compilacion ARM. Abortando."
  exit 1
fi

echo ""
echo ">> Compilando x86 y preparando workers..."

# Array de workers disponibles (los que responden SSH y compilan sin error).
AVAIL=()

for s in $CAND; do
  w="chroma-$s"       # hostname: "chroma-chris", "chroma-randall", etc.
  printf "   - %-16s " "$s"  # imprimir nombre del worker alineado

  # Verificar conectividad SSH con timeout corto.
  # BatchMode=yes: no pedir contraseña interactivamente (falla si no hay key).
  # ConnectTimeout=$TIMEOUT: no esperar más de 6 segundos.
  if ! ssh -o BatchMode=yes -o ConnectTimeout=$TIMEOUT "$w" "exit 0" 2>/dev/null; then
    echo "no responde -> descartada"
    continue  # saltar al siguiente worker si no hay conexión
  fi

  # Compilar en el worker remoto (arquitectura x86_64 en laptops del equipo).
  # ssh -o ConnectTimeout=60: timeout mayor para la compilación.
  compile_out=$(ssh -o BatchMode=yes -o ConnectTimeout=60 "$w" \
    "make -C $REMOTE_SRCDIR clean 2>&1 && make -C $REMOTE_SRCDIR mpi 2>&1" 2>/dev/null)

  # Verificar que la compilación en el worker fue exitosa ($? es el exit code).
  if [ $? -ne 0 ]; then
    echo "ERROR compilacion x86:"
    printf '%s\n' "$compile_out" | sed 's/^/        /'  # mostrar error indentado
    echo "        -> descartada"
    continue
  fi

  # Verificar que el binario y la imagen existen en el worker antes de agregarlo.
  if ! ssh -o BatchMode=yes -o ConnectTimeout=$TIMEOUT "$w" \
       "test -f $REMOTE_SRCDIR/$BINARY_NAME && test -f $REMOTE_SRCDIR/$IMAGE" 2>/dev/null; then
    echo "ERROR: binario o imagen no encontrados en $REMOTE_SRCDIR -> descartada"
    continue
  fi

  echo "OK"
  AVAIL+=("$w")  # agregar al array de workers disponibles
done

NW=${#AVAIL[@]}  # número de workers disponibles (#: longitud del array)
echo ""

# ── Función: ejecutar una corrida MPI con una lista de hosts ─────────────
# $1 = cadena de hosts separados por espacios (ej: "localhost chroma-chris")
# $2 = número total de procesos MPI (np)
run_once() {
  local hosts="$1" np="$2" hf="$SRCDIR/hostfile"

  # Crear el hostfile: una línea por host con "slots=1" (un proceso por máquina).
  : > "$hf"  # truncar el archivo existente (: es un no-op)
  for h in $hosts; do
    echo "$h slots=1" >> "$hf"
  done

  cd "$SRCDIR" || exit 1

  # Leer los flags de mpirun en un array (separados por nulo).
  mapfile -d '' -t F < <(mpi_flags)

  # Lanzar MPI con:
  # env -u DISPLAY -u XAUTHORITY: limpiar variables gráficas (no hay display)
  # CHROMA_WORK=...: exportar la variable de carga al ambiente de cada proceso
  env -u DISPLAY -u XAUTHORITY \
    CHROMA_WORK="${CHROMA_WORK:-1}" \
    "$MPIRUN" -np "$np" --hostfile "$hf" "${F[@]}" \
    ./$BINARY_NAME $IMAGE 2>/dev/null
}

# ── MODO NORMAL (sin --bench) ─────────────────────────────────────────────
if [ $BENCH -eq 0 ]; then
  # Construir la lista de hosts: master local + todos los workers disponibles.
  hosts="localhost"
  for w in "${AVAIL[@]}"; do hosts="$hosts $w"; done

  np=$((NW+1))  # np = número de workers + 1 master

  if [ $NW -eq 0 ]; then
    echo ">> Sin workers disponibles; corriendo solo en rasp (-np 1)."
  fi

  echo ">> Corriendo -np $np ..."
  echo ""
  run_once "$hosts" "$np"

  # Limpiar archivos temporales generados por el procesamiento.
  rm -f "$SRCDIR"/cluster_*.ppm "$SRCDIR"/cluster_*.bin "$SRCDIR/hostfile"
  exit 0
fi

# ── MODO BENCHMARK ────────────────────────────────────────────────────────
if [ $NW -eq 0 ]; then
  echo "ERROR: no hay workers disponibles para benchmark."
  exit 1
fi

# Crear el directorio de resultados si no existe.
mkdir -p "$BENCH_DIR"

# Timestamp para el nombre único del archivo CSV.
TS=$(date +%Y%m%d_%H%M%S)
CSV="$BENCH_DIR/bench_$TS.csv"

# Escribir la cabecera del CSV de resultados.
echo "workers,procesos,tiempo_avg_s,tiempo_min_s" > "$CSV"

echo ""
echo ">> BENCHMARK: 1..$NW workers, $REPS repeticiones c/u, carga CHROMA_WORK=$WORK"

# Exportar variables de entorno para el modo benchmark.
# CHROMA_BENCH=1: activa el modo sin I/O de archivos (más rápido y limpio).
# CHROMA_WORK=$WORK: parámetro de carga de trabajo para el algoritmo.
export CHROMA_BENCH=1
export CHROMA_WORK=$WORK

declare -a AVG MIN  # arrays para guardar promedios y mínimos por configuración

# Iterar sobre el número de workers: de 1 hasta NW (máximo disponible).
for ((k=1; k<=NW; k++)); do

  # Construir la lista de hosts para esta iteración: master + k workers.
  hosts="localhost"
  for ((i=0; i<k; i++)); do hosts="$hosts ${AVAIL[$i]}"; done
  np=$((k+1))   # procesos totales = k workers + 1 master
  sum=0         # acumulador de tiempos
  mn=999999     # mínimo (iniciar con un valor grande)

  # Ejecutar REPS repeticiones con la misma configuración de workers.
  for ((r=1; r<=REPS; r++)); do
    out=$(run_once "$hosts" "$np")

    # Extraer el tiempo MPI del output con grep y expresión regular.
    # El master imprime "tiempo=X.XXXX" que este script parsea.
    t=$(printf '%s\n' "$out" | grep -oE 'tiempo=[0-9.]+' | head -1 | cut -d= -f2)

    [ -z "$t" ] && t=0  # si no se encontró el tiempo, usar 0

    # Sumar al acumulador usando awk (soporta aritmética flotante en shell).
    sum=$(awk -v s="$sum" -v t="$t" 'BEGIN{print s+t}')

    # Actualizar el mínimo: si t>0 y t<mn, usar t.
    mn=$(awk  -v m="$mn"  -v t="$t" 'BEGIN{print (t>0 && t<m)?t:m}')

    # Limpiar archivos temporales entre repeticiones.
    rm -f "$SRCDIR"/cluster_*.ppm "$SRCDIR"/cluster_*.bin
  done

  # Calcular el promedio: suma / número de repeticiones.
  avg=$(awk -v s="$sum" -v r="$REPS" 'BEGIN{printf "%.4f", s/r}')

  AVG[$k]=$avg   # guardar promedio para el gráfico final
  MIN[$k]=$mn    # guardar mínimo

  # Escribir la fila en el CSV: k workers, np procesos, tiempo promedio, mínimo.
  echo "$k,$np,$avg,$mn" >> "$CSV"
  printf "   %d worker(s): avg=%ss  min=%ss\n" "$k" "$avg" "$mn"
done

# Limpiar variables de entorno del modo benchmark.
unset CHROMA_BENCH CHROMA_WORK
rm -f "$SRCDIR/hostfile"

# ── Tabla de speedup ──────────────────────────────────────────────────────
echo ""
echo "===== TIEMPO DE COMPUTO (MPI_Wtime, menor=mejor) ====="

# Encontrar el tiempo máximo (1 solo worker) para normalizar las barras.
maxt=0
for ((k=1; k<=NW; k++)); do
  maxt=$(awk -v m="$maxt" -v t="${AVG[$k]}" 'BEGIN{print (t>m)?t:m}')
done

# Imprimir una barra proporcional y el speedup para cada configuración.
for ((k=1; k<=NW; k++)); do

  # Calcular el número de '#' proporcional al tiempo (barra de 40 caracteres).
  bars=$(awk -v t="${AVG[$k]}" -v mt="$maxt" \
    'BEGIN{printf "%d", (mt>0)?(t/mt*40):0}')

  # Generar la barra de caracteres '#' con printf y tr.
  bar=$(printf '%*s' "$bars" '' | tr ' ' '#')

  # Calcular el speedup respecto a 1 worker: speedup = T(1 worker) / T(k workers)
  sp=$(awk -v a="${AVG[1]}" -v b="${AVG[$k]}" \
    'BEGIN{printf "%.2f", (b>0)?a/b:0}')

  # Imprimir la línea de la tabla con barras y speedup.
  printf "%d nodo(s) |%-40s %ss  (speedup x%s)\n" \
    "$k" "$bar" "${AVG[$k]}" "$sp"
done

echo ""
echo "CSV guardado en: $CSV"
# Instrucción para generar el gráfico PNG en otro nodo del equipo.
echo "Graficar PNG en randall:  scp beto@100.92.103.128:$CSV .  &&  ./graficar.sh bench_$TS.csv"
