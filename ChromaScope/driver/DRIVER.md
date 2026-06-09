# ChromaScope LCD Driver

**Módulo kernel para pantalla TFT ST7735 (160×128 px, modo paisaje)**
CE 4303 · Principios de Sistemas Operativos · Hardware: Raspberry Pi 4 — BCM2711

---

## Archivos

| Archivo | Descripción |
|---|---|
| `chromascope_lcd.c` | Módulo kernel: SPI bit-bang + protocolo ST7735 + primitivas de dibujo |
| `chromascope_lcd.h` | API pública: structs `ioctl`, constantes de color RGB565, dimensiones |
| `chromascope_gpio.c` | Módulo GPIO genérico (futuro uso: LED matrix, servo) |
| `chromascope_gpio.h` | API ioctl del módulo GPIO |
| `test_lcd.c` | Programa de prueba userspace: colores, rectángulos, espectro HSV, histograma |
| `Makefile` | Compila ambos `.ko` y el binario `test_lcd` |

---

## Arquitectura general

```
Espacio de usuario          Espacio del kernel               Hardware
─────────────────           ──────────────────           ──────────────────
  test_lcd
  libchromascope.a
  (cualquier app)
       │
       │  open("/dev/chromascope")
       │  ioctl(fd, IOCTL_LCD_INIT)
       │  ioctl(fd, IOCTL_LCD_SHOW_HISTOGRAM, &hist)
       ▼
  /dev/chromascope  ──────►  lcd_fops_ioctl()
  (char device)               mutex_lock(&lcd_mutex)
                                    │
                                    │  lcd_fill_rect()
                                    │  lcd_show_histogram()
                                    │  hue_to_rgb565()
                                    ▼
                              spi_write_byte()
                              gpio_high/gpio_low()
                                    │
                                    │  iowrite32/ioread32
                                    ▼
                              gpio_base (virtual)
                              ioremap(0xFE200000)
                                    │
                                    ▼
                          Registros físicos BCM2711
                          GPFSEL0 / GPSET0 / GPCLR0
                                    │
                                    ▼
                          Pines GPIO (CLK, MOSI, CS, DC, RST)
                                    │
                                    ▼
                          Pantalla ST7735 TFT 1.8" 160×128
```

El driver **no usa `gpiolib`, `pinctrl` ni el subsistema SPI del kernel**. Accede directamente a los registros físicos BCM2711 mediante `ioremap`. El bus SPI se implementa por software (bit-banging).

---

## Conexión física

| Pin LCD | Señal    | GPIO RPi 4 | Pin físico | Parámetro módulo |
|---------|----------|-----------|------------|------------------|
| SCK/CLK | Reloj    | GPIO 11   | 23         | `pin_clk=11`     |
| DIN/MOSI| Datos    | GPIO 10   | 19         | `pin_din=10`    |
| CS      | Chip Sel.| GPIO 8    | 24         | `pin_cs=8`       |
| DC/RS   | Dato/Cmd | GPIO 25   | 22         | `pin_dc=25`      |
| RST     | Reset    | GPIO 27   | 13         | `pin_rst=27`     |
| VCC     | 3.3 V    | Pin 1     | —          | —                |
| GND     | Masa     | Pin 6     | —          | —                |
| BL      | Backlight| 3.3 V     | —          | siempre encendido|

Los pines se pueden cambiar al cargar el módulo:
```bash
sudo insmod chromascope_lcd.ko pin_dc=24 pin_rst=22
```


---

## Registros del BCM2711 utilizados

Base física: `0xFE200000` (mapeada con `ioremap` al espacio virtual del kernel)

| Registro | Offset | Función |
|---|---|---|
| `GPFSEL0` | `0x00` | Configura la función de GPIO 0–9 (3 bits por pin: `001` = OUTPUT) |
| `GPFSEL1` | `0x04` | Configura la función de GPIO 10–19 |
| `GPFSEL2` | `0x08` | Configura la función de GPIO 20–29 |
| `GPSET0`  | `0x1C` | Poner un pin en HIGH — escribir `1` en el bit N activa GPIO-N |
| `GPCLR0`  | `0x28` | Poner un pin en LOW — misma lógica de solo escritura que GPSET0 |

### Cómo funciona GPFSEL

Cada pin ocupa exactamente **3 bits** dentro de su registro GPFSEL. La fórmula:

```
registro = GPFSEL0 + (pin / 10) × 4      ← offset en bytes
bits     = (pin % 10) × 3                 ← posición de inicio

Valor de 3 bits:  000 → INPUT   001 → OUTPUT   010–111 → Alt. functions
```

Ejemplo — GPIO 25 (pin DC del display):
- Registro: `GPFSEL2` (offset `0x08`), porque `25 / 10 = 2`
- Bits: `17:15`, porque `(25 % 10) × 3 = 15`
- Para configurarlo como salida se escribe `001` en esos bits

### Por qué GPSET/GPCLR son de solo escritura

