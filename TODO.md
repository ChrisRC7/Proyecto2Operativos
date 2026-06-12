# ChromaScope — TodoList del Proyecto
> CE 4303 Principios de Sistemas Operativos | Entrega: 12 de Junio, 2026 · 13:00

---

## Leyenda de estados
- [ ] Pendiente
- [~] En progreso
- [x] Completado

---

## 0. Setup inicial y estructura del repositorio

- [~] Crear estructura de carpetas del proyecto
  ```
  ChromaScope/
  ├── driver/       ← módulo kernel (.ko)
  ├── library/      ← biblioteca estática (.a)
  ├── crypto/       ← ChaCha20 puro en C
  ├── image/        ← procesamiento PPM (filtro + histograma)
  ├── client/       ← programa cliente
  ├── server/       ← orquestador MPI (nodo 0)
  ├── nodes/        ← lógica de nodo worker MPI
  ├── hardware/     ← esquemas, scripts de config GPIO
  ├── docs/         ← documentación IEEE-Trans
  └── Makefile      ← Makefile raíz que compila todo
  ```
- [~] Configurar repositorio Git con ramas por componente (`driver`, `mpi`, `crypto`, `image`, `lib`) — rama/commits de clúster, imagen y crypto ya iniciados
- [~] Documentar ambiente de desarrollo (versiones de GCC, OpenMPI, kernel headers, etc.) — agregado documento de estado/instrucciones del clúster
- [~] Preparar `hostfile` de OpenMPI con las 4 PCs y verificar SSH sin contraseña entre nodos — validado con 2 PCs por Tailscale; pendiente 4 PCs
- [~] Instalar dependencias en todos los nodos: `build-essential`, `linux-headers-$(uname -r)`, `openmpi-bin`, `libopenmpi-dev` — validado en master y worker de prueba; pendiente completar en todos los nodos finales

---

## 1. Cifrado — ChaCha20 (RFC 7539) `crypto/`
> Prerequisito de Client y Server/MPI. Sin OpenSSL ni libsodium.

- [x] Implementar `chacha20_block()` — generación del keystream (64 bytes, 20 rondas)
- [x] Implementar `chacha20_encrypt()` / `chacha20_decrypt()` — cifrado XOR por bloques
- [x] Definir estructura de clave: 256 bits (32 bytes) + nonce 96 bits (12 bytes) + counter 32 bits
- [~] Generar clave/nonce por sesión y distribuirla de forma segura antes del envío MPI — existe clave/nonce de prueba en metadata; pendiente generación segura real
- [x] Escribir tests unitarios: cifrar → descifrar → verificar igualdad con original
- [ ] Validar con vectores de prueba del RFC 7539
- [x] Exponer API pública: `chacha20_encrypt_buffer(key, nonce, in, out, len)`

---

## 2. Procesamiento de imagen PPM `image/`
> Prerequisito del Client.

- [x] Implementar parser de formato PPM P6 (binario): leer header + píxeles RGB
- [x] Implementar escritura de archivo PPM P6
- [x] Implementar filtro de escala de grises: `Y = 0.299R + 0.587G + 0.114B`
- [x] Guardar imagen filtrada en nuevo archivo `<nombre>_gray.ppm`
- [x] Implementar cálculo de histograma HSV a partir de la imagen **original**
  - Convertir cada píxel RGB → HSV
  - Cuantizar H en N bins (ej. 36 bins de 10°)
  - Acumular conteos por bin
- [x] Serializar histograma HSV como arreglo de enteros para envío MPI
- [x] Verificar manejo de imágenes grandes sin segfault (validar malloc/free)

---

## 3. Cliente `client/`
> Evaluación: 5%

