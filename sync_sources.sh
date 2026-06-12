#!/bin/bash
# sync_sources.sh  —  Copia el codigo fuente a todos los workers via rsync o tar+scp.
# Se ejecuta DESDE la rasp antes de run_cluster.sh si hubo cambios en el codigo.
#
#   ./sync_sources.sh               -> sincroniza a todos (chris randall yerik gabriel)
#   ./sync_sources.sh "chris yerik" -> solo esos

SRCDIR="/home/beto/Proyecto2Operativos"
ALL="chris randall yerik gabriel"
TIMEOUT=6
CAND="${1:-$ALL}"

echo ">> Sincronizando fuentes desde $SRCDIR ..."
echo ""

OK=0; FAIL=0
for s in $CAND; do
  w="chroma-$s"
  printf "   - %-16s " "$s"

  if ! ssh -o BatchMode=yes -o ConnectTimeout=$TIMEOUT "$w" "exit 0" 2>/dev/null; then
    echo "no responde -> saltada"
    FAIL=$((FAIL+1))
    continue
  fi

  ssh -o BatchMode=yes -o ConnectTimeout=$TIMEOUT "$w" \
    "mkdir -p /home/beto/Proyecto2Operativos" 2>/dev/null

  if command -v rsync &>/dev/null \
     && ssh -o BatchMode=yes -o ConnectTimeout=$TIMEOUT "$w" \
          "command -v rsync" &>/dev/null; then
    if rsync -az --delete \
         --exclude='*.o' \
         --exclude='chromascope_mpi' \
         --exclude='image_test' \
         --exclude='crypto_test' \
         --exclude='*.x86' \
         --exclude='*.bin' \
         --exclude='benchmark/' \
         -e "ssh -o BatchMode=yes -o ConnectTimeout=$TIMEOUT" \
         "$SRCDIR/" "$w:/home/beto/Proyecto2Operativos/" 2>/dev/null; then
      echo "OK (rsync)"
      OK=$((OK+1))
    else
      echo "ERROR en rsync"
      FAIL=$((FAIL+1))
    fi
  else
    TARFILE="/tmp/chromascope_sync_$$.tar.gz"
    tar -czf "$TARFILE" \
      --exclude='*.o' \
      --exclude='chromascope_mpi' \
      --exclude='image_test' \
      --exclude='crypto_test' \
      --exclude='*.x86' \
      --exclude='*.bin' \
      --exclude='benchmark' \
      -C "$(dirname $SRCDIR)" "$(basename $SRCDIR)" 2>/dev/null

    if scp -o BatchMode=yes -o ConnectTimeout=$TIMEOUT -q \
         "$TARFILE" "$w:/tmp/chromascope_sync.tar.gz" 2>/dev/null \
       && ssh -o BatchMode=yes -o ConnectTimeout=$TIMEOUT "$w" \
            "tar -xzf /tmp/chromascope_sync.tar.gz \
                 -C /home/beto/ \
                 --overwrite 2>/dev/null \
             && rm -f /tmp/chromascope_sync.tar.gz" 2>/dev/null; then
      echo "OK (tar+scp)"
      OK=$((OK+1))
    else
      echo "ERROR en tar+scp"
      FAIL=$((FAIL+1))
    fi
    rm -f "$TARFILE"
  fi
done

echo ""
echo ">> Sincronizacion terminada: $OK OK, $FAIL fallidos."