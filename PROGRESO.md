# Progreso del trabajo de Shaeon en BM-DOS

Fichero de trabajo para retomar el trabajo si una sesión se corta. Se
actualiza al empezar y al terminar cada paso, y se lee al empezar cada sesión.
(Estuvo en docs/PROGRESO.md; docs/ está en el .gitignore del proyecto.)

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
| (el de "web: password from the page, rename, move, free space") | Servidor web al estilo ZXMetalCircle: contraseña desde la web, renombrar, mover, espacio libre |
| (el de "fix: the boot stopped after the \"fb:\" lines") | Cuelgue al arrancar (foto del usuario): segunda llamada a CGlueStdioInit (assert) del commit 6ca96f0; quitada.  PROGRESO.md pasa a la raíz |
| (el de "dos: Ctrl+Alt+Del as on a PC - the BIOS asks...") | Ctrl+Alt+Supr como en un PC: la BIOS emulada pide el reinicio (parche), reinicio de DOS sin reiniciar la placa, CPU y caché del dynarec limpias, MIDI aparcado; R del menú = reiniciar DOS.  **Probar en placa:** 10-20 reinicios seguidos (memoria libre estable), sonido/ratón/CD después, reiniciar tras DOOM (modo protegido), Windows 3.x se queda la combinación. |
| (el de "fix: clock and temperature read only with the panel on") | Temperatura y reloj a 0: BareSysInfoRefresh solo corría con el panel PetSis visible; ahora siempre (y en el menú).  El veto por temperatura del governor no actuaba con el panel oculto.  Corrige la explicación anterior (no era la carrera del búfer del firmware) |

## En curso

### Revisión completa del proyecto (pedida por el usuario, 12-nov)

"Ahora que funciona y estamos afinando: revísalo y ve arreglando lo que veas;
si hay dudas, pregunta antes."  Orden de trabajo; marcar cada casilla al
terminarla y anotar debajo lo encontrado:

- [ ] R1. **Teclado en el menú (fallos vistos por el usuario):** al escribir la
      contraseña salen muchas letras, el Mayús izquierdo no funciona, y sospecha
      que lo que se ve no es lo que se guarda.  Mirar: bare_input.cpp (cómo
      llegan pulsaciones y modificadores), BareMenuKey / BareMenuTick
      (bare_menu.cpp), menu_keymap.h (posiciones ISO: ç, º, <), escritura de
      wpa_supplicant.conf y boot.conf.
- [ ] R2. Menú: guardar/descartar/por defecto de cada pestaña contra los
      ficheros reales (boot.conf, dosbox.conf, cmdline.txt).
- [ ] R3. Arranque: orden, esperas, logo, SUPR, reinicio de DOS.
- [ ] R4. Audio y MIDI: colas, descartes (drop), volumen, pruebas de sonido.
- [ ] R5. Vídeo: presentador, menú encima, restaurar pantalla.
- [ ] R6. Red: WiFi, NTP, servidor web, módem/IPX.
- [ ] R7. Governor y ritmo (P1/P2 de la hoja de ruta).
- [ ] R8. CI, make-sd, parches, README.

## Pendiente

- Previstos en el menú: asignar botones del mando, lanzador de juegos,
  corrección de aspecto, panel de estado, actualizar desde GitHub.
- Arreglo del Makefile con GNU Make 4.3 (PATH en `$(shell)`).
- Probar en placa todo lo del menú y la red (el usuario aún no lo ha probado),
  y el reinicio de DOS (ver la fila de Ctrl+Alt+Supr).
- Llevar el trabajo a Miguel Ángel (mmoroca) como PR.

## Hoja de ruta de rendimiento (12-nov, pendiente de que el usuario elija)

A. Governor: A1 ver qué hace (datos en Rendimiento), A2 subir antes (huecos de
   audio como señal, calentamiento al arrancar un programa), A3 nivel de
   overclock opcional (Pi 4 a 1800/2000), A4 margen de temperatura.
B. Perfilador por partes del frame en el núcleo 0.
C. Compilación: -O3 y LTO en DOSBox-core (medir).
D. Ajustes de DOSBox en el menú: oplemu, frecuencia del mezclador, ciclos
   automáticos.
E. No copiar el frame (si el core lo admite).
F. OPL en el núcleo 3 (solo si B lo justifica).

Lectura del panel del usuario (12-nov, escena de créditos, Pi 4):
`v2 39/s ov0 12.6ms vs | 1500 MHz 43C | L1/2 max over3215 | pace 38/70 w0% bd77
dup 0 + | fr 25673us | HDMI 4734/5292 | drop 9512 gap 0`.  Núcleo 2 bien; la
temperatura ya se lee.  bd77 > 70: la máquina avanza ~10 % más rápido que el
tiempo real sin que el bucle la frene (w0%, "+"), el audio llega de más y se
tira (drop 9512 muestras/s = ~11 % de 88200): posible origen de chasquidos.
El governor recibe el tiempo de toda la llamada (fr 25.7 ms, con ~2 fronteras
dentro) contra el presupuesto de UN frame (14.3 ms) -> ve sobrecarga
(over3215) que no es real y se queda en el máximo.  Propuesta: P1 frenar a la
máquina cuando va por delante (prioritario), P2 governor midiendo por
frontera (A1/A2).

## Notas técnicas

- Compilar: `export PATH=$PWD/opt/arm-gnu-toolchain-15.2.rel1-x86_64-aarch64-none-elf/bin:$PATH; make -j2 RASPPI=4`.
  Con cambios en `bare_env.h` o en el Makefile se recompila casi todo (~15 min).
- Pruebas en el PC: `make check-host` (o las sueltas: check-menu, check-conf,
  check-bootconf, check-platform, check-kernel...).
- `bare_net.cpp`, `bare_sdlnet.cpp`, `bare_web.cpp` no compilan con los
  sustitutos del PC (cabeceras de red de Circle): se listan en check-symbols
  como definidos en otra parte; solo los comprueba la compilación cruzada.
