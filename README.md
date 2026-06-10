# ChromaScope — Clúster MPI (guía de comandos)

Procesamiento distribuido de imágenes con **OpenMPI + Tailscale**: el master reparte
franjas de una imagen, los workers descifran (ChaCha20), calculan histograma HSV y
aplican un filtro RGB, y el master reconstruye y valida el resultado.

---

## 1. Nodos del clúster

| Nodo | Rol | IP Tailscale | Usuario | Arquitectura |
|------|-----|--------------|---------|--------------|
| **raspberrypi** | Master (Rank 0) | `100.92.103.128` | `beto` | ARM aarch64 |
| **chris** | Worker | `100.120.142.9` | `beto` | x86_64 |
| **randall** | Worker | `100.104.253.57` | `randall` | x86_64 |
| **yerik** | Worker | `100.100.81.76` | `beto` | x86_64 |
| **gabriel** | Worker | `100.101.13.93` | `beto` | x86_64 |

> El **master es siempre la raspberry**. Los workers se detectan automáticamente y se
> descartan los que estén apagados.

Todos los nodos usan **OpenMPI 5.0.7 + OpenPMIx 5.0.7 + hwloc 2.12.0** instalados en `/usr/local`.

---

## 2. Correr el clúster (modo normal)

Desde **tu terminal**:

```bash
ssh beto@100.92.103.128          # contraseña: jason
./run_cluster.sh                 # usa todos los workers encendidos
```

Variantes (dentro de la rasp):

```bash
./run_cluster.sh "chris yerik"   # solo esos workers
./run_cluster.sh chris           # solo rasp + chris
```

El modo normal **escribe** los resultados en `/tmp/chromascope_test/`:
- `cluster_filtered_rgb.ppm` — imagen con filtro RGB
- `cluster_decrypted.ppm` — imagen descifrada (verificación)
- `cluster_encrypted_pixels.bin` — píxeles cifrados

```bash
ls -lh /tmp/chromascope_test/cluster_*      # ver resultados
rm -f /tmp/chromascope_test/cluster_*       # limpiar (la rasp tiene poco disco)
```

---

## 3. Benchmark de desempeño (speedup)

```bash
# ./run_cluster.sh --bench [repeticiones] [carga] [nodos]
./run_cluster.sh --bench 3 100 "chris yerik"
```

- **repeticiones**: corridas por configuración (promedia). Default `3`.
- **carga**: factor `CHROMA_WORK` que repite el cómputo del worker → vuelve el workload
  *compute-bound* para ver speedup real. Default `40`.
- **nodos**: lista opcional para elegir solo nodos con **link directo** (excluir relayed).

Corre con 1, 2, …, N workers, mide el **tiempo de cómputo** (`MPI_Wtime`, sin overhead de
lanzamiento ni escritura de imágenes), imprime un gráfico ASCII + speedup, y guarda un CSV
en `~/chromascope_persist/bench_<fecha>.csv`.

### Graficar el resultado (PNG)

En **tu máquina** (randall, que tiene matplotlib):

```bash
./graficar.sh                    # baja el último CSV de la rasp y genera el PNG
# o
./graficar.sh bench_XXXX.csv     # grafica un CSV local
```

> **Nota de red:** medir con un nodo *relayed* por Tailscale (ej. randall vía DERP) distorsiona
> el speedup, porque MPI espera al más lento. Para un benchmark limpio usá solo nodos con link
> directo. Verificar el tipo de link desde la rasp:
> ```bash
> tailscale ping -c 2 <IP_del_nodo>     # "direct" = bueno, "DERP(...)" = relay (lento)
> ```

---

## 4. Agregar un worker nuevo

En el nodo nuevo (Ubuntu/Debian x86_64):

1. **Tailscale** conectado (`tailscale ip -4` para ver su IP).
2. Crear/confirmar usuario y **SSH sin contraseña** desde la rasp; agregar alias en
   `~/.ssh/config` de la rasp:
   ```
   Host chroma-<nombre>
       HostName <IP_tailscale>
       User <usuario>
       ForwardX11 no
   ```
3. ⚠️ **Quitar el OpenMPI de apt si está** (contamina el build con `libopen-pal.so.40`):
   ```bash
   sudo apt remove -y libopenmpi-dev libopenmpi3t64 openmpi-bin libcoarrays-openmpi-dev
   ```
4. Compilar en `/usr/local`: **hwloc 2.12.0 → OpenPMIx 5.0.7 → OpenMPI 5.0.7**
   (OpenMPI con `--with-pmix=/usr/local --with-hwloc=/usr/local --with-libevent=/usr
   --with-libevent-libdir=/usr/lib/x86_64-linux-gnu --disable-mpi-fortran`).
5. Dejar copia persistente: `~/chromascope_persist/{chromascope_mpi, test_image.ppm}`.
6. Agregar el nombre a la lista `ALL` en `scripts/run_cluster.sh`.

---

## 5. Comando `mpirun` crudo (referencia)

El script lo arma solo, pero por si se necesita manual (desde la rasp, en `/tmp/chromascope_test`):

```bash
mpirun -np <N> \
  --hostfile hostfile \
  --prefix /usr/local \
  --mca plm_rsh_agent "ssh -x -o BatchMode=yes" \
  --mca btl self,tcp \
  --mca btl_tcp_if_include tailscale0 \
  --mca btl_tcp_disable_family 6 \
  --mca oob_tcp_if_include tailscale0 \
  --wdir /tmp/chromascope_test \
  chromascope_mpi test_image.ppm
```

`hostfile`:
```
localhost slots=1
chroma-chris slots=1
chroma-yerik slots=1
...
```

---

## 6. Solución de problemas (fixes clave)

| Síntoma | Causa | Solución |
|---------|-------|----------|
| `/usr/bin/prted: No existe` | `--prefix` equivocado | usar `--prefix /usr/local` |
| `PMIX ERROR ... line 1540` | PMIx 5.0.5 (bundled) ≠ 5.0.7 de la rasp | instalar **OpenPMIx 5.0.7** en el nodo |
| `Unable to find reachable pairing` | Tailscale mezcla IPv4/IPv6 | `--mca btl_tcp_disable_family 6` (forzar IPv4) |
| `undefined symbol: opal_smsc_base_framework` | OpenMPI 4.1.6 de apt contamina el build | quitar paquetes apt de OpenMPI y recompilar |
| Falla aleatoria entre nodos | versiones de OpenMPI/PMIx/hwloc distintas | alinear los tres en `/usr/local` |
| Se borra `/tmp/chromascope_test` | `/tmp` se limpia al reiniciar | el script lo recrea desde `~/chromascope_persist` |

> **Arquitectura ARM + x86:** NO es problema. La rasp (ARM) corre de master con workers x86
> sin inconvenientes; cada nodo guarda el binario de **su** arquitectura.

---

## 7. Variables de entorno del programa

| Variable | Efecto |
|----------|--------|
| `CHROMA_BENCH=1` | Omite escribir las 3 imágenes de salida (ahorra disco/I-O; para benchmark) |
| `CHROMA_WORK=K` | Repite el cómputo del worker K veces (workload compute-bound para medir speedup) |

---

## 8. Scripts

| Script | Dónde corre | Qué hace |
|--------|-------------|----------|
| `scripts/run_cluster.sh` | en la **rasp** (`/home/beto/`) | corre el clúster (normal o `--bench`) |
| `scripts/graficar.sh` | en **randall** | baja el CSV y genera el PNG del benchmark |