Escribir `0` en GPSET no baja el pin; solo escribir `1` en GPCLR lo baja. Así se puede cambiar un pin individual con una escritura de 32 bits **sin leer el registro antes** y sin afectar los demás pines del mismo registro.

---

## Protocolo SPI bit-banging

El driver implementa SPI **Modo 0** (CPOL=0, CPHA=0) por software, sin usar el controlador SPI del hardware:

| Parámetro | Valor |
|---|---|
| Modo | SPI Mode 0 (CPOL=0, CPHA=0) |
| Orden de bits | MSB primero (bit 7 → bit 0) |
| Frecuencia efectiva | ~3–5 MHz (latencia de `iowrite32` ≈ 100 ns) |
| Máximo del ST7735 | 66 MHz escritura, 15 MHz lectura |

### Diagrama de un byte (modo 0, MSB first)

```
MOSI  ─ bit7 ─ bit6 ─ bit5 ─ bit4 ─ bit3 ─ bit2 ─ bit1 ─ bit0 ─
CLK   ─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─
        └─┘ └─┘ └─┘ └─┘ └─┘ └─┘ └─┘ └─
              ↑ ST7735 muestrea MOSI en cada flanco ascendente
```

### Pin DC (Data/Command)

El ST7735 distingue comandos de datos con el pin DC:

| Estado DC | Interpretación del byte SPI |
|---|---|
| DC = LOW (0) | Opcode de **comando** (instrucción al controlador) |
| DC = HIGH (1) | **Dato** (argumento de comando o color de píxel) |

---

## Protocolo de dibujo ST7735

Para escribir píxeles, el driver envía siempre esta secuencia de tres pasos:

```
1. CASET (0x2A) — Column Address Set: define x0 y x1 (4 bytes de dato)
2. RASET (0x2B) — Row Address Set:    define y0 e y1 (4 bytes de dato)
3. RAMWR (0x2C) — Memory Write:       los bytes siguientes son píxeles

   Cada píxel = 2 bytes RGB565:
     Byte alto:  RRRRR GGG   (5 bits Rojo + 3 bits MSB Verde)
     Byte bajo:  GGG BBBBB   (3 bits LSB Verde + 5 bits Azul)
```

### Optimización de lcd_fill_rect

Para un rectángulo de W×H píxeles, CS se baja **una sola vez** antes del bucle y se sube **una sola vez** al final. Esto elimina 2×W×H operaciones GPIO superfluas.

---

## Color RGB565

El ST7735 trabaja con colores de 16 bits empaquetados en el formato RGB565:

| Canal | Bits | Macro de empaquetado |
|---|---|---|
| Rojo   | 15–11 (5 bits) | `(r & 0xF8) << 8` |
| Verde  | 10–5  (6 bits) | `(g & 0xFC) << 3` |
| Azul   |  4–0  (5 bits) | `b >> 3`           |

Colores predefinidos en `chromascope_lcd.h`:

| Constante | Valor hex | Color |
|---|---|---|
| `LCD_BLACK`   | `0x0000` | Negro    |
| `LCD_WHITE`   | `0xFFFF` | Blanco   |
| `LCD_RED`     | `0x001F` | Rojo     |
| `LCD_GREEN`   | `0x07E0` | Verde    |
| `LCD_BLUE`    | `0xF800` | Azul     |
| `LCD_YELLOW`  | `0xFFE0` | Amarillo |
| `LCD_CYAN`    | `0x07FF` | Cian     |
| `LCD_MAGENTA` | `0xF81F` | Magenta  |
| `LCD_ORANGE`  | `0xFD20` | Naranja  |

Macro para crear colores personalizados: `RGB565(r, g, b)`

---

## Visualización del histograma HSV

La función `lcd_show_histogram()` convierte un arreglo de conteos en un gráfico de barras de colores:

```
Bin i  →  Matiz H = i × (360 / n_bins) grados
       →  hue_to_rgb565(H)
       →  barra de color desde la parte inferior de la pantalla
```

### Conversión HSV → RGB565 (aritmética entera pura)

La rueda de color se divide en 6 sectores de 60°:

```
sector = H / 60           (0 a 5)
frac   = H % 60           (0 a 59)
x      = frac × 255 / 60  (componente variable, 0 a 255)

  Sector 0 (  0– 59°): R=255   G=x     B=0     → rojo  → amarillo
  Sector 1 ( 60–119°): R=255-x G=255   B=0     → amarillo → verde
  Sector 2 (120–179°): R=0     G=255   B=x     → verde → cian
  Sector 3 (180–239°): R=0     G=255-x B=255   → cian  → azul
  Sector 4 (240–299°): R=x     G=0     B=255   → azul  → magenta
  Sector 5 (300–359°): R=255   G=0     B=255-x → magenta → rojo
```

### Normalización de barras

```
bar_h[i] = (bins[i] × LCD_HEIGHT) / max(bins)
x_pos[i] = i × LCD_WIDTH / n_bins
```

