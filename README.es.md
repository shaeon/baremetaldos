# BM-DOS

**DOSBox sobre Raspberry Pi sin sistema operativo.**

por [@mmoroca](https://github.com/mmoroca) y [arena.ai](https://arena.ai), 2026

![BM-DOS arrancando en el prompt de DOSBox](images/bm-dos.png)

BM-DOS convierte una Raspberry Pi en una máquina DOS de la época. No hay Linux, ni
escritorio, ni gestor de ventanas: la Pi arranca directamente en el intérprete de
comandos de DOS, y los programas de MS-DOS funcionan. El mismo árbol de fuentes
compila para la Pi 3, la familia Pi 4 (incluidas la 400 y la CM4) y la Pi 5.

---

## Índice

- [Qué es](#qué-es)
- [Hardware](#hardware)
- [Inicio rápido](#inicio-rápido)
- [Entorno de compilación](#entorno-de-compilación)
- [Compilar en GitHub](#compilar-en-github)
- [Compilar desde cero](#compilar-desde-cero)
- [Ponerlo en una tarjeta](#ponerlo-en-una-tarjeta)
- [Configuración: `boot.conf`](#configuración-bootconf)
- [Configuración: `cmdline.txt`](#configuración-cmdlinetxt)
- [Configuración: `config.txt` y `dosbox.conf`](#configuración-configtxt-y-dosboxconf)
- [Teclas](#teclas)
- [El panel de estado](#el-panel-de-estado)
- [Cómo funciona](#cómo-funciona)
- [Pruebas](#pruebas)
- [Problemas típicos](#problemas-típicos)
- [Estructura del repositorio](#estructura-del-repositorio)
- [Licencia](#licencia)
- [Créditos](#créditos)
- [Agradecimientos](#agradecimientos)

---

## Qué es

Una Raspberry Pi arranca el entorno **Circle** (bare metal: sin sistema operativo), que levanta el
framebuffer, el USB, la tarjeta SD y un dispositivo de audio. Sobre eso corre
**DOSBox-core** - el fork libretro de DOSBox - gobernado por un frontend escrito
para este port. La imagen del núcleo es un único `kernel8-rpi4.img` de unos 4 MB.

Lo que funciona hoy:

| Función | Estado |
|---|---|
| Intérprete de DOS, unidades `C:`/`D:` desde la tarjeta | funciona |
| `dosbox.conf` leído de la tarjeta, `[autoexec]` respetado | funciona |
| Modos de texto y gráficos VGA, escalado y relación de aspecto | funciona |
| Sound Blaster, Adlib, altavoz del PC (HDMI o jack de 3,5 mm) | funciona |
| Recompilador dinámico (JIT), seleccionable desde `dosbox.conf` | funciona |
| Teclado, ratón y mandos USB (HID) | funciona |
| Governor del reloj ARM, 600 MHz en el prompt de DOS | funciona |
| Panel de estado, capturas de pantalla, apagado seguro | funciona |
| MIDI: MT-32 (Munt) y General MIDI (FluidSynth), generados en el núcleo 1 | funciona si se compila con `scripts/build-midi-libs.sh` |
| CD-ROM, red, IPX, módem | no implementado |
| Voodoo / 3dfx | no implementado |

La máquina emulada es un 486DX2-66 (23880 ciclos) salvo que `dosbox.conf` diga
otra cosa.

## Hardware

Cualquier Raspberry Pi desde la 3 en adelante, y los modelos posteriores en la
medida en que Circle los soporte. La placa se elige al compilar con `RASPPI`, y el
mismo árbol de fuentes cubre todas:

| `RASPPI` | Placas | Imagen del núcleo |
|---|---|---|
| `3` | Pi 3, Pi 3+, Zero 2 W | `kernel8.img` |
| `4` | **por defecto**: Pi 4, Pi 400, CM4, CM5 | `kernel8-rpi4.img` |
| `5` | Pi 5 | `kernel_2712.img` |

El mínimo es una placa con 1 GB de RAM; 2 GB o más es cómodo, porque el
framebuffer y la memoria del emulador salen del mismo montón.

Aparte de la placa:

* Tarjeta microSD de 2 GB o más, FAT32, una sola partición. **Una vez escrita la
  tarjeta es FAT32 corriente**, así que los juegos, `dosbox.conf` y `boot.conf` se
  editan desde cualquier ordenador - Windows, macOS o Linux - sin herramientas
  especiales. El anfitrión Linux sólo hace falta para *compilar* el núcleo y para
  *escribir* la tarjeta la primera vez.
* Pantalla HDMI y teclado USB. Ratón y mando son opcionales.
* No hace falta ventilador: el governor mantiene el SoC a 600 MHz siempre que no
  se esté emulando nada.

Probado en una **Raspberry Pi 400** con una pantalla HDMI de 1080p.

## Inicio rápido

Si la máquina ya está preparada:

```bash
make                       # compila kernel8-rpi4.img
make card-check            # muestra qué cambiaría en la tarjeta, sin escribir
make card-update           # escribe en una tarjeta que ya arranca, conservando dos/
```

Desde un checkout vacío, la secuencia completa está en [Compilar desde
cero](#compilar-desde-cero). Lleva entre 30 y 90 minutos, la mayor parte
compilando libc++.

## Entorno de compilación

### Anfitrión

Una máquina Linux x86_64. Probado en CachyOS/Arch; los comandos de Debian/Ubuntu
van al lado.

| Paquete | Para qué |
|---|---|
| `git` | traer Circle, circle-stdlib y DOSBox-core |
| `make` | la compilación |
| `python3` | el check de listas de fuentes y los tests generados |
| `mtools`, `dosfstools` | escribir la tarjeta FAT32 |
| `parted` | particionar la tarjeta |
| `curl` | descargar el toolchain |
| `texinfo` | newlib genera su documentación con `makeinfo`, y falla sin él |
| `cmake` | compilar libc++, y Munt y FluidSynth para el MIDI |
| `wget` | el `boot/Makefile` de Circle descarga el firmware con él |

```bash
# Arch / CachyOS
sudo pacman -S --needed base-devel git python mtools dosfstools parted curl texinfo cmake wget

# Debian / Ubuntu
sudo apt install build-essential git python3 mtools dosfstools parted curl texinfo cmake wget
```

Unos 8 GB de disco para los árboles de compilación y el toolchain.

### Toolchain cruzado

**Arm GNU Toolchain para el objetivo AArch64 ELF bare metal (sin sistema operativo)**, prefijo
`aarch64-none-elf-`, versión **15.2 o superior**. No es negociable: el libc++ que
compila circle-stdlib usa builtins del compilador (`__is_array`, `__has_feature`,
`__builtin_operator_new`) que GCC 13 y 14 no tienen, y la compilación muere miles
de líneas dentro de libc++ con errores que parecen una librería rota.

Desde <https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads>, o:

```bash
mkdir -p opt && cd opt
curl -LO https://developer.arm.com/-/media/Files/downloads/gnu/15.2.rel1/binrel/arm-gnu-toolchain-15.2.rel1-x86_64-aarch64-none-elf.tar.xz
tar -xf arm-gnu-toolchain-15.2.rel1-x86_64-aarch64-none-elf.tar.xz
```

Vive **dentro del proyecto**, en `opt/`, que está en `.gitignore`. No se escribe
nada en `$HOME` ni se toca `$PATH`: el `Makefile` pone
`opt/arm-gnu-toolchain-*/bin` delante para su propia invocación, y `make version`
dice qué compilador está usando. Para usar un toolchain de otro sitio:

```bash
make TOOLCHAIN_DIR=/ruta/con/arm-gnu-toolchain-*
```

**No** uses el paquete `gcc-aarch64-linux-gnu` ni el `aarch64-none-elf-gcc` de la
mayoría de las distribuciones: son GCC 13/14 y la compilación falla.

## Compilar en GitHub

`.github/workflows/build.yml` lo compila todo en GitHub Actions en cada push:
`make check-host`, después el núcleo para Pi 3, Pi 4 y Pi 5 en paralelo (con MIDI),
y al final `BM-DOS-card.zip`: la tarjeta con el firmware, los ficheros de
configuración y los tres núcleos, lista para descomprimir en una tarjeta FAT32. Los
núcleos y el zip se descargan desde la página de la ejecución (*Artifacts*), y una
etiqueta `v*` los publica como release.

Lo lento queda en caché (toolchain, circle-stdlib, las librerías MIDI): la primera
ejecución de cada placa tarda alrededor de una hora, las siguientes unos minutos. En
un fork hay que activar Actions una vez desde la pestaña *Actions* del repositorio.

## Compilar desde cero

```bash
git clone https://github.com/mmoroca/baremetaldos.git
cd baremetaldos          # el directorio se llama como el repositorio
chmod +x scripts/*.sh scripts/hostcheck/*.sh
```

El `chmod` no es opcional y conviene saber por qué: git sólo conserva el bit de
ejecución en los ficheros con los que se guardó, y una copia que lo haya perdido
produce scripts que contestan `Permission denied` - o, en un sistema de ficheros
que no guarda ese bit, scripts que no arrancan. Hacerlo una vez después de clonar
no cuesta nada y elimina una clase de fallos confusos.

**1. Traer y compilar Circle, newlib y libc++** (30-60 minutos, la parte larga):

```bash
./scripts/setup.sh              # Pi 3, familia Pi 4 o Pi 5 segun RASPPI, por defecto 4
RASPPI=3 ./scripts/setup.sh     # Pi 3 / Zero 2 W
RASPPI=5 ./scripts/setup.sh     # Pi 5
```

El script comprueba el toolchain, clona `circle-stdlib` con sus submódulos en
`third_party/`, lo configura con libc++, un núcleo de 64 MB y soporte
multinúcleo, y lo compila. Si falta el toolchain, imprime los dos comandos que lo
descargan en `opt/` y se detiene.

**2. Traer DOSBox-core** en la revisión fijada:

```bash
make fetch
make check-rev        # confirma la revisión que espera el port
```

**2b. Opcional: compilar los sintetizadores MIDI** (Munt y FluidSynth, unos
minutos; necesita `cmake`):

```bash
./scripts/build-midi-libs.sh
```

Quedan en `third_party/midi-rpi$RASPPI/`.  El núcleo los enlaza si están y
compila sin MIDI si no están; `make MIDI=0` lo fuerza a apagado.  Después, en la
tarjeta: las ROM del MT-32 (`MT32_CONTROL.ROM` y `MT32_PCM.ROM`, o la pareja
`CM32L_*`) o un SoundFont (`default.sf2`) en `/dosbox/system/`, y en
`dosbox.conf`:

```ini
[midi]
mididevice=mt32        # o fluidsynth
mpu401=intelligent
```

Las ROM y los SoundFont no se incluyen.  El sintetizador corre en el núcleo 1 y
DOSBox sigue en el 0: no se comparte nada de DOSBox, solo un anillo de bytes
MIDI de entrada y otro de muestras de salida (`src/platform/bare_midi.cpp`).  La
idea y el parche de FluidSynth vienen de [mt32-pi](https://github.com/dwhinham/mt32-pi).

**3. Compilar la imagen del núcleo:**

```bash
rm -rf build
make -j"$(nproc)"
```

El resultado es `kernel8-rpi4.img` en la raíz (con `RASPPI=3` es `kernel8.img`, con
`RASPPI=5` es `kernel_2712.img`). `make version` imprime el sello de la
instantánea, el hash del Makefile y el estado de `third_party/`.

Para el jack de 3,5 mm en lugar del audio por HDMI:

```bash
make AUDIO_DEVICE=pwm
```

### El ARM stub de la Pi 4

En una Pi 4, Circle necesita `armstub8-rpi4.bin` junto a `config.txt`.
`make-sd.sh` descarga el firmware, compila el stub y lo copia, así que una tarjeta
escrita con ese script no necesita nada más. Si estás montando la tarjeta a mano:

```bash
make -C third_party/circle-stdlib/libs/circle/boot armstub64
cp third_party/circle-stdlib/libs/circle/boot/armstub8-rpi4.bin sd/
```

## Ponerlo en una tarjeta

Dos caminos, y el orden importa: **primero se escribe una tarjeta desde cero, y
después se actualiza** sin destruir nada. Los dos necesitan la tarjeta insertada
en el ordenador y **montada** - los scripts aceptan un dispositivo de bloque
(`/dev/sdX`) o un punto de montaje, y en Windows se usa una herramienta como
Rufus, balenaEtcher o Win32DiskImager para la primera escritura.

> **Averigua primero qué dispositivo es.** `/dev/sdX` en los comandos de abajo es
> la tarjeta, no el disco duro. `lsblk` (Linux) o `diskutil list` (macOS) antes,
> siempre; una letra equivocada aquí destruye el disco equivocado.

### 1. Una tarjeta desde cero

**Destructivo: particiona y formatea el dispositivo.** Por eso no es un objetivo de
`make`: así nunca queda a una pulsación de `make`.

```bash
./scripts/make-sd.sh --stage ./sdcard     # preparar el árbol primero, mirarlo
sudo ./scripts/make-sd.sh /dev/sdX        # y luego escribirlo en la tarjeta
```

`--stage` merece el paso extra: deja en un directorio local exactamente los
ficheros que necesita una tarjeta, para revisarlos antes de particionar nada.

El script particiona la tarjeta como **una única partición FAT32**, copia el
núcleo, los ficheros del firmware y el contenido de `sd/`, y la deja lista para
arrancar. El reparto es:

```
/            boot.conf, config.txt, cmdline.txt, kernel8-rpi4.img, firmware
/dosbox/     dosbox.conf
/dos/        juegos y programas - vacío; los pones tú
```

Se desmonta la tarjeta, se pone en la Pi y se enciende. El emulador arranca en el
prompt `C:`.

### 2. Una tarjeta que ya arranca

Es el camino de cada día cuando se ha compilado un núcleo nuevo. Sustituye el
núcleo, añade lo que falte y **nunca borra** `dos/`, `dosbox/saves` ni nada
editado a mano:

```bash
make card-check      # imprime el plan, no escribe nada
make card-update     # lo hace
```

o directamente, con la tarjeta montada:

```bash
sudo ./scripts/update-sd.sh /dev/sdX
sudo ./scripts/update-sd.sh --dry-run /dev/sdX
./scripts/update-sd.sh --stage ./sdcard     # preparar un directorio
```

Una tarjeta puede llevar además el kernel de **todas** las placas: `config.txt`
elige uno por modelo, así que una sola tarjeta arranca en Pi 3, Pi 4 y Pi 5 si los
tres kernels están en la raíz. El script copia por defecto el de la placa con la
que se compiló, y copia una lista si se le pasa `KERNELS`:

```bash
RASPPI=4 KERNELS="kernel8.img kernel8-rpi4.img kernel_2712.img" \
    ./scripts/make-sd.sh --stage ./sdcard
```

Cada kernel tiene que existir en el árbol, y cada uno tiene que venir de una
compilación de su propia placa: Circle es específico de placa en tiempo de
compilación (`-DRASPPI` y `-mcpu` salen de `Rules.mk`, y el `configure -r` de
`circle-stdlib` decide lo mismo en `Config.mk`), así que una compilación no puede
producir el kernel de otra placa. El script comprueba que cada línea `kernel=` de
`config.txt` nombre un fichero que esté en la tarjeta.

### 3. A partir de ahí, cualquier ordenador sirve

La tarjeta es FAT32 corriente, sin estructura especial, así que una vez escrita se
edita desde **cualquier** máquina - Windows, macOS o Linux:

* `boot.conf`, `config.txt` y `cmdline.txt` están en la raíz y son texto;
* `dosbox.conf` es texto en `/dosbox/`, y su sección `[autoexec]` es donde se
  montan las unidades;
* los juegos y programas son ficheros y directorios en `/dos/`, y se copian como
  cualquier otro fichero.

Para nada de eso hace falta un anfitrión Linux, ni el toolchain, ni los scripts. La
máquina Linux sólo se necesita para compilar el núcleo y para escribir una tarjeta
la primera vez.

## Configuración: `boot.conf`

`/boot.conf`, en la primera partición de la tarjeta, guarda los ajustes que
pertenecen al **port** y no a DOSBox. Lo lee el propio núcleo, antes de montar
nada, y por eso es un fichero aparte: el modo de pantalla tiene que estar elegido
antes de que exista el framebuffer, y `dosbox.conf` lo parsea DOSBox mucho
después.

El formato es una línea `clave = valor`, `#` empieza un comentario, las claves
desconocidas se ignoran **y se avisan en el log**, y que el fichero no exista no
es un error: todo valor tiene un defecto. Un fichero mal escrito no puede impedir
que la placa arranque; es deliberado.

### `display = ANCHOxALTO`

**Este es el valor que los scripts leen de este fichero**, y depende de en qué placa
esté la tarjeta. Lo que hace es poner `width=` / `height=` en `cmdline.txt` cuando se
hace o se actualiza una tarjeta - y el que decide de verdad es `cmdline.txt`, ver la
nota de abajo y [Configuración: `cmdline.txt`](#configuración-cmdlinetxt). El framebuffer es donde se convierte y se copia la salida de DOSBox, así
que el coste por frame es proporcional a esto y no al tamaño de la pantalla
emulada: cada píxel de más se paga en cada frame, y en las placas pequeñas es ahí
donde se va la tasa de refresco.

| Placa | Empieza por | Por qué | Puede subir a |
|---|---|---|---|
| **Pi 3** | **`800x600`** | el límite es la CPU, no la pantalla: 800x600 son 0,48 M píxeles a convertir por frame frente a los 0,92 M de 1280x720 | 1024x768 si los juegos son ligeros |
| **Pi 4** | **`800x600` a `1280x720`** | 1280x720 es 1,5x exacto en 1080p (~55 MB/s a 60 Hz); empieza en 800x600 si la máquina emulada es un 486 rápido o un Pentium | 1920x1080 con una máquina ligera |
| **Pi 5 y posteriores** | **`1280x720` en adelante** | hay margen para la copia; coge los píxeles | 1920x1080 |

Los otros dos valores existen por algo:

| Valor | Cuándo |
|---|---|
| `1920x1080` | nativo en una pantalla 1080p; el máximo de píxeles a copiar |
| `1024x768` | monitores 4:3 y juegos de la era VGA |
| `640x480` | VGA exacto, un píxel de salida por píxel emulado, sin escalador |

Si el modo se rechaza, se prueba el siguiente hacia abajo automáticamente, así que
un valor no soportado no puede dejar una pantalla negra.

> **`cmdline.txt` manda.** Circle crea su propia consola antes de que este port cree
> la suya, y su tamaño sale de `width=` / `height=` en `cmdline.txt`, no de aquí. Dos
> geometrías distintas son un cambio de modo en caliente, que es lo que dejaba la
> pantalla llena de basura. Así que gana la consola: el port se adapta a lo que
> `cmdline.txt` le dio y, si esta línea dice otra cosa, **arranca igual** y el panel
> dice qué fichero se usó - ver [Configuración: `cmdline.txt`](#configuración-cmdlinetxt).
> Los scripts escriben esta línea en `cmdline.txt` al construir la tarjeta, que es
> por lo que sigue teniendo sentido editarla aquí y relanzar
> `scripts/update-sd.sh`; para una tarjeta que ya existe, el que manda es el otro.

> **Nota sobre el ancho del panel de estado.** El panel ocupa 118 caracteres en su
> estado típico y hasta 133 con todos los campos al máximo, que son 1064 píxeles a
> 8 px por carácter. En un modo más estrecho de 1088 se recortan los campos de la
> derecha, y el último es `drop`. Usa 1280x720 o más para ver la línea entera.

### `cpu_max_mhz = N`

El techo que puede usar el governor del reloj, en MHz. El governor tiene tres
niveles propios - 600 en el prompt de DOS, 1500 para los ciclos por defecto de
DOSBox, 2000 sólo cuando los frames no caben - y esto limita la parte alta de ese
rango. `0` (por defecto) deja que el firmware decida lo que la placa puede hacer.
Es un techo, no una velocidad fija: el governor sigue bajando a 600 cuando no hay
nada que ejecutar.

### Elegir la velocidad de la CPU emulada (ciclos)

`display` decide el coste de *dibujar*; `cycles` decide el coste de *emular*. Son
los dos números que marcan la diferencia entre un juego que va y uno que da
saltos, y están en ficheros distintos.

La velocidad de la máquina emulada es `cycles` en `dosbox/dosbox.conf`:

```ini
cycles=fixed 24000     # 24000 = un 486DX2-66 (ver la tabla de abajo)
cycleup=2000           # Ctrl+F11 y Ctrl+F12 lo mueven en este paso
cycledown=2000
```

El valor que se entrega es 24000, un 486DX2-66, y `cycleup`/`cycledown` son 2000
para que una pulsación sea más o menos una generación de máquina. La tarjeta lleva
un valor por defecto; el valor en marcha se puede ajustar **de forma fina** sin
tocar ningún fichero:

- **Ctrl+F11 / Ctrl+F12** cambian los ciclos con la máquina en marcha, en pasos de
  `cycleup` / `cycledown` (2000 con el fichero que se entrega). Encuentra el número
  bueno en el panel y luego escríbelo en `dosbox.conf` para que sobreviva al
  reinicio.
- `config -set cycles=...` desde el prompt de DOS también funciona, y el core
  considera autoritativo un cambio hecho después del arranque.

Ciclos objetivo según la CPU emulada:

| CPU emulada | MHz | cycles |
|---|---|---|
| 386DX | 25 | 4595 |
| 386DX | 33 | 6075 |
| 486DX | 33 | 12019 |
| 486DX2 | 66 | 23880 |
| 486DX4 | 100 | 33445 |
| 486DX5 | 133 | 47810 |
| Pentium | 60 | 31545 |
| Pentium | 66 | 35620 |
| Pentium | 75 | 43500 |
| Pentium | 90 | 52000 |
| Pentium | 100 | 60000 |
| Pentium | 120 | 74000 |

Cómo usarla: elige la máquina que espera el juego, pon `cycles` cerca de esa cifra
y ajusta con Ctrl+F11/F12 hasta el valor que *vaya bien*: la tabla es un punto de
partida, no una ley, porque el limitador de frames propio del juego y la velocidad
del anfitrión mueven la respuesta. Si el panel marca `pace` muy por debajo de lo
que refresca el modo, se le está pidiendo a la máquina emulada más de lo que el
anfitrión puede dar, y la respuesta es **menos ciclos o un `display` más pequeño**,
en ese orden.

> La opción `cycles` del core es una rejilla (múltiplos de 10000 más una parte
> "fina" en millares), así que una cifra exacta como 23880 no es expresable desde
> la tabla de opciones del core: la tarjeta entrega 24000, que está un 0,5 % por
> encima de un 486DX2-66 real. `cycles=fixed 23880` escrito en `dosbox.conf` **sí**
> se respeta, porque el core parsea el fichero; en `src/platform/bare_coreopts.h`
> está por qué esos dos caminos difieren.

### `video_core = 2 | 0` y `vsync = on | off`

Qué núcleo pone la imagen en pantalla mientras corre el emulador.

| Valor | Qué pasa |
|---|---|
| `video_core = 2` | **por defecto**: el core 2 escala cada frame en una página oculta, cambia a ella y espera el vsync; el core 0 sólo entrega el frame, así que el emulador recupera el tiempo que costaba escalar - se nota sobre todo en una Pi 3 - y la imagen no hace tearing |
| `video_core = 0` | el core 0 escala directamente en la página visible, como antes: para comparar, o si el core 2 da problemas en alguna placa |
| `vsync = on` | **por defecto**: sin tearing; un modo de DOS a 70 Hz en una pantalla de 60 Hz pierde a cambio más o menos un frame de cada siete |
| `vsync = off` | el core 2 cambia de página en cuanto hay un frame listo |

Si el core 2 no está disponible, el port vuelve a 0 por su cuenta. La velocidad de la
máquina emulada no depende de ninguna de las dos opciones: lleva su propio reloj.
Mientras presenta el core 2, el panel muestra `v2 72/s ov9 3.2ms vs` donde antes
decía `4 cores`: frames mostrados por segundo, frames sustituidos antes de que el
core 2 los cogiera, el peor tiempo de escalado de ese segundo, y `vs` mientras el
vsync está activo (`1p` si el firmware sólo dio una página y no hay nada que
cambiar).

### `scaler = sharp | smooth` y una imagen nítida

`scaler = sharp` (por defecto) amplía la imagen por área en las dos direcciones:
una fila de pantalla es la fila de origen que tiene debajo, y solo las filas que
caen en el borde entre dos filas de origen las mezclan, en proporción. `smooth`
interpola cada fila a partir de las dos más cercanas, que es más suave.

Hay un segundo escalador fuera del port: **el de la GPU**. `config.txt` fija el
modo HDMI (`hdmi_mode=16` es 1920x1080), y si el framebuffer (`display` /
`width=`/`height=` en `cmdline.txt`) tiene otro tamaño, el firmware estira el
framebuffer entero al modo HDMI con un filtro de suavizado: un segundo
emborronado encima de lo que haya hecho el port. Para la imagen más nítida, que
coincidan:

| Configuración | Nitidez | Coste |
|---|---|---|
| `display = 1920x1080` con `hdmi_mode=16` | sin escalado de la GPU | 2,25 veces los píxeles de 1280x720 para el escalador; asumible en el core 2 (`video_core = 2`) en Pi 4 / Pi 5 |
| `display = 1280x720` con `hdmi_mode=4` (720p) | sin escalado de la GPU; escala la pantalla | lo más barato; la nitidez depende del televisor |
| `display = 1280x720` con `hdmi_mode=16` | la GPU estira x1,5 con suavizado | lo que trae la tarjeta |

El filtro del propio firmware también se puede poner a vecino más cercano con
`scaling_kernel=8` en `config.txt`: nítido, pero a x1,5 una de cada dos filas sale
doble.

### `mode = auto | performance | powersave`

Se parsea y se valida, pero todavía no se aplica: hacerlo significaría pisar al
governor, y eso no está en este hito. Cualquier valor distinto de `auto` produce un
aviso en el log.

### Lo que deliberadamente **no** está en `boot.conf`

* **El ritmo de frames.** El core anuncia la frecuencia de refresco de su propio
  modo de vídeo y el port la obedece. Forzar un número aquí sería pelearse con la
  emulación: un modo de texto de DOS va a 70 Hz, y clavarlo a 60 hace que la
  máquina emulada corra un 17 % más lenta.
* **La relación de aspecto y el escalado.** Los decide el core (4:3 para todo lo
  que importa) y el escalador los respeta.
* **El reloj de la CPU.** Eso es trabajo del governor, y fijar `arm_freq` en
  `config.txt` le quitaría la decisión.

## Configuración: `cmdline.txt`

`cmdline.txt` **no es un fichero de este port**: es de Circle, y el firmware se lo
pasa al núcleo como línea de órdenes. Lo que fija son las cosas que hay que decidir
antes de DOSBox o fuera de él, y la que más importa aquí es el modo de pantalla,
porque es el tamaño del framebuffer en el que se dibuja todo.

**Es una línea, y sólo los espacios separan opciones.** Circle no lee el fichero de
la tarjeta: se lo pide al firmware (`PROPTAG_GET_COMMAND_LINE`) y parte la respuesta
por `' '` - `doc/cmdline.txt` de Circle: *«Multiple options must be placed on a
single line delimited by a space character»*. Los saltos de línea y los comentarios
con `#` son **datos**: una opción comentada no es una opción, es parte del valor de
la anterior. Por encima de 2048 bytes la cadena entera no cabe en el búfer que
declara el tag del firmware, `CKernelOptions` se rinde y **se ignoran todas las
opciones del fichero** sin decir nada. Por eso `scripts/make-sd.sh` y
`scripts/update-sd.sh` colapsan lo que encuentren a una línea y se niegan a escribir
un fichero que llegue a 2048 bytes.

La tarjeta que construye este proyecto lleva:

```
loglevel=4 logdev=tty1 keymap=es usbpowerdelay=500 width=1280 height=720 socmaxtemp=80
```

Son 87 bytes. `width=` y `height=` se reescriben desde `boot.conf` cada vez que se
hace o se actualiza una tarjeta.

### Las opciones que importan en este port

| Opción | Valores | Qué hace aquí |
|---|---|---|
| `width=` / `height=` | hasta `1920` / `1080` | **El modo de pantalla, y gana sobre `boot.conf`.** Son los píxeles a los que el port convierte cada frame emulado, así que el coste por frame es proporcional a ellos. Sin poner, Circle le pide al firmware el tamaño propio de la pantalla, que llega con el borde de overscan ya restado. La tabla de modos por placa está [más arriba](#configuración-bootconf). |
| `loglevel=` | `0` pánico ... `4` debug, `5` traza | **Es un techo de severidad, no un filtro de destino.** `4` (el valor de Circle por defecto) enseña todo hasta debug; `0`, sólo los pánicos. **No** elige dónde va el log: lo que mantiene cuarenta mensajes del núcleo fuera de la pantalla es `boot_log = quiet` en `boot.conf`. |
| `logdev=` | `tty1` (pantalla), `ttyS1` (UART) | Dónde escribe el logger de Circle. `tty1` en una placa sin cable serie. En una Pi 5 el UART es `ttyS11`. |
| `keymap=` | `DE` `DV` `ES` `FR` `IT` `UK` `US` | La distribución **física** que decodifica Circle, para que el informe USB de la tecla rotulada `ñ` sea el correcto. **No tiene nada que ver** con la distribución que DOSBox entrega a la máquina emulada, que sigue siendo US salvo que `keyboardlayout` diga otra cosa en `dosbox.conf`. Este port le pasa a DOSBox códigos de uso USB y no consulta el keymap de Circle, así que la línea está para la capa de consola (el logger de Circle, la terminal) y no para la máquina emulada. |
| `usbpowerdelay=` | `200` ... `8000` ms | Retardo entre dar corriente a un puerto y hablar con el dispositivo. Aquí `500`; súbelo si en el arranque se pierde un teclado o un pincho en una placa con hub. |
| `socmaxtemp=` | `40` ... `85` °C | La temperatura que el governor del port trata como veto: a esa temperatura o por encima, el reloj baja un nivel. **Aquí `80`, y `80` es también el valor interno por defecto** - así que esta línea no cambia nada. Circle acepta `40`..`78`; por encima de `78` la clave la ignora Circle y sólo queda el `80` del propio port, que es el mismo número. |

### El resto de opciones de Circle

Circle acepta muchas más, y están documentadas donde se implementan -
`doc/cmdline.txt` del Circle con el que hayas compilado
(<https://github.com/rsta2/circle/blob/master/doc/cmdline.txt>). Tocan hardware que
este port no usa a propósito: el boost de USB MIDI, la calibración de pantallas
táctiles, el códec WM8960, un DAC I2S, `gpiofanpin` para un ventilador, `usbspeed=full`,
`usbignore`. Ninguna hace falta para arrancar una tarjeta hecha con estos scripts, y
ninguna está puesta.

Dos merecen mención aunque sólo sea para explicar lo que este port **no** usa:

* **`fast=true`** le pide el reloj máximo al `CCPUThrottle` de Circle. En este núcleo
  no hay `CCPUThrottle`: el reloj es cosa del governor (ver [El panel de
  estado](#el-panel-de-estado) y la nota de la Pi 3 en [Problemas
  típicos](#problemas-típicos)), así que la clave no hace nada.
* **`sounddev=`** elige el dispositivo de sonido de Circle. Este port abre los suyos en
  orden - HDMI, luego PWM (el jack), luego I2S - y se queda con el primero que
  arranca, porque un dispositivo que se construye bien no es un dispositivo que
  suene. Para forzar el jack, compila con `make AUDIO_DEVICE=pwm`.

## Configuración: `config.txt` y `dosbox.conf`

`sd/config.txt` es el fichero del firmware: elige la imagen del núcleo por modelo de
Pi, fija el modo HDMI, habilita el audio, desactiva el overscan y pone los techos de
reloj y tensión que el firmware permitirá. **No** pone `arm_freq` a propósito: el
reloj es del governor, y fijarlo le quitaría la decisión.

`sd/dosbox.conf` es el fichero del emulador, con la sintaxis de DOSBox:

* **`[cpu] cycles` decide si un juego es jugable.** La tabla completa de valores
  objetivo por CPU emulada está en el apartado de `boot.conf` de arriba, y
  Ctrl+F11 / Ctrl+F12 lo cambian con la máquina en marcha.
* **`core = auto|dynamic|normal`** elige el JIT. `auto` usa el intérprete en modo real
  y pasa al recompilador dinámico en modo protegido, que es el valor por defecto y lo
  más rápido para casi todo. `normal` fuerza el intérprete y sirve para comparar
  comportamientos.

## Teclas

| Tecla | Qué hace |
|---|---|
| `PetSis` | mostrar / ocultar el panel de estado |
| `Ctrl+Alt+PetSis` | reiniciar la placa |
| `Ctrl+F9` | apagar de forma segura; después se puede cortar la corriente |
| `Alt+Pause` | pausar / reanudar la máquina emulada |
| `Ctrl+F5` | captura de pantalla en `/DOSBOX/SHOTnnnn.BMP` |
| `Ctrl+F11` / `Ctrl+F12` | ciclos emulados, 2000 por pulsación |

Cortar la corriente **no** es una forma segura de parar la máquina: el FAT de la
tarjeta y la caché de escritura de la propia SD pueden dejar `dosbox.conf`, un
montaje o un fichero de partida a medio escribir, y en una Pi sin sistema operativo no hay
comprobación de sistema de ficheros que lo arregle después. `Ctrl+F9` vacía la
tarjeta, lo dice en el panel y sólo entonces dice que se puede cortar. Si el
vaciado falla, dice eso en su lugar, con el código de error de la propia tarjeta.

## El panel de estado

`PetSis` muestra una línea en una franja al pie de la pantalla sobre la que el
emulador nunca dibuja. Es un diagnóstico, así que viene apagado.

```
Pi 400  4 cores  600 MHz  43C  L2/3 idle idle   pace 70/70 w62% bd70 dup 0   fr 8200us  HDMI 1397/5292  drop 0
└─┬──┘  └──┬──┘  └──┬───┘  └┬┘  └──────┬──────┘   └───────────┬────────────┘   └──┬──┘  └────┬─────┘  └──┬─┘
  placa  núcleos reloj    temp   governor            ritmo y avances        coste del frame audio  descartes
```

### `pace R/W` - lo dibujado frente a lo que el modo refresca

`R` es el número de imágenes que llegaron al callback de vídeo por segundo; `W` es
la frecuencia que el core anunció para el modo actual. **Son dos tasas, no un
objetivo y un resultado**, así que la fracción no es un porcentaje de éxito: un
juego que va perfecto en un modo de 60 Hz imprime 60/60, y el mismo juego igual de
perfecto en 70 Hz imprime 70/70.

| En pantalla | Significado |
|---|---|
| `pace 70/70` | el modo pide 70 y se presentan 70 |
| `pace 34/70` | el modo pide 70 y sólo se producen 34: la máquina no llega, o el juego va marcado por otra cosa |
| `pace 24/70 dup 46` | judder: sólo un tercio de los frames son nuevos, el resto son repeticiones |
| `pace 70/70` con `fr 45ms` | bien marcado, pero cada frame es caro |

**La regla: `R` debe ser igual a `W`, y `W` debe ser la tasa del modo.**

### `bd N` - cuántas veces avanzó la máquina emulada

`pace` cuenta **imágenes**. `bd` cuenta **bordes de frame que cruzó la máquina
emulada**, por segundo. En una máquina bien marcada los dos coinciden; la
diferencia entre ellos es lo que contesta «¿va acelerado?».

| En pantalla | Significado |
|---|---|
| `pace 70/70 bd70` | el bucle de frames es el metrónomo: cada avance produjo su imagen |
| `pace 33/70 bd70` | el emulador avanza a su ritmo y dibuja de menos: va lento, no rápido |
| `pace 33/70 bd140` | **va acelerado**: la máquina emulada avanza el doble de lo que pide su modo |
| `pace 70/70 bd70 +` | además, algunos de esos avances ocurrieron sin que el bucle los pidiera: nada en el bucle de frames puede medirlos |

### `wNN%` - cuánto del segundo se pasó esperando

La fracción del último segundo de reloj que el bucle de frames pasó esperando su
siguiente frame. Una máquina bien marcada espera la mayor parte de cada segundo
(40-90 %) y llama al emulador con el resto. **0 % significa que el bucle no está
marcando el paso**, pero también ocurre legítimamente cuando un frame cuesta más
que el periodo del modo, así que nunca se lee solo.

### `fr NNus` - el coste de una llamada a `retro_run()`

Microsegundos por llamada. Es lo que distingue «el core produce un frame por
llamada» de «el core produce seis»: en una máquina rápida es la diferencia entre
sobrar tiempo y no tener ninguno.

### `lm/max overNNN` - el governor del reloj

Dos niveles de tres (los de la placa), luego una palabra:

| Palabra | Significado |
|---|---|
| `idle` | en pantalla no cambia nada; no se justifica ningún reloj |
| `comf` | los frames van holgados: el reloj puede bajar |
| `busy` | por encima del presupuesto y hay un nivel por encima: el governor está subiendo |
| `max` | por encima del presupuesto en el nivel más alto: ningún reloj lo arreglará |
| `-` | entre holgura y sobrecoste: no hay evidencia en ningún sentido |

`overNNN` es cuántos frames consecutivos han ido por encima del presupuesto. Un
`max` sobre el prompt de DOS significa que la máquina cree que está trabajando
mientras la imagen no cambia, y eso es un fallo que merece un informe.

### `dup N`, `drop N`, `HDMI n/m`

`dup` son los frames que el core pidió repetir (la misma imagen otra vez) por
segundo; `drop` son muestras de audio descartadas por segundo; `HDMI 1397/5292` es
la cola del dispositivo de audio en frames de hardware. Las constantes son
normales. Un `drop` que sube sin parar mientras el sonido entrecorta, no.

## Cómo funciona

El port es un **frontend libretro** para un único core. DOSBox-core se escribió
como core libretro, así que espera un anfitrión que registre callbacks (vídeo,
audio, entrada, entorno, log) y llame a `retro_run()` una vez por frame. Todo lo
que hay debajo es de este port:

* `src/kernel.cpp` - la aplicación Circle: orden de arranque, bucle de frames, el
  pacer, el panel de estado.
* `src/platform/bare_libretro.cpp` - el frontend libretro: callbacks, opciones,
  teclas, apagado.
* `src/platform/bare_video.cpp` - framebuffer, escalado, relación de aspecto, los
  huecos del panel, capturas.
* `src/platform/bare_audio.cpp` - salida HDMI y PWM, remuestreo.
* `src/platform/bare_thread.cpp` - el sustituto monohilo del hilo del emulador de
  DOSBox-core, sobre el planificador cooperativo de Circle.
* `src/platform/bare_sysinfo.cpp` y `governor_policy.h` - el governor del reloj
  ARM, su tabla de niveles y sus reglas de decisión.
* `src/platform/bare_dynarec.cpp` - la caché de código que necesita el JIT y que
  el hardware sin sistema operativo no da por sí solo.
* `src/platform/bare_storage.h` - el vaciado de la tarjeta, que es lo que hace de
  `Ctrl+F9` una forma segura de parar la máquina.

Tres decisiones explican el resto:

1. **El ritmo es el del modo, no 60.** Con `RUN_SYNC` el core vuelve de
   `retro_run()` una vez por frame emulado, así que un modo de texto de 70 Hz
   vuelve 70 veces por segundo. Marcarlo a 60 haría correr la máquina un 17 % más
   rápido, y ningún número de ciclos lo arregla, porque los ciclos deciden cuánto
   trabajo cabe en un frame, no cuántos frames ocurren por segundo.
2. **El reloj se decide en marcha.** El governor arranca a 600 MHz, mide el coste
   de cada frame contra el presupuesto que implica el modo, y sube sólo cuando los
   frames no caben - y sólo mientras la imagen cambia de verdad.
3. **Todo lo que se puede probar en un PC se prueba en un PC.** La compilación
   tiene más de 300 comprobaciones en el anfitrión, contra dobles de Circle y
   FatFs, porque un check que necesita la placa es un check que se ejecuta dos
   veces por semana.

## Pruebas

```bash
make check-host      # todo lo que corre en el anfitrión, sin toolchain
make check           # sólo rutas de inclusión y cabeceras
make distclean       # borra build/ y third_party/ (conserva opt/)
```

`check-host` es un conjunto de programas y scripts pequeños:

| Objetivo | Qué comprueba |
|---|---|
| `check-platform` | los símbolos que promete `bare_env.h` frente a los que definen los ficheros de plataforma |
| `check-kernel` | que `kernel.cpp` compile contra un doble de circle-stdlib |
| `check-bootconf` | el parser de `/boot.conf`, con ficheros que la gente escribe de verdad |
| `check-scaling` | relación de aspecto y uso de pantalla del escalador, por modo |
| `check-dynarec` | el recorrido de tablas de página que hace ejecutable la caché del JIT |
| `check-coreopts` | los ciclos de CPU que el frontend le pide al core |
| `check-resample` | el filtro de imagen, contra patrones con respuesta conocida |
| `check-storage` | la secuencia de vaciado de la tarjeta, contra un FatFs falso |
| `check-governor` | la tabla de niveles del governor y sus reglas de decisión |
| `check-portorder` | cuándo puede anunciarse el puerto de juegos |
| `check-dynarec-hook` | el hook del JIT, compilado contra las fuentes reales del core |
| `check-sources` | la lista de fuentes de este Makefile frente a la de DOSBox-core |

`check-dynarec-hook` y `check-sources` necesitan `third_party/dosbox-core`; sin él
informan **OMITIDO con estado distinto de cero**, a propósito. Un check que no
puede mirar no es un check que pasa.

## Problemas típicos

**`make` dice `Config.mk not found`.** Circle no está compilado. Ejecuta
`./scripts/setup.sh`.

**La compilación muere dentro de libc++ con errores sobre `__is_array` o
`__builtin_operator_new`.** El toolchain es viejo. Tiene que ser GCC 15.2 o
superior; ver [Entorno de compilación](#entorno-de-compilación). `make version` dice
qué compilador está usando el build.

**Un juego va demasiado lento.** Es la CPU emulada, no el port. Los ciclos los
responde el port, no se leen de `dosbox.conf` (el core reescribe esa variable
desde su tabla de opciones durante el arranque), así que cambiar `cycles` ahí no
hace nada. Si el panel muestra el reloj cayendo a 600 MHz mientras el juego sigue
pidiendo fotogramas, eso es otro fallo: ver
[la nota sobre el reloj en una Pi 3](#el-reloj-en-una-pi-3).

**La imagen no llena la pantalla.** Es la relación de aspecto respetándose. Un modo
320x200 es 4:3 en el hardware original, y estirarlo a 16:9 está mal. Usa
`display = 640x480` si quieres VGA exacto.

**No hay sonido por HDMI pero sí por el jack.** El problema es la pantalla, no el
port: `hdmi_drive=2` ya está en `config.txt`, y el port elige el jack cuando el
HDMI no tiene altavoces. `make AUDIO_DEVICE=pwm` fuerza el jack.

**El panel de estado se recorta por la derecha.** El modo es más estrecho de 1088
píxeles; ver [la nota sobre el ancho del panel](#display--anchoxalto).

**Va acelerado y los ciclos no lo arreglan.** Mira `bd` en el panel: si está por
encima de `W`, la máquina emulada avanza más rápido que su propio modo y la
evidencia está en el panel, no en el número de ciclos. Incluye la línea entera del
panel en cualquier informe.

## Estructura del repositorio

```
Makefile                  compilación, checks y objetivos de tarjeta
DOSBOX_REV                la revisión fijada de DOSBox-core
LICENSE                   el texto de la GPL-3.0, literal y sin modificar
NOTICE                    copyright, por qué GPL-3.0, obra combinada, agradecimientos
README.md                 el README principal (inglés)
README.es.md              este mismo documento, en español
src/kernel.cpp            la aplicación Circle y el bucle de frames
src/kernel.h
src/main.cpp              punto de entrada
src/platform/             todo lo que habla con Circle, FatFs o el firmware
src/dosbox-stub/          los pocos símbolos POSIX y MIDI que DOSBox necesita y
                          que el port tiene que aportar
scripts/                  puesta a punto, compilación, tarjetas, checks del host
  setup.sh                toolchain + circle-stdlib, por placa
  make-sd.sh              la tarjeta de una placa, desde cero (destructivo)
  update-sd.sh            una tarjeta que ya arranca (nunca borra)
sd/                       los ficheros que van a la tarjeta, tal como se entregan
patches/                  arreglos para código de terceros, los aplica scripts/setup.sh
```

`third_party/`, `build/`, `opt/` y `sdcard/` se generan y están en `.gitignore`.

## Lecciones aprendidas

Estas son las trampas que costaron tiempo de verdad al sacar este port adelante.
Están aquí porque cualquiera que porte algo a Circle, o a bare metal en una
Raspberry Pi, va a pisar las mismas - y ninguna se ve desde el sofá.

### `cmdline.txt` no es un fichero de opciones: es una línea

Circle no lee `cmdline.txt` de la tarjeta. Se lo pide al firmware con
`PROPTAG_GET_COMMAND_LINE`, y lo que vuelve es **una sola cadena que se parte por
espacios nada más** (`doc/cmdline.txt` de Circle: *"Multiple options must be placed
on a single line delimited by a space character"*). Los saltos de línea son datos,
así que cada línea de comentario y cada salto pasan a formar parte del valor de la
opción anterior.

Y peor: `TPropertyTagCommandLine::String[2048]` en `circle/bcmpropertytags.h`, y
`CKernelOptions` se sale entero si se pasa:

```cpp
if (m_TagCommandLine.Tag.nValueLength >= sizeof m_TagCommandLine.String) return;
```

**Se tiran todas las opciones, sin un mensaje.** Este port llevó durante dos rondas
un `cmdline.txt` de 2623 bytes con comentarios, con `width=800 height=600` en la
línea 46, y la placa usó el modo por defecto del firmware porque Circle nunca vio
esa clave.

*Una línea, sin comentarios, por debajo de 2048 bytes.* `boot.conf` es lo contrario:
lo lee el propio port con `fopen`, así que ahí los comentarios están bien.

### Hay dos framebuffers antes de que DOSBox dibuje nada

`CStdlibAppStdio::Initialize()` crea el framebuffer de consola de Circle, dimensionado
desde `cmdline.txt`. Después este port crea el suyo. Pedirle al firmware otra
geometría en ese momento es un cambio de modo en caliente, que la propia
documentación de Circle avisa de que puede rechazar; y cuando medio funciona, la
pantalla acaba leyendo memoria donde la consola escribió texto, con la paleta de la
consola. Eso era la «basura»: no corrupción, sino un framebuffer que nadie iba a
repintar.

**Así que decide `cmdline.txt` y el port se adapta.** Todos los ficheros que dicen
fijar el modo de pantalla son una petición, y sólo uno de ellos se lee antes de que
exista el primer framebuffer. El port se negaba a arrancar cuando `boot.conf` y
`cmdline.txt` no coincidían, lo que convertía un desacuerdo cosmético entre dos
ficheros de texto en una placa muerta - peor que lo que denunciaba. Ahora escribe los
dos números, dice cuál está usando y sigue. Los scripts siguen escribiendo
`width=`/`height=` desde `boot.conf`, que es la razón de que editar `boot.conf` siga
funcionando cuando se rehace la tarjeta.

### `-Wunused-function` necesita generar código

`gcc -fsyntax-only` **no** avisa de una función estática a la que no llama nadie.
Una comprobación montada sobre `-fsyntax-only` anuncia «sin avisos» siendo incapaz
de ver esa clase de fallo; `-c` sí lo ve. La misma forma de error vale para las
pruebas: **una prueba que imprime un valor sin compararlo pasa cualquier sabotaje.**

### Una imagen quieta no es una máquina en reposo, y `fr` no es trabajo

La medición más difícil del governor, y la que necesitó tres intentos.

`fr 42254us` es lo que tardó `retro_run()`, y en una pantalla de texto la mayor
parte de eso es la máquina emulada **durmiendo** en su propio temporizador - así que
un prompt parece un frame que costó tres presupuestos. Eso es lo que mantenía una
Pi 3 a 1200 MHz sobre una pantalla que no hacía nada, a 63 °C.

Los contadores obvios son peores de lo que parece. Una máquina sana lee `pace 30/60`
o `35/70` con todo fluido, y un juego que repite su imagen - menús, barras de estado,
el hueco entre dos tics - se ve **igual** que un prompt a través de `pace`, de `bd` o
de su cociente. Se probaron dos reglas y las dos fallaron en la placa: «frame
idéntico es reposo» (bajaba el prompt, y también DOOM, y el audio se cortaba) y
«idéntico **y** barato es reposo» (DOOM bien, prompt clavado arriba).

Lo que los separa es la **duración**: un prompt de DOS deja la pantalla literalmente
idéntica durante minutos, y un juego repinta algo en segundos. Así que la regla
espera diez segundos antes de llamar reposo a una pantalla quieta. No es listo, es
observable, y equivocarse cuesta un nivel durante tres segundos.

### DOSBox sigue en un núcleo; el trabajo de alrededor, no

Circle sólo atiende interrupciones en el core 0, y DOSBox-core no es reentrante: sus
globales se tocan sin cerrojos. Así que el emulador corre en el core 0, en un solo
hilo, y ahí se queda. `fr` en una pantalla de texto es sobre todo sueño, así que un
número alto ahí no es una petición de más núcleos.

Lo que se ha movido es el trabajo que no comparte estado con DOSBox
(`src/platform/bare_cores.cpp` arranca los cores 1-3 una vez y los deja dormidos en
WFE hasta que reciben una tarea):

| Core | Tarea | Qué cruza al core 0 |
|---|---|---|
| 0 | DOSBox, el frontend, las interrupciones | - |
| 1 | sintetizadores MT-32 / General MIDI (`bare_midi.cpp`) | un anillo de bytes MIDI de entrada y otro de muestras de salida |
| 2 | presentación de vídeo y el mailbox del firmware (`bare_video.cpp`) | un triple búfer sin bloqueos para los frames; el panel con un seqlock |
| 3 | libre | |

El core 2 es dueño del mailbox mientras presenta porque esperar el vsync mantiene el
spinlock del mailbox hasta que llega el vsync: un cambio de reloj o una lectura de
temperatura desde el core 0 en ese momento pararía la máquina emulada. Así que el
core 0 encola los cambios de reloj y lee valores publicados
(`BareSysInfoSetMailboxProxy`).

### El firmware resta un borde de overscan a tu modo

En modos CEA (`hdmi_group=1`, los de TV) el firmware aplica por defecto un borde de
overscan de 48 píxeles por lado, y el framebuffer que asigna es *«the display's size
minus overscan»* - la frase está en la plantilla de `config.txt` del propio
firmware. En 1080p eso son 1824x984, no 1920x1080. Si un tamaño sale corto, busca
`disable_overscan=1` antes de sospechar del código.

## Licencia

**GNU General Public License versión 3.0**, en `LICENSE`.

Una licencia más permisiva no está disponible para la imagen, y no por elección: el
núcleo enlaza estáticamente Circle (GPL-3.0) y DOSBox-core (GPL-2.0-o-posterior),
así que el binario combinado tiene que ser GPL-3.0 y, si se distribuye, su código
fuente tiene que estar disponible. Este repositorio es ese código. `NOTICE` tiene
la línea de copyright, el razonamiento y la licencia de cada componente que acaba en
la imagen; `LICENSE` es el texto literal de la GPL-3.0, y nada más.

## Créditos

* **DOSBox-core** - el equipo de DOSBox, y Nikos Chantziaras por el port a
  libretro. <https://github.com/libretro/dosbox-core>
* **DOSBox** - el equipo de DOSBox. <https://www.dosbox.com>
* **Circle** - Rene Stange, el entorno C++ sin sistema operativo para Raspberry Pi.
  <https://github.com/rsta2/circle>
* **circle-stdlib** - Stephan Muehlstrasser.
  <https://github.com/smuehlst/circle-stdlib>
* Todo lo escrito para este port - **@mmoroca** y [arena.ai](https://arena.ai),
  2026.

### El reloj en una Pi 3

En una Pi 3 el reloj ARM lo manda el firmware: 600 MHz, 1200 MHz y 1,4 GHz son sus
escalones, y se mueve entre ellos por su cuenta. El port pide una frecuencia y el
firmware puede conceder otra: el panel muestra lo que el reloj *es*, no lo que se
pidió. Si marca 600 MHz mientras el emulador está claramente ocupado, el juego va
a la velocidad que permite el reloj y el sonido se entrecorta.

El veto térmico del governor no es la causa cuando el panel marca 60 °C o menos:
empieza a 80 °C, y `socmaxtemp=` en `cmdline.txt` sólo puede *bajarlo* (el rango
aceptado es 40-78). Poner `cpu_max_mhz` en `boot.conf` elimina el escalón entre
los niveles del governor, lo que hace el fallo más fácil de ver, pero no lo
arregla.

### Parches de Circle, aplicados a mano

`patches/` contiene arreglos para código de terceros. No los aplica la
compilación: tocan un árbol que clona `scripts/setup.sh`, y `make distclean` o un
clon nuevo los pierde.

`patches/circle-rpi3-usbstring.patch` arregla un aborto de Circle en la Pi 3. En
una Pi 3 `CUSBString::GetFromDescriptor()` pide un descriptor de cadena de 4 bytes,
y un dispositivo USB que responde con uno vacío (legal, y aceptado por la
comprobación de justo arriba) hace saltar `assert (m_pUSBString->bLength > 2)` en
`lib/usb/usbstring.cpp`, que detiene el sistema entero: un `assertion failed` en
rojo en pantalla tras la enumeración de dispositivos. Esa rama sólo existe para
`RASPPI <= 3`, así que una Pi 4 no la ejecuta nunca. El parche convierte la
aserción en la respuesta «sin nombre» que el llamante ya sabe manejar.

```bash
cd third_party/circle-stdlib/libs/circle
git apply --check /ruta/a/baremetaldos/patches/circle-rpi3-usbstring.patch
git apply         /ruta/a/baremetaldos/patches/circle-rpi3-usbstring.patch
cd ../.. && make -j"$(nproc)"      # recompila sólo Circle; libc++ se reutiliza
cd ../.. && make -j"$(nproc)"      # reenlaza kernel8.img
```

El parche se aplica a Circle Step51.1.1, la revisión que trae circle-stdlib hoy.

## Agradecimientos

Inspirado por el trabajo de [@rtomasa](https://github.com/rtomasa) en Pi-DOS
(<https://github.com/rtomasa/Pi-DOS>), un DOSBox para Raspberry Pi bajo Linux, y el
reto propuesto por [@shaeon](https://github.com/shaeon) desarrollador de
ZXMetalCircle (<https://github.com/shaeon/ZXMetalCircle>).

No hay código copiado de ninguno de los dos proyectos. Lo que aportaron a éste es
la idea de que una Raspberry Pi puede ejecutar todo el software de un ordenador sin
un sistema operativo debajo, y la razón para intentarlo con DOSBox.
