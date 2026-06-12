#!/bin/bash
# sync_sources.sh — Sincroniza el código fuente desde la rasp a todos los workers.
#
# Debe ejecutarse DESDE la Raspberry Pi antes de run_cluster.sh cuando hay
# cambios en el código fuente que los workers necesitan recompilar.
#
# Usa rsync si está disponible en ambos extremos (más rápido, solo sincroniza
# diferencias). Si rsync no está disponible, usa tar+scp como fallback.
#
# Uso:
#   ./sync_sources.sh               → sincroniza a todos: chris randall yerik gabriel
#   ./sync_sources.sh "chris yerik" → solo a esos dos workers

# Directorio raíz del proyecto en la Raspberry Pi.
SRCDIR="/home/beto/Proyecto2Operativos"

# Todos los workers del clúster (identificados por su nombre corto).
ALL="chris randall yerik gabriel"

# Timeout SSH en segundos: si el worker no responde en 6s, se salta.
TIMEOUT=6

# Si se pasa un argumento, usarlo como lista de candidatos; si no, usar todos.
CAND="${1:-$ALL}"

echo ">> Sincronizando fuentes desde $SRCDIR ..."
echo ""

OK=0    # contador de sincronizaciones exitosas
FAIL=0  # contador de fallos

for s in $CAND; do
  w="chroma-$s"       # hostname completo: "chroma-chris", "chroma-randall", etc.
  printf "   - %-16s " "$s"  # imprimir el nombre alineado

  # Verificar que el worker responde por SSH antes de intentar la sincronización.
  # BatchMode=yes: no pedir contraseña (requiere key SSH configurada).
  if ! ssh -o BatchMode=yes -o ConnectTimeout=$TIMEOUT "$w" "exit 0" 2>/dev/null; then
    echo "no responde -> saltada"
    FAIL=$((FAIL+1))
    continue  # pasar al siguiente worker
  fi

  # Crear el directorio de destino en el worker si no existe.
  ssh -o BatchMode=yes -o ConnectTimeout=$TIMEOUT "$w" \
    "mkdir -p /home/beto/Proyecto2Operativos" 2>/dev/null

  # ── Intentar sincronización con rsync (preferido) ────────────────────
  # Verificar que rsync está disponible tanto en la rasp como en el worker.
  if command -v rsync &>/dev/null \
     && ssh -o BatchMode=yes -o ConnectTimeout=$TIMEOUT "$w" \
          "command -v rsync" &>/dev/null; then

    # rsync con las siguientes opciones:
    # -a: archive mode (preservar permisos, timestamps, symlinks)
    # -z: comprimir datos en tránsito (reduce uso de red)
    # --delete: eliminar en destino archivos que ya no existen en origen
    # --exclude='*.o': no sincronizar objetos compilados (se recompilan en el worker)
    # --exclude='chromascope_mpi': no sincronizar el binario ARM (incompatible en x86)
    # --exclude='*.bin': no sincronizar archivos cifrados de salida
    # --exclude='benchmark/': no sincronizar resultados del benchmark
    # -e "ssh ...": usar SSH con BatchMode para la transferencia
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
    # ── Fallback: tar + scp ────────────────────────────────────────────
    # Si rsync no está disponible, crear un tarball y enviarlo por scp.

    # Nombre único del tarball temporal usando el PID del proceso actual ($$).
    TARFILE="/tmp/chromascope_sync_$$.tar.gz"

    # Crear el tarball excluyendo los mismos archivos que rsync.
    # -C dirname: cambiar al directorio padre antes de empaquetar
    # basename: incluir solo el nombre del directorio raíz en el tar
    tar -czf "$TARFILE" \
      --exclude='*.o' \
      --exclude='chromascope_mpi' \
      --exclude='image_test' \
      --exclude='crypto_test' \
      --exclude='*.x86' \
      --exclude='*.bin' \
      --exclude='benchmark' \
      -C "$(dirname $SRCDIR)" "$(basename $SRCDIR)" 2>/dev/null

    # Copiar el tarball al worker y descomprimirlo en el destino.
    # scp -q: silencioso (sin barra de progreso)
    # tar -xzf: descomprimir; --overwrite: sobrescribir archivos existentes
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

    rm -f "$TARFILE"  # limpiar el tarball temporal de la rasp
  fi
done

echo ""
echo ">> Sincronizacion terminada: $OK OK, $FAIL fallidos."
