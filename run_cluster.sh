#!/bin/bash
# Corre ChromaScope MPI desde la rasp (master fijo) + los workers DISPONIBLES.
# Sin argumentos: prueba todos los workers conocidos y descarta los que no respondan.
# Con argumento: usa esa lista (igual descarta los que no respondan).
#   ./run_cluster.sh                  -> auto: usa los que esten encendidos
#   ./run_cluster.sh "chris yerik"    -> solo prueba chris y yerik

sudo insmod /home/beto/Proyecto2Operativos/ChromaScope/driver/chromascope_gpio.ko
sudo insmod /home/beto/Proyecto2Operativos/ChromaScope/driver/chromascope_lcd.ko

set -u
WDIR=/tmp/chromascope_test
ALL="chris randall yerik gabriel"     # workers conocidos
TIMEOUT=6                              # seg para detectar si un nodo responde

if [ $# -ge 1 ]; then CANDIDATES="$1"; else CANDIDATES="$ALL"; fi

echo ">> Preparando master (rasp)..."
mkdir -p "$WDIR"
cp ~/chromascope_persist/chromascope_mpi ~/chromascope_persist/test_image.ppm "$WDIR/" \
  || { echo "ERROR: falta ~/chromascope_persist en la rasp"; exit 1; }

echo ">> Detectando workers disponibles..."
AVAIL=""
for s in $CANDIDATES; do
  w="chroma-$s"
  printf "   - %-16s " "$s"
  if ssh -o BatchMode=yes -o ConnectTimeout=$TIMEOUT "$w" \
        "mkdir -p $WDIR && cp ~/chromascope_persist/chromascope_mpi ~/chromascope_persist/test_image.ppm $WDIR/" 2>/dev/null; then
    echo "DISPONIBLE"
    AVAIL="$AVAIL $w"
  else
    echo "no responde -> descartada"
  fi
done

# hostfile con master + workers disponibles
{ echo "localhost slots=1"; for w in $AVAIL; do echo "$w slots=1"; done; } > "$WDIR/hostfile"
NP=1; for w in $AVAIL; do NP=$((NP+1)); done

echo ""
if [ -z "$AVAIL" ]; then
  echo ">> Ningun worker disponible. Corriendo solo en la rasp (-np 1)."
else
  echo ">> Nodos:$AVAIL  + rasp(master)   ->  -np $NP"
fi
echo ">> hostfile:"; cat "$WDIR/hostfile"
echo ""

cd "$WDIR" || exit 1
env -u DISPLAY -u XAUTHORITY /usr/local/bin/mpirun -np "$NP" \
  --hostfile hostfile --prefix /usr/local \
  --mca plm_rsh_agent "ssh -x -o BatchMode=yes" \
  --mca btl self,tcp \
  --mca btl_tcp_if_include tailscale0 \
  --mca btl_tcp_disable_family 6 \
  --mca oob_tcp_if_include tailscale0 \
  --wdir "$WDIR" \
  chromascope_mpi test_image.ppm
