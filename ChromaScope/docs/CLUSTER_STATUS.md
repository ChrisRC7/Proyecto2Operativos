#
 ChromaScope — Estado actual del clúster MPI

> CE 4303 Principios de Sistemas Operativos  
> Módulo: Clúster MPI + integración `image/` + `crypto/`  
> Estado: prueba local y prueba distribuida adaptada con 2 computadoras completadas

---

## 1. Resumen del estado actual

Actualmente el clúster MPI ya cuenta con una integración funcional entre:

- `server/`: nodo maestro MPI, `Rank 0`.
- `nodes/`: nodos workers MPI, `Rank 1...N`.
- `image/`: lectura PPM P6, escritura PPM, filtro de escala de grises e histograma HSV.
- `crypto/`: cifrado/descifrado ChaCha20 en C puro.
- `scripts/`: script de ejecución distribuida adaptada por Tailscale.

La arquitectura validada funciona de esta forma:

```text
Rank 0 / Master
├── Lee imagen PPM real
├── Genera imagen en escala de grises
├── Cifra franjas de la imagen con ChaCha20
├── Envía metadata + franjas cifradas por MPI_Send
├── Participa en MPI_Reduce con histograma vacío
└── Recibe histograma consolidado y determina bin dominante

Rank 1...N / Workers
├── Reciben metadata por MPI_Recv
├── Reciben franja cifrada por MPI_Recv
├── Descifran franja con ChaCha20
├── Calculan histograma HSV parcial
├── Muestran snapshot de recursos desde /proc
└── Participan en MPI_Reduce con MPI_SUM
```

---

## 2. Pruebas ya completadas

### 2.1 Prueba MPI local con 4 procesos

Se ejecutó una prueba local con:

```bash
mpirun -np 4 ./chromascope_mpi test_image.ppm
```

Resultado validado:

```text
Rank 0 = master local
Rank 1 = worker local
Rank 2 = worker local
Rank 3 = worker local
```

Se verificó:

- Distribución de imagen en franjas.
- Envío de metadata.
- Envío de franjas cifradas.
- Descifrado en workers.
- Cálculo de histogramas parciales.
- Consolidación con `MPI_Reduce`.
- Validación de pixeles procesados.

---

### 2.2 Prueba de `image/`

Se validó:

- Lectura de imagen PPM P6.
- Escritura de copia PPM.
- Generación de imagen en escala de grises.
- Histograma HSV con 36 bins.
- La suma del histograma coincide con las dimensiones de la imagen.

Para la imagen de prueba:

```text
Dimensiones: 960 x 959
Pixeles esperados: 920640
Pixeles en histograma: 920640
Resultado: OK
```

---

### 2.3 Prueba de `crypto/`

Se validó:

- Cifrado y descifrado de texto.
- Cifrado y descifrado de archivo PPM.
- Comparación byte a byte entre archivo original y archivo descifrado.

Comando usado:

```bash
cmp test_image.ppm decrypted_output.ppm
```

Resultado esperado:

```text
Sin salida de cmp
echo $? = 0
```

También se verificó que el archivo cifrado sea diferente al original.

---

### 2.4 Prueba distribuida adaptada con 2 computadoras por Tailscale

Se ejecutó una prueba real con 2 computadoras físicas conectadas por Tailscale:

```text
Rank 0 = Yerik-Laptop       = Master
Rank 1 = bbg-Summit-E13     = Worker
```

La prueba confirmó:

- OpenMPI lanzó procesos en dos máquinas diferentes.
- El master cargó la imagen PPM real.
- El master generó la imagen gris.
- El master cifró la franja con ChaCha20.
- El worker recibió la franja cifrada.
- El worker descifró con ChaCha20.
- El worker calculó histograma HSV parcial.
- El histograma se consolidó con `MPI_Reduce`.
- La validación final coincidió:

```text
Pixeles esperados: 920640
Pixeles en histograma: 920640
Validación OK
```

---

## 3. Requisitos de instalación en cada computadora

En todas las computadoras, tanto master como workers:

```bash
sudo apt update
sudo apt install build-essential openmpi-bin libopenmpi-dev openssh-server
```

Activar SSH:

```bash
sudo systemctl enable --now ssh
```

Verificar OpenMPI:

```bash
mpicc --version
mpirun --version
```

Verificar Tailscale:

```bash
tailscale status
tailscale ip -4
```

---

## 4. Configuración del master

La computadora master es la que ejecuta `mpirun`.

### 4.1 Configurar SSH hacia cada worker

En la master, editar:

```bash
nano ~/.ssh/config
```

Ejemplo para un worker:

```text
Host chroma-worker
    HostName 100.101.13.93
    User bbg
    ForwardX11 no
```

Para 4 computadoras, se recomienda:

```text
Host chroma-worker-1
    HostName 100.xxx.xxx.xxx
    User usuario1
    ForwardX11 no

Host chroma-worker-2
    HostName 100.xxx.xxx.xxx
    User usuario2
    ForwardX11 no

Host chroma-worker-3
    HostName 100.xxx.xxx.xxx
    User usuario3
    ForwardX11 no
```

