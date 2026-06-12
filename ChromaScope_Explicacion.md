# ChromaScope — Explicación Técnica Completa

**CE 4303 Principios de Sistemas Operativos**

---

## Índice

1. [Visión general del sistema](#1-visión-general-del-sistema)
2. [Arquitectura del clúster](#2-arquitectura-del-clúster)
3. [Flujo de ejecución completo](#3-flujo-de-ejecución-completo)
4. [Driver GPIO: `chromascope_gpio.c`](#4-driver-gpio-chromascope_gpioc)
5. [Driver LCD: `chromascope_lcd.c`](#5-driver-lcd-chromascope_lcdc)
6. [Protocolo MPI: `mpi_protocol.h`](#6-protocolo-mpi-mpi_protocolh)
7. [Master: `main_server.c`](#7-master-main_serverc)
8. [Worker: `worker.c`](#8-worker-workerc)
9. [Monitor de recursos: `resource_monitor.c`](#9-monitor-de-recursos-resource_monitorc)
10. [Imagen PPM: `ppm.c`](#10-imagen-ppm-ppmc)
11. [Histograma HSV: `histogram.c`](#11-histograma-hsv-histogramc)
12. [Filtro RGB: `rgb_filter.c`](#12-filtro-rgb-rgb_filterc)
13. [Criptografía ChaCha20: `chacha20.c`](#13-criptografía-chacha20-chacha20c)
14. [Scripts de despliegue](#14-scripts-de-despliegue)
15. [Sistema de compilación: Makefiles](#15-sistema-de-compilación-makefiles)
16. [Preguntas frecuentes de defensa](#16-preguntas-frecuentes-de-defensa)

---

## 1. Visión general del sistema

ChromaScope es un sistema de **procesamiento distribuido de imágenes** que combina:

| Componente | Tecnología | Propósito |
|---|---|---|
| Comunicación entre nodos | MPI (Message Passing Interface) | Dividir la imagen y repartir trabajo |
| Cifrado de datos | ChaCha20 (RFC 8439) | Proteger los píxeles en tránsito |
| Procesamiento de imagen | Histograma HSV + Filtro RGB | Analizar y transformar la imagen |
| Driver de kernel (GPIO) | Módulo Linux `.ko` | Controlar el servo SG90 directamente |
| Driver de kernel (LCD) | Módulo Linux `.ko` | Mostrar el histograma en pantalla TFT |
| Red VPN | Tailscale | Conectar nodos en diferentes redes físicas |

### ¿Por qué MPI y no threads?

Los **threads** comparten memoria dentro de un proceso. Para distribuir el trabajo en **máquinas diferentes** (Raspberry Pi + laptops) es necesario un modelo de **paso de mensajes**: cada proceso tiene su propia memoria y los datos se copian explícitamente mediante `MPI_Send`/`MPI_Recv`. MPI es el estándar industrial para computación distribuida de alto rendimiento (HPC).

---

## 2. Arquitectura del clúster

```
┌──────────────────────────────────────────────┐
│             RASPBERRY PI 4 (ARM64)            │
│                                               │
│   Rank 0 — Master (main_server.c)             │
│   ┌────────────────────────────────────┐      │
│   │  Lee imagen PPM                    │      │
│   │  Cifra franjas con ChaCha20        │      │
│   │  Distribuye trabajo via MPI_Send   │      │
│   │  Recolecta resultados MPI_Recv     │      │
│   │  Consolida histograma MPI_Reduce   │      │
│   │  Muestra histograma en LCD         │      │  ─── /dev/chromascope ──► ST7735 TFT
│   │  Mueve servo SG90                  │      │  ─── /dev/chromascope_gpio ► SG90
│   └────────────────────────────────────┘      │
└──────────────────────────────────────────────┘
         │                │                │
    Tailscale VPN (100.x.x.x)
         │                │                │
┌────────┴────┐   ┌────────┴────┐   ┌───────┴─────┐
│  Rank 1     │   │  Rank 2     │   │  Rank 3     │
│  (worker.c) │   │  (worker.c) │   │  (worker.c) │
│             │   │             │   │             │
│ Descifra    │   │ Descifra    │   │ Descifra    │
│ Histograma  │   │ Histograma  │   │ Histograma  │
│ Filtro RGB  │   │ Filtro RGB  │   │ Filtro RGB  │
└─────────────┘   └─────────────┘   └─────────────┘
  chroma-chris      chroma-yerik     chroma-gabriel
```

### División de trabajo

Si la imagen tiene **H filas** y hay **N workers**:
- Filas base por worker: `H / N`
- Filas sobrantes: `H % N` (se reparten una por una a los primeros workers)
- Ejemplo: imagen 300 filas, 3 workers → worker 1: 100 filas, worker 2: 100 filas, worker 3: 100 filas
- Ejemplo: imagen 301 filas, 3 workers → worker 1: 101 filas, worker 2: 100 filas, worker 3: 100 filas

---

## 3. Flujo de ejecución completo

```
mpirun -np 4 ./chromascope_mpi test_image.ppm
        │
        ├─ Rank 0 (Master):
        │   1. ppm_read("test_image.ppm")          ← carga imagen en RAM
        │   2. fill_test_key_nonce()               ← genera clave/nonce ChaCha20
        │   3. Para cada worker (rank 1, 2, 3):
        │      a. Calcular franja (start_row, rows)
        │      b. chacha20_encrypt_buffer()        ← cifrar píxeles de la franja
        │      c. MPI_Send(metadata, TAG=100)      ← enviar config al worker
        │      d. MPI_Send(encrypted, TAG=101)     ← enviar píxeles cifrados
        │   4. MPI_Reduce(zeros → global_hist)     ← colectiva: suma histogramas
        │   5. Para cada worker:
        │      MPI_Recv(filtered, TAG=103)         ← recibir franja filtrada
        │   6. ppm_write("cluster_filtered.ppm")  ← guardar imagen filtrada
        │   7. display_histogram_on_lcd()          ← ioctl al driver LCD
        │   8. gpio_completion_sweep()             ← ioctl al driver GPIO (servo)
        │
        ├─ Rank 1 (Worker 1):
        │   1. MPI_Recv(metadata, TAG=100)         ← esperar instrucciones
        │   2. MPI_Recv(encrypted, TAG=101)        ← recibir píxeles cifrados
        │   3. chacha20_encrypt_buffer()           ← descifrar (mismo XOR)
        │   4. print_resource_snapshot()           ← leer /proc/meminfo + loadavg
        │   5. hsv_histogram_rgb_buffer()          ← calcular histograma parcial
        │   6. rgb_filter_buffer()                 ← aplicar filtro (inversión)
        │   7. MPI_Reduce(partial → master)        ← colectiva: contribuir histograma
        │   8. MPI_Send(filtered, TAG=103)         ← enviar franja filtrada
        │
        └─ Ranks 2, 3: mismo flujo que Rank 1
```

### Operaciones colectivas MPI

`MPI_Reduce` es una operación que **TODOS** los procesos deben llamar simultáneamente:

```
Rank 0: [0, 0, 0, ..., 0]    (contribución vacía del master)
Rank 1: [45, 0, 12, ..., 8]  (histograma de la franja 1)
Rank 2: [23, 5, 31, ..., 0]  (histograma de la franja 2)
Rank 3: [12, 0,  8, ..., 4]  (histograma de la franja 3)
                              ↓ MPI_SUM
Global: [80, 5, 51, ..., 12] (histograma de la imagen completa)
```

---

## 4. Driver GPIO: `chromascope_gpio.c`

### ¿Qué es un módulo de kernel (.ko)?

Un **módulo de kernel** es código C que se carga dinámicamente en el kernel Linux en tiempo de ejecución con `insmod`. Corre en **espacio del kernel** (Ring 0), tiene acceso directo al hardware y comparte el espacio de memoria del kernel. Al contrario de un proceso de usuario, no está aislado por la MMU.

### Acceso directo a registros GPIO (sin gpiolib)

La Raspberry Pi 4 usa el SoC **BCM2711**. Sus registros GPIO están mapeados en la memoria física en la dirección `0xFE200000`. Para acceder a ellos desde el kernel:

```c
gpio_base = ioremap(0xFE200000, 0xB4);   // mapear físico → virtual kernel
```

`ioremap` reserva un rango de direcciones virtuales del kernel y configura la MMU para que apunten a la región de hardware. Luego se accede con:

```c
ioread32(gpio_base + offset)     // leer registro de 32 bits
iowrite32(valor, gpio_base + offset)  // escribir registro de 32 bits
```

### Registros clave del BCM2711

| Registro | Offset | Función |
|---|---|---|
| GPFSEL0 | 0x00 | Configura función de GPIO 0–9 (3 bits/pin: 000=INPUT, 001=OUTPUT) |
| GPSET0  | 0x1C | Escribir 1 en bit N → GPIO N en HIGH (3.3V) |
| GPCLR0  | 0x28 | Escribir 1 en bit N → GPIO N en LOW (0V) |
| GPLEV0  | 0x34 | Leer bit N → nivel lógico actual de GPIO N |

### ¿Por qué GPSET y GPCLR separados?

Los registros GPSET y GPCLR son **write-1-to-set/clear**: escribir un 1 activa/desactiva el bit correspondiente, y escribir 0 no tiene efecto. Esto permite controlar un pin sin afectar los demás mediante una sola escritura (sin lectura-modificación-escritura), lo cual es **atómico a nivel de hardware**.

### Control del servo SG90 (PWM por software)

El servo SG90 requiere una señal **PWM de 50 Hz** (período = 20 ms):

```
 ┌──────┐              ┌──────┐
 │      │              │      │
─┘      └──────────────┘      └─── 
  1000µs     19000µs
  (pulso 0°)

 ┌─────────┐           ┌─────────┐
 │         │           │         │
─┘         └───────────┘         └─
   2000µs      18000µs
   (pulso 180°)
```

El driver genera esta señal con `udelay()` (busy-wait de microsegundos) directamente desde el kernel, sin necesitar un periférico PWM dedicado.

### Interfaz ioctl

```c
// Desde espacio de usuario:
int fd = open("/dev/chromascope_gpio", O_RDWR);

int pin = 18;
ioctl(fd, IOCTL_GPIO_SET_OUTPUT, &pin);  // configurar como salida

struct gpio_cmd_servo cmd = { .pin=18, .angle=90, .duration_ms=500 };
ioctl(fd, IOCTL_GPIO_SET_SERVO_ANGLE, &cmd);  // mover servo
```

Los comandos ioctl se definen con macros `_IOW`/`_IOR`/`_IOWR` que codifican el número mágico (`'C'`), el número de comando y el tamaño del tipo en un entero de 32 bits.

---

## 5. Driver LCD: `chromascope_lcd.c`

### Hardware: pantalla TFT ST7735 (128×160 px)

El controlador **ST7735** es un chip dedicado que maneja la pantalla TFT. Se comunica mediante **SPI** (Serial Peripheral Interface). En este driver se implementa **SPI por software (bit-banging)**: se controlan manualmente las líneas CLK y MOSI con iowrite32 a los registros GPIO, sin usar el periférico SPI del BCM2711.

### Pines de comunicación SPI

| Pin lógico | GPIO BCM | Función |
|---|---|---|
| CLK (SCLK) | 26 | Reloj de sincronización |
| DIN (MOSI) | 22 | Datos del maestro al esclavo |
| CS | 17 | Chip Select (LOW = activo) |
| DC | 27 | Data/Command (HIGH=dato, LOW=comando) |
| RST | 4 | Reset hardware (LOW = reset activo) |

### Protocolo SPI Modo 0 (CPOL=0, CPHA=0)

- **CPOL=0**: el reloj está en LOW cuando está en reposo.
- **CPHA=0**: el dato se muestrea en el **flanco ascendente** del CLK.
- **MSB first**: se envía el bit más significativo primero.

```
MOSI: ─── D7 ─── D6 ─── D5 ─── D4 ─── D3 ─── D2 ─── D1 ─── D0 ───
CLK:  ──┐  ┌──┐  ┌──┐  ┌──┐  ┌──┐  ┌──┐  ┌──┐  ┌──┐  ┌──
        └──┘  └──┘  └──┘  └──┘  └──┘  └──┘  └──┘  └──┘
              ↑ ST7735 lee aquí (flanco ascendente)
```

### Envío de comando vs. dato

```c
// Comando (DC=0):    le dice al ST7735 qué operación ejecutar
// Dato    (DC=1):    le envía argumentos o píxeles al comando anterior

void lcd_write_cmd(u8 cmd) {
    gpio_low(pin_dc);    // DC=0: modo comando
    gpio_low(pin_cs);    // CS=0: activar chip
    spi_write_byte(cmd); // enviar byte por SPI
    gpio_high(pin_cs);   // CS=1: desactivar chip
}
```

### Ventana de dirección (CASET/RASET)

Para escribir un rectángulo en el display, primero se define la ventana:

```c
lcd_write_cmd(0x2A); // CASET: definir columnas x0..x1
lcd_write_cmd(0x2B); // RASET: definir filas y0..y1
lcd_write_cmd(0x2C); // RAMWR: comenzar escritura de píxeles
// A partir de aquí, los bytes son píxeles RGB565
```

El ST7735 avanza automáticamente la posición de escritura de izquierda a derecha y de arriba a abajo dentro de la ventana.

### Formato de color RGB565 (16 bits)

```
Bit: 15 14 13 12 11 | 10  9  8  7  6  5 |  4  3  2  1  0
      R4 R3 R2 R1 R0   G5 G4 G3 G2 G1 G0   B4 B3 B2 B1 B0
      (5 bits Rojo)     (6 bits Verde)       (5 bits Azul)
```

Verde tiene 6 bits (en lugar de 5) porque el ojo humano es más sensible al verde.

### Histograma en la LCD

La función `lcd_show_histogram()` recibe los 36 bins del histograma y dibuja **barras verticales**:
- **Posición horizontal**: distribuida equitativamente en los 160 px del ancho.
- **Altura**: proporcional al conteo del bin, normalizada al bin máximo.
- **Color de la barra**: el matiz HSV que corresponde a ese bin (bin 0 = 0° = rojo, bin 12 = 120° = verde, etc.).

### Secuencia de inicialización

La pantalla necesita ~20 comandos de configuración al encenderse (frame rate, control de potencia, orientación, gamma). Estos se almacenan en tablas de bytes (`st7735_rcmd1[]`, etc.) con el formato: `[n_cmds][cmd][n_args][arg0]...[delay_ms?]`. La función `lcd_exec_cmd_list()` recorre estas tablas y envía cada comando con sus argumentos.

---

## 6. Protocolo MPI: `mpi_protocol.h`

### Tags de mensaje

Los tags permiten que `MPI_Recv` filtre solo el tipo de mensaje esperado:

| Tag | Valor | Contenido |
|---|---|---|
| TAG_METADATA | 100 | `struct StripMetadata` (config completa) |
| TAG_IMAGE_STRIP | 101 | Bytes de píxeles cifrados |
| TAG_FILTERED_STRIP | 103 | Bytes de píxeles filtrados (devueltos al master) |

### `struct StripMetadata`

Esta es la estructura **más importante** del proyecto. Se envía como primer mensaje del master a cada worker y contiene **todo lo que el worker necesita** para procesar su franja:

```c
typedef struct {
    // Geometría
    int width, height;      // dimensiones de la imagen completa
    int start_row, rows;    // qué filas le corresponden a este worker
    int channels;           // bytes por píxel (siempre 3)
    int strip_bytes;        // total de bytes: width × rows × channels

    // Histograma
    int hist_bins;          // número de bins (36)

    // Criptografía ChaCha20
    uint8_t key[32];        // clave de 256 bits
    uint8_t nonce[12];      // nonce único por worker
    uint32_t crypto_counter;// contador de bloque inicial

    // Filtro
    float filter_r_gain;    // ganancia canal R
    float filter_g_gain;    // ganancia canal G
    float filter_b_gain;    // ganancia canal B
} StripMetadata;
```

Se envía como `MPI_BYTE` (bytes crudos) porque es una struct plana sin punteros: el layout en memoria es directamente serializable.

---

## 7. Master: `main_server.c`

### Medición de tiempo con `MPI_Wtime()`

```c
double t_start = MPI_Wtime();  // reloj MPI de alta resolución
// ... todo el trabajo distribuido ...
double t_end = MPI_Wtime();
printf("tiempo=%.4f\n", t_end - t_start);  // parseable por run_cluster.sh
```

`MPI_Wtime()` devuelve segundos como `double` con resolución de microsegundos. Es preferible a `clock()` porque `clock()` mide tiempo de CPU del proceso actual, no tiempo real de pared (wall-clock).

### Nonce único por worker

```c
metadata.nonce[CHACHA20_NONCE_SIZE - 1] ^= (uint8_t)worker_rank;
```

Si todos los workers usaran el mismo nonce+clave, sus keystreamss serían idénticos. Un atacante que observara dos franjas cifradas podría XOR-earlas para obtener `plain1 XOR plain2` (ataque two-time-pad). El XOR del último byte con el rank hace cada keystream único.

### Modo benchmark (`CHROMA_BENCH`)

Cuando la variable de entorno `CHROMA_BENCH=1` está definida:
- No se escriben archivos PPM de salida (evita I/O de disco).
- No se accede a los drivers LCD ni GPIO (los dispositivos pueden no existir).
- Solo se mide el tiempo puro de comunicación MPI y procesamiento.

Esto permite comparar el speedup con 1, 2, 3 y 4 nodos sin contaminación de I/O.

---

## 8. Worker: `worker.c`

### ¿Por qué el histograma se calcula ANTES del filtro?

```c
// CORRECTO: histograma sobre imagen original
hsv_histogram_rgb_buffer(decrypted_strip, ...);  // ← original

// LUEGO: aplicar el filtro
rgb_filter_buffer(decrypted_strip, filtered_strip, ...);
```

El histograma de color debe representar los **colores de la imagen original**. Si se calculara sobre la imagen filtrada (negativo), los colores estarían invertidos (el rojo aparecería como cian, el verde como magenta, etc.) y el histograma sería inútil para análisis de la imagen real.

### Flujo de buffers

```
encrypted_strip  ──► chacha20_decrypt ──► decrypted_strip ──► hsv_histogram
                                                    │
                                                    └──────────► rgb_filter ──► filtered_strip ──► MPI_Send
```

### `MPI_Reduce` colectiva

Todos los procesos (master + workers) **deben llamar** a `MPI_Reduce` en el mismo punto de su ejecución. El master bloquea esperando las contribuciones de todos los workers antes de continuar. Esto actúa como **barrera de sincronización implícita**.

---

## 9. Monitor de recursos: `resource_monitor.c`

### `/proc/meminfo`

El sistema de archivos virtual `/proc` es una interfaz del kernel Linux que expone información del sistema como archivos de texto. No hay un disco físico — el kernel genera el contenido dinámicamente al leer el archivo.

```
MemTotal:       7812744 kB   ← RAM física total instalada
MemFree:         123456 kB   ← RAM completamente libre
MemAvailable:   5432100 kB   ← RAM disponible para nuevas aplicaciones
                                (incluye cachés liberables)
```

### `/proc/loadavg`

```
0.25 0.15 0.08 1/253 12345
^    ^    ^    ^     ^
|    |    |    |     └── PID del último proceso creado
|    |    |    └──────── procesos ejecutables / total de procesos
|    |    └───────────── promedio de carga últimos 15 min
|    └────────────────── promedio de carga últimos 5 min
└─────────────────────── promedio de carga último 1 min
```

Un **load average de 1.0** en una CPU de 4 núcleos significa que el 25% de la capacidad está ocupada. Un load de 4.0 significa todos los núcleos ocupados al 100%.

---

## 10. Imagen PPM: `ppm.c`

### Formato PPM P6 (binary portable pixmap)

```
P6                    ← magic number: P6 = binario, P3 = texto ASCII
480 270               ← ancho alto (en píxeles)
255                   ← valor máximo de color (8 bits por canal)
<datos binarios>      ← width × height × 3 bytes (R, G, B por píxel)
```

El parser salta comentarios (líneas que empiezan con `#`) y espacios en blanco entre tokens, lo cual es válido según la especificación PPM.

### Protección contra overflow en `checked_image_size()`

```c
// MAL: puede desbordarse en size_t
size_t total = (size_t)width * height * 3;  // overflow si imagen > 1.4 Gpx

// BIEN: verificar antes de multiplicar
if (w > SIZE_MAX / h) return -1;        // w×h no cabe en size_t?
if (pixels > SIZE_MAX / 3) return -1;   // pixels×3 no cabe en size_t?
```

Sin esta protección, una imagen maliciosa con dimensiones exageradas podría provocar un **integer overflow** que resulte en `malloc(muy_poco)` seguido de escritura fuera del buffer (heap overflow).

---

## 11. Histograma HSV: `histogram.c`

### ¿Por qué HSV y no RGB?

En el espacio **RGB**, un rojo oscuro y un rojo brillante tienen valores muy distintos. En el espacio **HSV (Hue, Saturation, Value)**:
- **H (Matiz/Hue)**: el color en sí (0°–360°), independiente del brillo.
- **S (Saturación)**: qué tan "puro" es el color.
- **V (Valor/Brillo)**: qué tan claro u oscuro es.

Un histograma de **matiz** clasifica los píxeles por su color dominante, ignorando si están en la sombra o en la luz.

### Conversión RGB → Hue

```
Dado un píxel (R, G, B) normalizado a [0, 1]:

max = máximo(R, G, B)
min = mínimo(R, G, B)
delta = max - min

Si delta = 0: píxel gris (sin color) → Hue = 0°

Si max = R: Hue = 60° × ((G - B) / delta)
Si max = G: Hue = 60° × ((B - R) / delta + 2)
Si max = B: Hue = 60° × ((R - G) / delta + 4)

Si Hue < 0: Hue += 360°
```

### Mapeo a bins

Con 36 bins, cada bin cubre **10° de arco**:
- Bin 0: 0°–10° (rojo puro)
- Bin 3: 30°–40° (naranja)
- Bin 6: 60°–70° (amarillo)
- Bin 12: 120°–130° (verde)
- Bin 18: 180°–190° (cian)
- Bin 24: 240°–250° (azul)
- Bin 30: 300°–310° (magenta)

---

## 12. Filtro RGB: `rgb_filter.c`

### Inversión de colores (negativo fotográfico)

```c
R' = 255 - R
G' = 255 - G
B' = 255 - B
```

Esta operación produce el **negativo fotográfico** de la imagen:
- Blanco (255,255,255) → Negro (0,0,0)
- Rojo (255,0,0) → Cian (0,255,255)
- Verde (0,255,0) → Magenta (255,0,255)
- Azul (0,0,255) → Amarillo (255,255,0)

### ¿Por qué se suprimen los gains con `(void)`?

```c
(void)r_gain;
(void)g_gain;
(void)b_gain;
```

Los parámetros `r_gain`, `g_gain`, `b_gain` están en la API del filtro para una versión futura de ajuste de brillo por canal. En la versión actual solo se implementa la inversión. Sin el `(void)`, el compilador generaría un error `-Wunused-parameter -Werror`, lo cual rompería la compilación.

---

## 13. Criptografía ChaCha20: `chacha20.c`

### ¿Qué es un cifrador de flujo?

Un **cifrador de flujo** genera una secuencia de bytes pseudoaleatoria (keystream) a partir de una clave y un nonce. Los datos se cifran con XOR byte a byte:

```
ciphertext = plaintext XOR keystream
plaintext  = ciphertext XOR keystream  ← misma operación
```

### Estado inicial de ChaCha20 (4×4 = 16 palabras de 32 bits)

```
┌─────────────┬─────────────┬─────────────┬─────────────┐
│  0x61707865 │  0x3320646e │  0x79622d32 │  0x6b206574 │  ← constantes "expa nd 3 2-by te k"
├─────────────┼─────────────┼─────────────┼─────────────┤
│   key[0-3]  │   key[4-7]  │  key[8-11]  │ key[12-15]  │  ← clave (256 bits)
├─────────────┼─────────────┼─────────────┼─────────────┤
│ key[16-19]  │ key[20-23]  │ key[24-27]  │ key[28-31]  │
├─────────────┼─────────────┼─────────────┼─────────────┤
│   counter   │  nonce[0-3] │  nonce[4-7] │ nonce[8-11] │  ← contador + nonce (96 bits)
└─────────────┴─────────────┴─────────────┴─────────────┘
```

### Quarter Round (operación primitiva ARX)

```c
a += b; d ^= a; d = rotl32(d, 16);
c += d; b ^= c; b = rotl32(b, 12);
a += b; d ^= a; d = rotl32(d,  8);
c += d; b ^= c; b = rotl32(b,  7);
```

Cada quarter round mezcla 4 palabras de 32 bits con **Add (suma modular), Rotate (rotación de bits) y XOR**. 20 rondas de quarter rounds aseguran que cada bit de salida depende de todos los bits de entrada.

### ¿Por qué el nonce debe ser único?

Si se reutiliza el mismo nonce con la misma clave, dos mensajes distintos comparten el mismo keystream. Un atacante puede XOR-ear los dos ciphertexts:
```
cipher1 XOR cipher2 = (plain1 XOR ks) XOR (plain2 XOR ks)
                    = plain1 XOR plain2
```
Esto revela la diferencia entre los dos mensajes (ataque "two-time-pad"), lo cual puede ser suficiente para recuperar el contenido original.

### Por qué la misma función cifra y descifra

```c
// Cifrar (master):
output = input XOR keystream(key, nonce, counter)

// Descifrar (worker): misma operación
output = input XOR keystream(key, nonce, counter)
// porque: (plain XOR ks) XOR ks = plain (XOR es su propia inversa)
```

---

## 14. Scripts de despliegue

### `sync_sources.sh` — Sincronizar código fuente

Antes de correr el clúster, el código fuente debe estar actualizado en todos los workers. El script usa **rsync** (si está disponible) o **tar+scp** como fallback:

| Método | Ventaja | Desventaja |
|---|---|---|
| rsync | Solo transfiere archivos modificados (rápido) | Requiere rsync instalado en ambos extremos |
| tar+scp | Universal, no requiere rsync | Transfiere todo el árbol aunque nada cambió |

Los binarios compilados (`.o`, `chromascope_mpi`) se excluyen porque cada arquitectura (ARM64 en RPi, x86_64 en laptops) necesita su propia compilación.

### `run_cluster.sh` — Orquestador principal

El script hace:
1. Cargar los drivers de kernel (GPIO + LCD) con `insmod`.
2. Compilar el binario MPI en la Raspberry Pi (ARM64).
3. Para cada worker candidato: probar SSH, compilar en el worker (x86_64), verificar que el binario existe.
4. Construir el **hostfile** de MPI (una línea `hostname slots=1` por nodo).
5. Lanzar `mpirun` con los flags de Tailscale.

#### Modo benchmark

```bash
./run_cluster.sh --bench 5
```

Corre el programa con 1, 2, 3, ... N workers (5 repeticiones cada uno) y mide el tiempo promedio. Calcula el **speedup**:

```
speedup(k) = T(1 worker) / T(k workers)
```

Si el algoritmo fuera perfectamente paralelo, speedup(k) = k. En la práctica, la overhead de comunicación MPI reduce el speedup.

### Scripts de Tailscale (2 y 4 nodos)

Tailscale crea una **VPN mesh** donde cada dispositivo tiene una IP fija `100.x.x.x` accesible desde cualquier lugar (oficina, casa, diferentes redes). Los flags clave de `mpirun`:

```bash
--mca btl_tcp_if_include tailscale0    # solo usar la interfaz VPN
--mca oob_tcp_if_include tailscale0    # canal de control también por VPN
--mca plm_rsh_agent "ssh -x -o BatchMode=yes"  # SSH sin contraseña
```

Sin estos flags, MPI intentaría usar todas las interfaces de red y fallaría en entornos donde los nodos no están en la misma LAN.

---

## 15. Sistema de compilación: Makefiles

### `ChromaScope/Makefile` — Binarios de aplicación

| Flag | Significado |
|---|---|
| `-Wall` | Todas las advertencias estándar |
| `-Wextra` | Advertencias adicionales (parámetros no usados, etc.) |
| `-Werror` | Las advertencias se tratan como errores (código limpio obligatorio) |
| `-std=c11` | Estándar ISO C 2011 |
| `-O2` | Optimización nivel 2: inlining, vectorización, eliminación de código muerto |

**`mpicc` vs `gcc`**: `mpicc` es un wrapper de GCC que automáticamente agrega las flags de inclusión y enlazado de la librería MPI. Equivale a:
```bash
gcc -I/usr/local/include/openmpi -L/usr/local/lib -lmpi <archivos.c>
```

### `driver/Makefile` — Módulos de kernel

```makefile
KDIR = /lib/modules/$(uname -r)/build   # árbol del kernel en ejecución
obj-m += chromascope_lcd.o              # registrar módulo para compilar

modules:
    $(MAKE) -C $(KDIR) M=$(PWD) modules
```

El sistema de build del kernel usa su propio Makefile. El comando `make -C $(KDIR)` invoca el Makefile del kernel, y `M=$(PWD)` le indica dónde están los fuentes del módulo externo. El resultado es el archivo `chromascope_lcd.ko`.

---

## 16. Preguntas frecuentes de defensa

### ¿Por qué usar MPI y no sockets TCP directos?

MPI abstrae la comunicación entre procesos con una API de alto nivel. Implementar la misma funcionalidad con sockets requeriría manejar manualmente la serialización, el direccionamiento, el manejo de errores, las operaciones colectivas (como `MPI_Reduce`), y el lanzamiento de procesos remotos. MPI hace todo eso en pocas líneas y es el estándar en supercomputación.

### ¿Por qué los drivers acceden directamente a los registros en lugar de usar gpiolib?

La capa gpiolib del kernel Linux agrega una abstracción portable pero introduce overhead (múltiples funciones intermedias, bloqueos, conversiones de número de pin). Para el control preciso de tiempos del PWM del servo (udelay a nivel de microsegundos), el acceso directo es más predecible y rápido.

### ¿ChaCha20 es seguro para producción?

ChaCha20 como algoritmo **sí es seguro** (se usa en TLS 1.3, WireGuard, SSH). La implementación de este proyecto usa una clave y nonce fijos de demostración, lo cual **no es seguro en producción**. En un sistema real: la clave sería aleatoria (generada con `/dev/urandom`), el nonce sería único para cada sesión, y la clave nunca viajaría en texto claro por la red.

### ¿Qué pasa si un worker falla durante la ejecución?

Con el modelo actual (comunicación síncrona punto a punto), si un worker falla, `MPI_Recv` en el master esperará indefinidamente. MPI_Abort termina todos los procesos si un worker llama a él explícitamente. Para tolerancia a fallos real se necesitaría MPI con checkpoint/restart o redundancia de nodos (fuera del alcance de este proyecto).

### ¿Qué significa el speedup obtenido?

El speedup ideal es lineal: 2 workers = 2× más rápido. En la práctica:
- **Overhead de comunicación**: los MPI_Send/Recv tienen latencia.
- **Overhead de cifrado**: el master cifra todas las franjas secuencialmente.
- **Desbalance de carga**: si la imagen no es divisible equitativamente, algunos workers terminan antes que otros.

El benchmark de `run_cluster.sh` mide el tiempo real (wall-clock) entre el primer `MPI_Send` y el último `MPI_Recv`, incluyendo todo el overhead.

### ¿Por qué `MPI_Reduce` en lugar de `MPI_Send` individual de histogramas?

`MPI_Reduce` es una operación **colectiva optimizada**: la librería MPI puede usar un árbol de comunicación (cada nodo suma con su vecino y pasa el resultado), reduciendo el tiempo de O(N) a O(log N) mensajes. Además, el código es más simple: una sola llamada sustituye N `MPI_Send` + N `MPI_Recv`.

### ¿Qué es `ioremap` y por qué es necesario?

En Linux, los procesos del kernel también usan **direcciones virtuales**, no físicas. La MMU protege el acceso a memoria: no se puede leer `0xFE200000` directamente aunque sea la dirección física de los registros GPIO. `ioremap` pide al kernel que cree un mapeo en el espacio de direcciones virtuales del kernel que apunte a esa dirección física, configurando las entradas de la tabla de páginas correspondientes con atributos de I/O (no cacheable, no especulativo).

### ¿Por qué usar `udelay` en lugar de `msleep` en el driver?

- **`msleep(n)`**: pone el proceso a dormir y cede la CPU (puede dormir hasta 1 ms extra por el scheduler). Apropiado para retardos largos.
- **`udelay(n)`**: busy-wait activo de microsegundos. Apropiado para retardos < 1000 µs donde la precisión importa.

El PWM del servo necesita pulsos precisos de 1000–2000 µs. Con `msleep`, el jitter del scheduler rompería la forma de onda. Con `udelay`, el kernel bloquea la CPU pero garantiza la precisión temporal.

---

*Generado para la defensa del Proyecto 2 — CE 4303 Principios de Sistemas Operativos*
