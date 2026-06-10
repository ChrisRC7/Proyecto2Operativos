#!/bin/bash
# Grafica resultados del benchmark de ChromaScope (genera PNG con matplotlib).
# Uso:
#   ./graficar.sh                 -> baja el CSV mas reciente de la rasp y grafica
#   ./graficar.sh bench_xxx.csv   -> grafica ese CSV local
set -u
RASP_USER=beto
RASP_IP=100.92.103.128
RASP_PASS=jason

if [ $# -ge 1 ] && [ -f "$1" ]; then
  CSV="$1"
else
  echo ">> Bajando CSV mas reciente de la rasp..."
  LATEST=$(sshpass -p "$RASP_PASS" ssh -o StrictHostKeyChecking=accept-new "$RASP_USER@$RASP_IP" 'ls -t ~/chromascope_persist/bench_*.csv 2>/dev/null | head -1')
  [ -z "$LATEST" ] && { echo "No hay CSV de benchmark en la rasp. Corre primero: ./run_cluster.sh --bench"; exit 1; }
  CSV=$(basename "$LATEST")
  sshpass -p "$RASP_PASS" scp -q -o StrictHostKeyChecking=accept-new "$RASP_USER@$RASP_IP:$LATEST" "./$CSV"
  echo ">> Descargado: $CSV"
fi

OUT="${CSV%.csv}.png"
python3 - "$CSV" "$OUT" <<'PY'
import csv, sys
import matplotlib; matplotlib.use('Agg')
import matplotlib.pyplot as plt
csv_path, out = sys.argv[1], sys.argv[2]
w=[]; t=[]
with open(csv_path) as f:
    for row in csv.DictReader(f):
        w.append(int(row['workers'])); t.append(float(row['tiempo_avg_s']))
if not w:
    print("CSV vacio"); sys.exit(1)
sp=[t[0]/x if x>0 else 0 for x in t]
fig,(ax1,ax2)=plt.subplots(1,2,figsize=(11,4.5))
ax1.plot(w,t,'o-',color='tab:blue')
ax1.set_xlabel('# nodos worker'); ax1.set_ylabel('Tiempo de computo (s)')
ax1.set_title('Tiempo vs nodos'); ax1.set_xticks(w); ax1.grid(True,alpha=.3)
ax2.plot(w,sp,'o-',color='tab:green',label='real')
ax2.plot(w,w,'--',color='gray',label='ideal (lineal)')
ax2.set_xlabel('# nodos worker'); ax2.set_ylabel('Speedup (T1/Tn)')
ax2.set_title('Speedup vs nodos'); ax2.set_xticks(w); ax2.legend(); ax2.grid(True,alpha=.3)
fig.suptitle('ChromaScope MPI — benchmark de desempeno')
plt.tight_layout(); plt.savefig(out,dpi=120)
print('Grafico guardado en:', out)
PY

command -v xdg-open >/dev/null && xdg-open "$OUT" >/dev/null 2>&1 &
echo ">> Listo: $OUT"
