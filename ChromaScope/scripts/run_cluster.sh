#!/bin/bash
# ChromaScope cluster runner + benchmark.  Se ejecuta DESDE la rasp (master fijo).
#   ./run_cluster.sh                 -> corre normal con los workers encendidos
#   ./run_cluster.sh "chris yerik"   -> normal, solo esos (si responden)
#   ./run_cluster.sh --bench [reps]  -> benchmark 1..N workers + grafico de speedup (reps por config, def 3)
#
# El binario x86 canonico (~/chromascope_persist/chromascope_mpi.x86) se empuja a cada
# worker antes de correr, para garantizar que todos usen exactamente el mismo binario.
set -u
WDIR=/tmp/chromascope_test
PERSIST="$HOME/chromascope_persist"
ALL="chris randall yerik gabriel"
TIMEOUT=6

MPIRUN=/usr/local/bin/mpirun
mpi_flags() {
  printf '%s\0' --prefix /usr/local \
    --mca plm_rsh_agent "ssh -x -o BatchMode=yes" \
    --mca btl self,tcp \
    --mca btl_tcp_if_include tailscale0 \
    --mca btl_tcp_disable_family 6 \
    --mca oob_tcp_if_include tailscale0 \
    -x CHROMA_WORK \
    --wdir "$WDIR"
}

# --bench [reps] [work] [nodos] : reps repeticiones por config; work = factor de carga;
#                                 nodos = lista opcional (ej "chris yerik") para excluir relayed
BENCH=0; REPS=3; WORK=40; CAND="$ALL"
if [ "${1:-}" = "--bench" ]; then
  BENCH=1; REPS="${2:-3}"; WORK="${3:-40}"; [ -n "${4:-}" ] && CAND="$4"
elif [ $# -ge 1 ]; then CAND="$1"; fi

# --- master (rasp, binario ARM) ---
mkdir -p "$WDIR"
cp "$PERSIST/chromascope_mpi" "$PERSIST/test_image.ppm" "$WDIR/" \
  || { echo "ERROR: falta $PERSIST en la rasp"; exit 1; }

# --- detectar workers disponibles + sincronizar binario x86 + imagen ---
echo ">> Detectando workers y sincronizando binario..."
AVAIL=()
for s in $CAND; do
  w="chroma-$s"
  printf "   - %-16s " "$s"
  if ssh -o BatchMode=yes -o ConnectTimeout=$TIMEOUT "$w" "mkdir -p $WDIR && cp ~/chromascope_persist/test_image.ppm $WDIR/ 2>/dev/null" 2>/dev/null \
     && scp -o BatchMode=yes -o ConnectTimeout=$TIMEOUT -q "$PERSIST/chromascope_mpi.x86" "$w:$WDIR/chromascope_mpi" 2>/dev/null; then
    echo "DISPONIBLE"; AVAIL+=("$w")
  else
    echo "no responde -> descartada"
  fi
done
NW=${#AVAIL[@]}

# corre una vez: $1=hosts (incluye localhost), $2=np ; imprime stdout del programa
run_once() {
  local hosts="$1" np="$2" hf="$WDIR/hostfile"
  : > "$hf"; for h in $hosts; do echo "$h slots=1" >> "$hf"; done
  cd "$WDIR" || exit 1
  mapfile -d '' -t F < <(mpi_flags)
  env -u DISPLAY -u XAUTHORITY CHROMA_WORK="${CHROMA_WORK:-1}" "$MPIRUN" -np "$np" --hostfile "$hf" "${F[@]}" chromascope_mpi test_image.ppm 2>/dev/null
}

# ===================== MODO NORMAL =====================
if [ $BENCH -eq 0 ]; then
  hosts="localhost"; for w in "${AVAIL[@]}"; do hosts="$hosts $w"; done
  np=$((NW+1))
  if [ $NW -eq 0 ]; then echo ">> Sin workers; corriendo solo rasp (-np 1)."; fi
  echo ">> Corriendo -np $np ..."; echo ""
  run_once "$hosts" "$np"
  rm -f "$WDIR"/cluster_*.ppm "$WDIR"/cluster_*.bin
  exit 0
fi

# ===================== MODO BENCHMARK =====================
if [ $NW -eq 0 ]; then echo "ERROR: no hay workers disponibles para benchmark."; exit 1; fi
TS=$(date +%Y%m%d_%H%M%S)
CSV="$PERSIST/bench_$TS.csv"
echo "workers,procesos,tiempo_avg_s,tiempo_min_s" > "$CSV"
echo ""
echo ">> BENCHMARK: 1..$NW workers, $REPS repeticiones c/u, carga CHROMA_WORK=$WORK (compute-bound, sin escribir imagenes)"
export CHROMA_BENCH=1
export CHROMA_WORK=$WORK
declare -a AVG MIN
for ((k=1;k<=NW;k++)); do
  hosts="localhost"; for ((i=0;i<k;i++)); do hosts="$hosts ${AVAIL[$i]}"; done
  np=$((k+1)); sum=0; mn=999999
  for ((r=1;r<=REPS;r++)); do
    out=$(run_once "$hosts" "$np")
    t=$(printf '%s\n' "$out" | grep -oE 'tiempo=[0-9.]+' | head -1 | cut -d= -f2)
    [ -z "$t" ] && t=0
    sum=$(awk -v s="$sum" -v t="$t" 'BEGIN{print s+t}')
    mn=$(awk -v m="$mn" -v t="$t" 'BEGIN{print (t>0 && t<m)?t:m}')
    rm -f "$WDIR"/cluster_*.ppm "$WDIR"/cluster_*.bin
  done
  avg=$(awk -v s="$sum" -v r="$REPS" 'BEGIN{printf "%.4f", s/r}')
  AVG[$k]=$avg; MIN[$k]=$mn
  echo "$k,$np,$avg,$mn" >> "$CSV"
  printf "   %d worker(s): avg=%ss  min=%ss\n" "$k" "$avg" "$mn"
done
unset CHROMA_BENCH CHROMA_WORK

# --- grafico ASCII (tiempo) + speedup vs 1 worker ---
echo ""
echo "===== TIEMPO DE COMPUTO (MPI_Wtime, menor=mejor) ====="
maxt=0; for ((k=1;k<=NW;k++)); do maxt=$(awk -v m="$maxt" -v t="${AVG[$k]}" 'BEGIN{print (t>m)?t:m}'); done
for ((k=1;k<=NW;k++)); do
  bars=$(awk -v t="${AVG[$k]}" -v mt="$maxt" 'BEGIN{printf "%d", (mt>0)?(t/mt*40):0}')
  bar=$(printf '%*s' "$bars" '' | tr ' ' '#')
  sp=$(awk -v a="${AVG[1]}" -v b="${AVG[$k]}" 'BEGIN{printf "%.2f", (b>0)?a/b:0}')
  printf "%d nodo(s) |%-40s %ss  (speedup x%s)\n" "$k" "$bar" "${AVG[$k]}" "$sp"
done
echo ""
echo "CSV: $CSV"
echo "Graficar PNG en randall:  scp beto@100.92.103.128:$CSV .  &&  ./graficar.sh bench_$TS.csv"