El bin con mayor conteo ocupa siempre la altura completa. Las barras crecen desde abajo hacia arriba.

---

## API ioctl

Definida en `chromascope_lcd.h`. Número mágico: `'L'`. Dispositivo: `/dev/chromascope`.

| Comando | Número | Argumento | Efecto |
|---|---|---|---|
| `IOCTL_LCD_INIT`           | 1 | — | Inicializa el hardware ST7735 (reset + 3 tablas de init) |
| `IOCTL_LCD_FILL_SCREEN`    | 2 | `struct lcd_cmd_fill *` | Rellena toda la pantalla con un color |
| `IOCTL_LCD_DRAW_PIXEL`     | 3 | `struct lcd_cmd_pixel *` | Dibuja un píxel individual |
| `IOCTL_LCD_DRAW_RECT`      | 4 | `struct lcd_cmd_rect *` | Dibuja un rectángulo sólido |
| `IOCTL_LCD_SHOW_HISTOGRAM` | 5 | `struct lcd_cmd_histogram *` | Visualiza el histograma HSV |

Todos los comandos están protegidos por `mutex_lock_interruptible(&lcd_mutex)`.

### Uso desde C (userspace)

```c
#include <fcntl.h>
#include <sys/ioctl.h>
#include "chromascope_lcd.h"

int fd = open(LCD_DEV, O_RDWR);   // abre /dev/chromascope

// Inicializar
ioctl(fd, IOCTL_LCD_INIT);

// Rellenar de rojo
struct lcd_cmd_fill f = { .color = LCD_RED };
ioctl(fd, IOCTL_LCD_FILL_SCREEN, &f);

// Dibujar rectángulo verde
struct lcd_cmd_rect r = { .x=10, .y=10, .w=50, .h=30, .color=LCD_GREEN };
ioctl(fd, IOCTL_LCD_DRAW_RECT, &r);

// Mostrar histograma (36 bins)
struct lcd_cmd_histogram hist;
hist.n_bins = 36;
for (int i = 0; i < 36; i++)
    hist.bins[i] = conteos[i];
ioctl(fd, IOCTL_LCD_SHOW_HISTOGRAM, &hist);

close(fd);
```

---

## Ciclo de vida del módulo

### `chromascope_lcd_init()` — al cargar con `insmod`

```
1. ioremap(0xFE200000, 0x100)
        └─ mapea registros GPIO físicos al espacio virtual del kernel

2. lcd_setup_pins()
        └─ configura CLK/MOSI/CS/DC/RST como OUTPUT y los lleva a idle

3. alloc_chrdev_region(&dev_num, 0, 1, "chromascope")
        └─ reserva un número mayor dinámicamente

4. cdev_init() + cdev_add()
        └─ registra el dispositivo de caracteres con sus file_operations

5. class_create("chromascope")
        └─ crea /sys/class/chromascope/

6. device_create(...)
        └─ crea /dev/chromascope automáticamente vía udev
```

Si cualquier paso falla, todos los anteriores se deshacen en orden inverso mediante `goto` encadenados.

### `chromascope_lcd_exit()` — al descargar con `rmmod`

```
device_destroy → class_destroy → cdev_del → unregister_chrdev_region → iounmap
```

---

## Compilación y uso

### Requisito previo (una sola vez)

```bash
sudo apt install raspberrypi-kernel-headers
```

### Compilar todo

```bash
cd ChromaScope/driver
make          # genera chromascope_lcd.ko y test_lcd
```

### Cargar el driver LCD

```bash
make load     # sudo insmod chromascope_lcd.ko
```

### Ejecutar la prueba

```bash
sudo ./test_lcd   # o: make run
```

La prueba ejecuta: INIT → colores primarios → 4 rectángulos → espectro HSV → histograma simulado → apagado.

### Descargar el driver

```bash
make unload   # sudo rmmod chromascope_lcd
```

### Ver logs del kernel

```bash
make dmesg
```

Salida esperada al cargar:
```
chromascope_lcd: driver cargado — /dev/chromascope (major=<N>)
chromascope_lcd: CLK=11 MOSI=10 CS=8 DC=25 RST=27
```

Salida al inicializar el display:
```
chromascope_lcd: display ST7735 inicializado (160×128 px, paisaje)
```

---

## Errores comunes

| Error | Causa probable | Solución |
|---|---|---|
| `open: No such file or directory` | Módulo no cargado | `make load` |
| `open: Permission denied` | Falta `sudo` | `sudo ./test_lcd` |
| `insmod: ERROR: could not insert module` | Ya está cargado | `make unload` primero |
| Pantalla en blanco tras INIT | Pines mal conectados | Verificar cableado |
| Colores invertidos (rojo↔azul) | Panel RGB en vez de BGR | Cambiar MADCTL a `0x48` |
| Imagen rotada 90° extra | Orientación diferente del panel | Cambiar MADCTL a `0x68` |
| Kernel panic al cargar | Módulo para otro kernel | `make clean && make` en la RPi |
