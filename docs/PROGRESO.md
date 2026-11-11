# Progreso del trabajo de Shaeon en BM-DOS

Fichero de trabajo para retomar el trabajo si una sesión se corta. Se
actualiza al terminar cada paso, y se lee al empezar cada sesión.

**Regla al empezar una sesión:** `git status` y `git log -3`.  Si hay cambios sin
guardar o commits que no aparecen aquí, son de un intento interrumpido: se
revisan como código ajeno antes de usarlos, y se anotan aquí.

Repositorio: fork `github.com/shaeon/baremetaldos` (de `mmoroca/baremetaldos`),
rama `main`.  Entrega: bundle `git bundle create ... <base>..shaeon-main`, que el
usuario aplica con `git fetch <bundle> shaeon-main && git merge --ff-only FETCH_HEAD`.

## Hecho (en `main` del fork)

1. MIDI en el núcleo 1 (Munt + FluidSynth), ganancia, diagnóstico en el panel.
2. Núcleos: gestor único (`bare_cores.cpp`), WFE para los libres.
3. Vídeo en el núcleo 2: triple búfer, doble página, vsync, proxy del mailbox,
   log de Circle fuera de la pantalla, escalador rápido, escalado nítido.
4. Audio: filtro de continua, limitador suave, contador de huecos.
5. CI en GitHub Actions: Pi 3, 4 y 5 + tarjeta completa (`BM-DOS-card.zip`).

## Hecho después del menú, paso 1

En GitHub hasta `9cbffd3` (elegir red WiFi).  Los dos últimos de la tabla
están en local y en el bundle, pendientes de push.

| Commit | Paso |
|---|---|
| 5a138f6 | Menú: ajustes que se guardan, pestaña Vídeo, diálogos |
| 3ea6b07 | Menú: Máquina |
| 80f4a2e | Menú: Sonido (salida, volúmenes, tarjetas, MIDI, pruebas) |
| 3836354 | Menú: Unidades (C:/E: carpetas, A:/D: imágenes en caliente, explorador) |
| 5a7270e | Menú: Entrada (KEYB, ratón, joystick) |
| a9d7eb8 | Menú: Rendimiento; el governor respeta `mode` |
| 289cfc8 | Menú: Arranque; SUPR/F2 al encender |
| a121ca7 | Red 1: Ethernet/WiFi, DHCP, NTP, zona horaria |
| 2d78fd9 | Red 2: módem y null-modem (capa SDL_net sobre sockets de Circle) |
| 357770d | Red 3: IPX sobre UDP |
| 9cbffd3 | Menú: elegir red WiFi (--wrap de ReceiveScanResult, campo de texto, teclado ES/US) |
| 533baea | **Encontrado sin conocer su origen** (intento interrumpido): WiFi encendido por defecto; espera NTP solo si hay red configurada. Revisado: correcto. **Confirmado por el usuario: WiFi siempre encendido.** |
| (el de "net: the card's web server") | Red 4: servidor web de la tarjeta (encontrado a medias de un intento interrumpido, revisado, corregido el escapado en la página, terminado) |
| (el de "fix: firmware calls from two cores, stdout off the screen...") | Fallos vistos en placa: carrera en el búfer del firmware de Circle (parche), stdout fuera de la pantalla, MIDI que perdía eventos sin consumo de audio |
| (el de "boot: BIOS-style logo, no rainbow splash...") | Logo de arranque original, sin arcoíris (disable_splash), boot_delay=0, NTP 5 s máx., tiempo de arranque en pantalla |

## En curso

- Nada.

## Pendiente

- Previstos en el menú: asignar botones del mando, lanzador de juegos,
  corrección de aspecto, panel de estado, actualizar desde GitHub.
- Arreglo del Makefile con GNU Make 4.3 (PATH en `$(shell)`).
- Probar en placa todo lo del menú y la red (el usuario aún no lo ha probado).
- Llevar el trabajo a Miguel Ángel (mmoroca) como PR.

## Notas técnicas

- Compilar: `export PATH=$PWD/opt/arm-gnu-toolchain-15.2.rel1-x86_64-aarch64-none-elf/bin:$PATH; make -j2 RASPPI=4`.
  Con cambios en `bare_env.h` o en el Makefile se recompila casi todo (~15 min).
- Pruebas en el PC: `make check-host` (o las sueltas: check-menu, check-conf,
  check-bootconf, check-platform, check-kernel...).
- `bare_net.cpp`, `bare_sdlnet.cpp`, `bare_web.cpp` no compilan con los
  sustitutos del PC (cabeceras de red de Circle): se listan en check-symbols
  como definidos en otra parte; solo los comprueba la compilación cruzada.
