# ChromaScope GPIO Driver

**Módulo kernel para control directo de GPIO en Raspberry Pi 4 (BCM2711)**
CE 4303 · Principios de Sistemas Operativos

---

## Archivos

| Archivo | Descripción |
|---|---|
| `chromascope_gpio.c` | Implementación del módulo kernel |
| `chromascope_gpio.h` | Definiciones `ioctl` compartidas kernel/userspace |
| `test_blink.c` | Programa de prueba: parpadeo de LED cada 3 s |
| `Makefile` | Compilación del módulo y del programa de prueba |

---

## Arquitectura general

```
Espacio de usuario          Espacio del kernel              Hardware
─────────────────           ──────────────────         ──────────────────
  test_blink.c
  (o cualquier app)
       │
       │  open("/dev/chromascope")
       │  ioctl(fd, IOCTL_GPIO_SET_OUTPUT, &pin)
       │  ioctl(fd, IOCTL_GPIO_SET_HIGH,   &pin)
       ▼
  /dev/chromascope  ──────►  chromascope_fops
  (char device)               .unlocked_ioctl()
                                    │
                                    │  iowrite32 / ioread32
                                    ▼
                              gpio_base  (virtual)
                              ioremap(0xFE200000)
                                    │
                                    ▼
                          Registros físicos BCM2711
                          GPFSEL / GPSET0 / GPCLR0 / GPLEV0
                                    │
                                    ▼
                               Pines GPIO 0–27
                               (voltaje 3.3 V)
```

El driver **no usa la capa `gpiolib` ni `pinctrl`** del kernel. Accede directamente a los registros del controlador BCM2711 mediante `ioremap`, lo que le da control total e independiente del sistema.

---

## Registros del BCM2711 utilizados

Base física: `0xFE200000`

| Registro | Offset | Función |
|---|---|---|
| `GPFSEL0–2` | `0x00–0x08` | Configura la función de cada pin (3 bits/pin, 10 pines/registro) |
| `GPSET0` | `0x1C` | Poner un pin en **HIGH** (escribir `1` en el bit del pin) |
| `GPCLR0` | `0x28` | Poner un pin en **LOW** (escribir `1` en el bit del pin) |
| `GPLEV0` | `0x34` | **Leer** el nivel actual de los pines 0–31 |

### Cómo funciona GPFSEL

Cada pin ocupa 3 bits dentro de su registro `GPFSEL`. La fórmula es:

```
registro = GPFSEL0 + (pin / 10) * 4
bits     = (pin % 10) * 3  ..  (pin % 10) * 3 + 2

Valor 000 → entrada (INPUT)
Valor 001 → salida  (OUTPUT)
```

Ejemplo para GPIO 18:
- Registro: `GPFSEL1` (offset `0x04`), porque `18 / 10 = 1`
- Bits: `26:24`, porque `(18 % 10) * 3 = 24`

### Por qué GPSET/GPCLR son de solo escritura

Escribir `0` en GPSET no baja el pin; solo escribir `1` en GPCLR lo baja. Esto permite cambiar pines individuales con una sola operación de 32 bits, sin leer-modificar-escribir y sin afectar a los demás pines.

---

## API ioctl

Definida en `chromascope_gpio.h`. Número mágico: `'C'`.

| Comando | Número | Argumento | Efecto |
|---|---|---|---|
| `IOCTL_GPIO_SET_OUTPUT` | 1 | `int *pin` | Configura el pin como **salida** |
| `IOCTL_GPIO_SET_INPUT`  | 2 | `int *pin` | Configura el pin como **entrada** |
| `IOCTL_GPIO_SET_HIGH`   | 3 | `int *pin` | Pone el pin en **3.3 V** |
| `IOCTL_GPIO_SET_LOW`    | 4 | `int *pin` | Pone el pin en **0 V** |
| `IOCTL_GPIO_READ`       | 5 | `int *pin` | Lee el nivel actual; escribe `0` ó `1` en `*pin` |

Todos los comandos validan:
1. Que el número mágico del comando sea `'C'` (`-ENOTTY` si no).
2. Que el pin esté en el rango `0–27` (`-EINVAL` si no).
3. Que `copy_from_user` / `copy_to_user` no fallen (`-EFAULT` si no).