- [~] Leer ruta de imagen PPM desde argumentos de línea de comandos — implementado temporalmente en `chromascope_mpi`; pendiente separar `client/`
- [~] Aplicar filtro de escala de grises y guardar imagen gris (llama a `image/`) — implementado temporalmente en `Rank 0`; pendiente separar `client/`
- [~] Calcular histograma HSV de imagen original (llama a `image/`) — implementado en workers; pendiente flujo formal del cliente
- [~] Cifrar imagen PPM original byte a byte con ChaCha20 (llama a `crypto/`) — implementado por franjas en `Rank 0`; pendiente cliente dedicado
- [~] Guardar copia del archivo cifrado localmente — se guarda `cluster_encrypted_pixels.bin`; pendiente formato final del cliente
- [ ] Enviar imagen cifrada + clave/nonce al Nodo 0 vía **MPI_Send** — pendiente cliente separado
- [ ] Sin interfaz gráfica — solo CLI
- [ ] Manejo de errores: archivo no encontrado, fallo de cifrado, fallo MPI

---

## 4. Servidor / Nodo Maestro (Nodo 0) `server/`
> Evaluación: 15%

- [ ] Recibir imagen cifrada + clave desde el cliente vía MPI_Recv
- [~] Descifrar imagen con ChaCha20 — validación local del master y descifrado de franjas en workers; pendiente recepción desde cliente real
- [~] Guardar archivo **cifrado** recibido en disco — se guarda `cluster_encrypted_pixels.bin`; pendiente archivo recibido desde cliente real
- [x] Guardar archivo **descifrado** en disco
- [x] Dividir imagen en franjas horizontales equitativas según cantidad de workers
- [x] Enviar cada franja cifrada a los nodos workers con MPI_Send (incluye clave/nonce en metadata)
- [x] Recibir/consolidar histogramas parciales mediante `MPI_Reduce`
- [x] Consolidar histogramas con **MPI_Reduce** (MPI_SUM) → histograma completo
- [x] Determinar bin de color dominante (H con mayor conteo)
- [ ] Llamar a la biblioteca `.a` → driver `/dev/chromascope` → encender LEDs con el histograma
- [ ] Activar servomotor SG90 (barrido 0°→90°) como señal de finalización

---

## 5. Nodos Workers (Nodos 1, 2, 3) `nodes/`
> Evaluación parte del Clúster 15%

- [x] Recibir franja de imagen cifrada + clave/nonce vía MPI_Recv
- [x] Descifrar franja con ChaCha20
- [x] Calcular histograma HSV parcial de la franja
- [x] Enviar histograma parcial al nodo maestro con MPI_Reduce
- [x] Mostrar monitor de recursos del sistema durante el procesamiento (salida de `/proc`)
- [x] **No compartir archivos directamente desde un directorio** — procesamiento distribuido por MPI; el script solo copia binario/imagen para despliegue


---

## 5.1 Estado actual del clúster MPI

- [x] Prueba local con 4 procesos MPI (`mpirun -np 4`) completada.
- [x] Integración `image/` + `crypto/` + MPI completada en ejecución local.
- [x] Imagen PPM real procesada desde `Rank 0`.
- [x] Filtro de escala de grises generado desde el flujo integrado.
- [x] Franjas cifradas con ChaCha20 enviadas a workers.
- [x] Workers descifran franjas y calculan histograma HSV parcial.
- [x] Consolidación por `MPI_Reduce` con `MPI_SUM`.
- [x] Validación de histograma correcta para imagen `960x959`: `920640` pixeles esperados y `920640` pixeles procesados.
- [x] Prueba distribuida adaptada con 2 computadoras por Tailscale completada:
  - `Rank 0`: `Yerik-Laptop` como master.
  - `Rank 1`: `bbg-Summit-E13` como worker.
- [~] Script de ejecución distribuida `scripts/run_mpi_tailscale_2nodes.sh` funcional desde terminal nativa de Ubuntu.
- [ ] Prueba distribuida final con 4 computadoras físicas pendiente por disponibilidad de equipos.
- [ ] Crear script equivalente para 4 nodos: `scripts/run_mpi_tailscale_4nodes.sh`.
- [ ] Separar formalmente `client/` del flujo que actualmente ejecuta `Rank 0`.

---

## 6. Driver de Kernel Linux `driver/`
> Evaluación: 20% — componente crítico