Probar conexión:

```bash
ssh chroma-worker hostname
```

Debe responder el hostname del worker sin pedir contraseña.

Si pide contraseña, configurar llave:

```bash
ssh-keygen -t ed25519
ssh-copy-id chroma-worker
```

---

## 5. Configuración de cada worker

En cada worker:

```bash
sudo apt update
sudo apt install build-essential openmpi-bin libopenmpi-dev openssh-server
sudo systemctl enable --now ssh
```

Verificar que Tailscale esté conectado:

```bash
tailscale status
tailscale ip -4
```

El worker no necesita ejecutar manualmente el programa. OpenMPI lo lanza desde el master por SSH.

---

## 6. Prueba local en una sola computadora

Desde la raíz del proyecto:

```bash
make clean
make
mpirun -np 4 ./chromascope_mpi test_image.ppm
```

Esta prueba simula los 4 procesos en una sola computadora:

```text
Rank 0 = master
Rank 1 = worker local
Rank 2 = worker local
Rank 3 = worker local
```

---

## 7. Prueba distribuida con 2 computadoras

Se usa el script:

```bash
./scripts/run_mpi_tailscale_2nodes.sh chroma-worker test_image.ppm
```

Este script realiza:

1. Compilación local.
2. Creación de `/tmp/chromascope_test` en master.
3. Verificación SSH con el worker.
4. Creación de `/tmp/chromascope_test` en worker.
5. Copia del ejecutable y la imagen.
6. Creación de `hostfile`.
7. Ejecución de `mpirun -np 2`.

El `hostfile` generado para 2 nodos es:

```text
localhost slots=1
chroma-worker slots=1
```

Asignación esperada:

```text
Rank 0 = localhost / master
Rank 1 = chroma-worker / worker
```

Importante: esta prueba debe ejecutarse desde una terminal nativa de Ubuntu. Se observó que la terminal integrada de VS Code puede provocar bloqueos con OpenMPI, SSH y Tailscale.

---

## 8. Prueba futura con 4 computadoras

Cuando estén disponibles las 4 computadoras, se debe usar la misma lógica pero con 4 procesos:

```text
Rank 0 = Master
Rank 1 = Worker 1
Rank 2 = Worker 2
Rank 3 = Worker 3
```

Hostfile esperado:

```text
localhost slots=1
chroma-worker-1 slots=1
chroma-worker-2 slots=1
chroma-worker-3 slots=1
```

Comando base:

```bash
env -u DISPLAY -u XAUTHORITY mpirun -np 4 \
  --hostfile hostfile \
  --wdir /tmp/chromascope_test \
  --mca plm_rsh_agent "ssh -x -o BatchMode=yes" \
  --mca btl self,tcp \
  --mca btl_tcp_if_include tailscale0 \
  --mca oob_tcp_if_include tailscale0 \
  ./chromascope_mpi test_image.ppm
```

Validación esperada:

```text
Pixeles esperados = width * height
Pixeles en histograma = width * height
Validación OK
```

---

## 9. Problemas encontrados y solución

### 9.1 VS Code se queda pegado, terminal normal funciona

Se observó que la terminal integrada de VS Code puede bloquear la ejecución distribuida de OpenMPI.

Solución recomendada:

```text
Ejecutar pruebas MPI distribuidas desde una terminal nativa de Ubuntu.
```

---

### 9.2 Warning: Authorization required

Mensaje observado:

```text
Authorization required, but no authorization protocol specified
```

No bloquea la ejecución si el programa continúa. Está asociado a variables gráficas/X11.

Mitigación:

```bash
env -u DISPLAY -u XAUTHORITY mpirun ...
```

También se recomienda usar:

```bash
ssh -x
```

---

### 9.3 OpenMPI intenta entrar con usuario incorrecto

Si aparece algo como:

```text
yerik@100.x.x.x: Permission denied
```

significa que OpenMPI está intentando entrar con el usuario local.

Solución:

- Usar alias en `~/.ssh/config`.
- Usar `chroma-worker` en el hostfile.
- No usar `usuario@ip` directamente dentro del hostfile.

---

## 10. Pendientes del clúster

- Probar con 4 computadoras físicas reales.
- Crear script `run_mpi_tailscale_4nodes.sh`.
- Separar formalmente el `client/`, ya que actualmente el `Rank 0` ejecuta parte del flujo del cliente.
- Integrar con `library/` y `driver/` cuando estén disponibles.
- Ejecutar pruebas con `valgrind` o AddressSanitizer.
- Documentar capturas y resultados en el informe IEEE-Trans.

---

## 11. Comando recomendado para la prueba actual

Desde una terminal normal de Ubuntu:

```bash
./scripts/run_mpi_tailscale_2nodes.sh chroma-worker test_image.ppm
```

No usar terminal integrada de VS Code para la demo distribuida.