### Uso desde C (userspace)

```c
#include <fcntl.h>
#include <sys/ioctl.h>
#include "chromascope_gpio.h"

int fd  = open(CHROMASCOPE_DEV, O_RDWR);
int pin = 18;

ioctl(fd, IOCTL_GPIO_SET_OUTPUT, &pin);  // configurar como salida
ioctl(fd, IOCTL_GPIO_SET_HIGH,   &pin);  // encender
ioctl(fd, IOCTL_GPIO_SET_LOW,    &pin);  // apagar

int nivel = pin;
ioctl(fd, IOCTL_GPIO_READ, &nivel);      // nivel = 0 ó 1

close(fd);
```

---

## Ciclo de vida del módulo

### `chromascope_init()` — al cargar con `insmod`

```
1. ioremap(0xFE200000, 0xB4)
        └─ mapea registros físicos al espacio virtual del kernel

2. alloc_chrdev_region(&dev_num, 0, 1, "chromascope")
        └─ reserva un número mayor dinámicamente

3. cdev_init() + cdev_add()
        └─ registra el dispositivo de caracteres con sus file_operations

4. class_create("chromascope")
        └─ crea la clase en /sys/class/chromascope/

5. device_create(...)
        └─ crea /dev/chromascope automáticamente vía udev
```

Si cualquier paso falla, se deshacen en orden inverso (goto encadenados).

### `chromascope_exit()` — al descargar con `rmmod`

Los cinco pasos se deshacen en orden inverso:
`device_destroy → class_destroy → cdev_del → unregister_chrdev_region → iounmap`

---

## Prueba de parpadeo (`test_blink.c`)

Enciende y apaga un LED en **GPIO 18** con un periodo de **3 segundos**.

### Conexión física

```
RPi 4 pin 12 (GPIO 18) ──[330 Ω]── LED (+)
                                    LED (−) ── GND (pin 14)
```

> La resistencia de 330 Ω limita la corriente a ≈ 8 mA con una caída de
> tensión típica de 2 V en el LED (3.3 V − 2 V) / 330 Ω ≈ 3.9 mA, seguro
> para el pin GPIO.

### Flujo del programa

```
open("/dev/chromascope")
ioctl → IOCTL_GPIO_SET_OUTPUT (pin 18)
loop:
    ioctl → IOCTL_GPIO_SET_HIGH  →  sleep(3)
    ioctl → IOCTL_GPIO_SET_LOW   →  sleep(3)
Ctrl+C → ioctl SET_LOW → close(fd) → exit
```

`SIGINT` y `SIGTERM` están capturados para apagar el LED antes de salir.

---

## Compilación y uso

### Requisito previo (una sola vez)

```bash
sudo apt install raspberrypi-kernel-headers
```

### Compilar

```bash
cd ChromaScope/driver
make          # módulo (.ko) + test_blink
```

### Cargar el driver

```bash
make load
# equivale a:
# sudo insmod chromascope_gpio.ko
# ls -la /dev/chromascope
```

### Ejecutar la prueba de parpadeo

```bash
sudo ./test_blink
# o en un paso:
make run
```

### Descargar el driver

```bash
make unload   # sudo rmmod chromascope_gpio
```

### Ver logs del kernel

```bash
make dmesg
# sudo dmesg | grep chromascope | tail -20
```

Salida esperada al cargar:
```
chromascope: registros GPIO mapeados @ phys=0xFE200000 (BCM2711)
chromascope: driver cargado — major=<N>, /dev/chromascope listo
```

Salida esperada al descargar:
```
chromascope: driver descargado
```

---

## Errores comunes

| Error | Causa probable | Solución |
|---|---|---|
| `open: No such file or directory` | El módulo no está cargado | `make load` |
| `open: Permission denied` | Falta `sudo` | Ejecutar con `sudo ./test_blink` |
| `insmod: ERROR: could not insert module` | Ya está cargado | `make unload` primero |
| `ioctl: Invalid argument` | Pin fuera de rango 0–27 | Verificar el número de pin BCM |
| Kernel panic al cargar | Módulo compilado para otro kernel | `make clean && make` en la RPi |