- [ ] Crear módulo de kernel `chromascope.ko` en C con `module_init` / `module_exit`
- [ ] Registrar dispositivo de caracteres en `/dev/chromascope` (`alloc_chrdev_region`, `cdev_add`)
- [ ] Implementar `file_operations`: `open`, `release`, `read`, `write`, `unlocked_ioctl`
- [ ] Definir comandos `ioctl`:
  - `IOCTL_SET_LED_COLOR(row, col, r, g, b)` — setear color de un LED de la matriz
  - `IOCTL_SET_LED_HISTOGRAM(bins[])` — cargar histograma completo en la matriz
  - `IOCTL_SET_SERVO_ANGLE(degrees)` — mover servomotor SG90
  - `IOCTL_CLEAR_LEDS` — apagar toda la matriz
- [ ] Configurar pines GPIO de la Raspberry Pi para la matriz de LEDs RGB (ánodo común)
  - Usar `gpio_request` / `gpio_free` del kernel API
  - Implementar PWM por software si no hay PWM hardware suficiente
- [ ] Configurar pin GPIO para señal PWM del servomotor SG90 (50Hz, 1ms–2ms)
- [ ] **NO usar ningún driver del sistema** para GPIO — implementación desde cero en el módulo
- [ ] Escribir `Makefile` para compilar con `linux-headers` del kernel objetivo (Raspberry Pi OS)
- [ ] Verificar carga con `insmod chromascope.ko` y listado con `lsmod`
- [ ] Verificar presencia de `/dev/chromascope` tras `insmod`
- [ ] Probar `rmmod` sin errores de kernel

---

## 7. Biblioteca estática `library/`
> Evaluación: 10%

- [ ] Definir API pública en `chromascope_lib.h`:
  - `int cs_open_device(const char *dev_path)`
  - `int cs_close_device(int fd)`
  - `int cs_write_histogram(int fd, uint32_t *bins, int n_bins)`
  - `int cs_set_led(int fd, int row, int col, uint8_t r, uint8_t g, uint8_t b)`
  - `int cs_move_servo(int fd, int degrees)`
  - `int cs_clear_leds(int fd)`
- [ ] Implementar cada función usando `ioctl()` sobre `/dev/chromascope`
- [ ] Compilar como biblioteca estática: `ar rcs libchromascope.a *.o`
- [ ] Generar `libchromascope.a` como artefacto verificable
- [ ] Esta biblioteca es la **única** que interactúa con el driver — verificar que server no llame directamente al driver

---

## 8. Hardware físico
> Evaluación: 15%

### Raspberry Pi + LEDs + Servomotor
- [ ] Seleccionar y documentar pines GPIO para la matriz de LEDs RGB
- [ ] Calcular resistencias necesarias para los LEDs RGB (según Vf de cada color)
- [ ] Armar circuito de matriz de LEDs RGB (ánodo común) en protoboard/PCB
- [ ] Conectar servomotor SG90 al pin GPIO PWM de la Raspberry Pi
- [ ] Probar manualmente el circuito antes de integrar el driver
- [ ] Fotografiar el prototipo físico para la documentación
- [ ] Generar diagrama esquemático de componentes (KiCad, Fritzing o manual)

### (Opcional - Punto Extra) ESP32-C6 + TFT LCD 1.8"
- [ ] Desarrollar driver en C para pantalla TFT LCD 1.8" (ST7735/ILI9341 por SPI)
- [ ] Implementar visualización gráfica del histograma en la pantalla
- [ ] Conectar ESP32-C6 vía GPIO/UART a la Raspberry Pi

---

## 9. Makefile raíz y scripts de despliegue

- [~] `Makefile` raíz con targets: `all`, `clean`, `driver`, `lib`, `client`, `server`, `nodes` — compilación de MPI/image/crypto lista; pendiente driver/lib/client formal
- [~] Script `deploy.sh`: copia binarios a cada nodo vía `scp`, inicia `mpirun` — existe script adaptado `run_mpi_tailscale_2nodes.sh`
- [ ] Script `load_driver.sh`: `insmod`, verifica `/dev/chromascope`, ajusta permisos
- [~] Script `run_demo.sh`: flujo completo end-to-end con imagen de prueba — flujo parcial clúster+image+crypto por script de 2 nodos
- [x] Agregar imagen PPM de prueba al repositorio (`test_image.ppm`)

---

## 10. Pruebas de integración

- [x] Test 1 — ChaCha20: cifrar/descifrar un archivo PPM y verificar integridad
- [x] Test 2 — Imagen: aplicar filtro gris y verificar salida visual
- [ ] Test 3 — Driver: cargar módulo, escribir vía ioctl, verificar LEDs encienden
- [ ] Test 4 — Biblioteca: llamar `cs_write_histogram()` y verificar LEDs desde userspace
- [x] Test 5 — MPI local (un host, 4 procesos): verificar distribución de franjas y reducción
- [~] Test 6 — MPI distribuido (4 PCs reales): flujo completo end-to-end — validado con 2 PCs por Tailscale; pendiente 4 PCs
- [~] Test 7 — Flujo completo: cliente → server → cluster → LEDs → servomotor — clúster+image+crypto integrado; pendiente cliente formal, driver, LEDs y servomotor
- [ ] Verificar con `valgrind` que no hay memory leaks ni segfaults en componentes userspace

---

## 11. Documentación (IEEE-Trans) `docs/`
> Evaluación: 20%

- [ ] **Introducción**: teoría de device drivers, MPI, cifrado ChaCha20, histograma HSV
- [ ] **Ambiente de desarrollo**: versiones de SO, kernel, GCC, OpenMPI, hardware utilizado
- [ ] **Atributos de aprendizaje continuo** (4 preguntas requeridas):
  - Necesidades de aprendizaje actuales
  - Tecnologías utilizables
  - Acciones implementadas (organización, búsqueda, repaso)
  - Evaluación crítica de eficiencia
- [ ] **Trabajo en equipo** (7 puntos requeridos con pregunta y respuesta)
- [ ] **Detalles de diseño**:
  - [ ] Diagrama de arquitectura general
  - [ ] Diagrama de componentes (UML)
  - [ ] Diagrama de secuencia (flujo end-to-end)
  - [ ] Diagrama de funcionalidades
  - [ ] Esquema de hardware (GPIO, circuito)
- [~] **Instrucciones de uso** del proyecto — agregado documento de estado e instrucciones del clúster
- [ ] **Tabla de actividades**: bitácora de horas por estudiante
- [ ] **Conclusiones**
- [ ] **Sugerencias y recomendaciones**
- [ ] **Referencias** (formato IEEE)

---

## 12. Entregables finales (antes del 12 de Junio 13:00)

- [ ] Código fuente completo con documentación interna (comentarios)
- [ ] Documentación en formato IEEE-Trans (PDF)
- [ ] `README.md` con instrucciones de compilación y ejecución
- [ ] Todos los archivos necesarios para ejecutar el programa
- [ ] Subir a TecDigital antes de la hora límite

---

## Orden de implementación recomendado

```
Semana 1 (26 Mayo - 1 Jun):
  crypto/    → ChaCha20 + tests
  image/     → parser PPM + filtro gris + histograma HSV

Semana 2 (2 Jun - 6 Jun):
  driver/    → módulo kernel + ioctl + GPIO LEDs + servo
  library/   → .a sobre ioctl del driver
  client/    → lectura PPM + cifrado + envío MPI

Semana 3 (7 Jun - 10 Jun):
  server/    → orquestador MPI + distribución + consolidación
  nodes/     → worker MPI + histograma parcial
  hardware/  → integración física Raspberry Pi

Días finales (11-12 Jun):
  docs/      → documento IEEE-Trans
  testing/   → pruebas integración completa
  entrega    → subir a TecDigital
```

---

## Notas de riesgo y consideraciones

> Tener presentes para no perder puntos

- **Driver**: cada integrante debe poder defender el código — revisar juntos
- **No compartir archivos por directorio** entre nodos — todo por MPI
- **No usar drivers del sistema** para GPIO — solo Kernel Module API
- **Sin soluciones alambradas** (hardcoded) — todo parametrizable
- **Segmentation fault = inaceptable** — revisar con `valgrind` / `AddressSanitizer`
- **lsmod** debe mostrar `chromascope` durante la demostración
- El servidor debe guardar **tanto el archivo cifrado como el descifrado**
- Monitor del sistema activo en **cada nodo** durante la demostración
- Propuesta validada: 2% — ya entregada; asegurarse de no cambiar alcance sin notificar
