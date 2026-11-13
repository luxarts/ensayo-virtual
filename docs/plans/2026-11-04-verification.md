# Ensayo Virtual — Verificación (2026-11-04)

Máquina: Windows 11 Pro, i9-12900 (24 hilos), VS 2022, CMake 4.4. Sin interfaz ASIO funcional
(el único driver instalado, "ASIO Helix", no inicializa), por lo que todo el audio de prueba corre
con el dispositivo falso (`FakeAudioDevice`, bloques de 128 muestras a 48 kHz, cadencia por timer
de alta resolución). La red es real (Winsock, loopback).

## Pruebas unitarias (Debug)

`EnsayoTests.exe`: **133 resultados, 0 fallos** en 10 ejecuciones consecutivas (4 sueltas + 6 en
lote, parte de ellas mientras corría la suite de integración). En la primera ejecución tras
compilar Release en paralelo hubo 1 fallo aislado que no volvió a reproducirse en ninguna de las
10 ejecuciones posteriores; queda registrado como posible flake de timing bajo carga.

Cobertura por unidad: BuildConfig (4), Protocol (9), MixMath (6), OpusCodec (6), JitterBuffer (13),
SpscQueue (5), BackingTrack, MixServer, ServerHost (incl. HighResClock), PluginSlot,
FakeAudioDevice, AudioEngine, Settings, SessionLink, SessionClient (contra servidor falso TCP/UDP),
YouTubeImporter (con yt-dlp falso), HeadlessClient (incl. prueba de latencia de dos clientes en
proceso: mediana 23–28 ms), Ui (lobby 20 controles, sesión 49; 50 desde que la sesión tiene "Abrir editor VST"), Screenshot.

## Integración (Release, `tests\integration\run-all.ps1`)

Dos procesos reales de `Ensayo Virtual.exe --headless` + `EnsayoServer.exe` (o servidor embebido
en A). A emite un click cada 250 ms por sus entradas; B detecta el click en su salida de audio;
ambos comparten el reloj QPC de la máquina → latencia ida real (captura + codec + UDP + jitter
buffer del servidor + tick + UDP + jitter buffer del cliente + salida).

| Script | Resultado | Latencia ida (ms) | Detalle |
|---|---|---|---|
| two-instances | PASS | mediana 24.06, p95 29.42, n 39 (otra corrida: 23.64 / 24.16 / 40) | `participants 2`, 0 underruns |
| embedded-host | PASS | mediana 15.80, p95 26.49, n 40 (otra: 21.56 / 24.60 / 39) | servidor dentro del cliente A, tickeado por su callback |
| backing-track | PASS | — | C sube `click.wav` (4 s, 16 clicks) y manda `play`; B detecta 16 clicks y `trackPlaying:true` en sus stats |
| packet-loss | PASS | mediana 22.67, p95 25.77, n 36 | B descarta 10 % de bloques recibidos; sin crash, PLC, stats con underruns |
| screenshots | PASS | — | `lobby.png`, `session.png`, `controls.json` (69 controles esperados presentes, visibles, con tamaño) + captura real de la ventana `--demo` |

Salidas en `tests/integration/out/<script>/` (ignoradas por git).

Las marcas de tiempo del click (emisión y detección) son las del inicio del bloque en cada
extremo, así que cada medida tiene una granularidad de ±1 bloque (±2.7 ms); la mediana sobre
≈ 40 clicks es representativa.

Comparación con el presupuesto del spec (§2.4, ≈ 38 ms con ping 10 ms): en loopback el ping es
≈ 0, así que el resto del pipeline (≈ 22–25 ms) coincide con lo previsto: 2 × ~2.7 ms de bloque
+ jitter buffers de 3–4 bloques en cada extremo + codec.

## Capturas de UI

- `tests/integration/out/screenshots/shots/lobby.png` — Nombre, Rol, Crear sesión, Link de sesión,
  Unirse; Dispositivo ASIO, Frecuencia, Buffer, Canal micrófono, Canal instrumento, Salida, Panel
  ASIO, Latencia; Plugin, Examinar…, Abrir editor, Bypass, Quitar; barra de estado + Reintentar.
- `.../session.png` — Link + Copiar link + Salir; franja "Yo (monitoreo)" (Voz/Instrumento faders,
  Mute, medidores); tres participantes (faders deshabilitados según rol); Backing track (Cargar
  archivo…, URL de YouTube, Descargar, Pausa, Stop, Loop, nombre, tiempo, posición, Volumen, Mute,
  progreso); barra de estado con Ping, Jitter, Latencia est., Pérdida, CPU.
- `.../demo-window.png` — captura real de la ventana (`--demo`, PrintWindow), idéntica al render.

Revisadas visualmente: todos los controles presentes, texto en español con acentos correctos, sin
solapamientos a 1100×760.

## Bugs reales encontrados por las pruebas (y corregidos)

1. `ServerHost::tick` saltaba frames cuando el hilo UDP tenía el lock de sesiones → cada salto
   vaciaba un bloque del jitter buffer de todos los clientes. Ahora cuenta los ticks perdidos y
   los recupera (hasta 4) en el siguiente tick.
2. `JitterBuffer` nunca se re-anclaba cuando el consumidor quedaba adelante (los paquetes se
   descartaban como "tardíos" para siempre). Ahora re-sincroniza tras 8 tardíos consecutivos.
3. `SessionClient::writeAll` fallaba la subida de archivos mayores que el buffer de envío del
   socket (`waitUntilReady` de JUCE usa try-lock contra el hilo lector). Ahora reintenta con sleep.

## Después de la revisión de código

Se corrigieron 2 hallazgos críticos (orden de destrucción en el cliente headless; doble "Crear
sesión" tickeando un servidor liberado) y 11 importantes (ajuste del buffer al tamaño de bloque de
la sesión, registro de sesiones sin lock en el tick y el hilo UDP, ids de cliente aleatorios,
límite de tamaño/anidamiento de líneas de control, escrituras parciales en el servidor, hilo
escritor en el cliente para que la UI no se bloquee durante subidas, hilo de envío UDP despertado
por el callback, lectura sin lock del estado del backing track, validación de la URL de YouTube,
reloj interno del servidor embebido si falla el dispositivo del host, orden de destrucción en la
UI) más 3 menores (ring de 32 slots en el jitter buffer, saneo de nombres de subida y borrado de
la carpeta al morir la sesión, rechazo de hosts IPv6).

Resultado: **145 resultados unitarios, 0 fallos** (3 corridas) y la suite de integración completa:

| Script | Resultado | Latencia ida (ms) |
|---|---|---|
| two-instances | PASS | mediana 15.40, p95 30.88, n 38 |
| embedded-host | PASS | mediana 14.26, p95 24.89, n 39 |
| backing-track | PASS | — (16 clicks del track detectados, `trackPlaying:true`) |
| packet-loss | PASS | mediana 12.99, p95 23.81, n 30 |
| screenshots | PASS | 69 controles verificados + captura real |

La mediana bajó de ≈ 22–25 ms a ≈ 13–15 ms gracias al envío UDP inmediato (antes esperaba el
timeout de 2 ms del `recvfrom`) y al registro de sesiones sin contención con el tick.

## Hardware real: Arturia MiniFuse 2 (driver "MiniFuse ASIO Driver")

La máquina tiene seis drivers ASIO registrados (Helix, HX Stomp, ASIO4ALL, FL Studio, MiniFuse,
X-AIR); el MiniFuse 2 está conectado y es el que se usó. Para probarlo sin UI se agregó al cliente
headless `--asio "<driver>" --mic N --inst N --out P --mute-monitor` (abre el driver real en vez
del dispositivo falso e imprime canales, tamaños de bloque, frecuencias y latencias del driver) y
el script `tests/integration/asio-loopback.ps1`, que aprovecha las salidas LOOPBACK del MiniFuse
(salidas 2/3 reaparecen como entradas 2/3):

```
B (falso, emite clicks) → servidor → A (MiniFuse salida 2/3) → loopback → A (entrada 2/3) → servidor → B (detecta)
```

Resultados (Release, 48 kHz):

| Bloque | Driver: latencia in / out | Ida y vuelta completa (2 saltos + hardware) | Callbacks | Underruns |
|---|---|---|---|---|
| 128 | 200 / 264 muestras (4.2 / 5.5 ms) | mediana 37.5–37.7 ms, p95 58.7, n 39/39 | 5272 en 14 s (= 48000/128) | 0 |
| 64 | 136 / 168 muestras (2.8 / 3.5 ms) | mediana 29.2 ms, p95 30.8, n 39/39 | 10544 en 14 s | 0 |
| 256 | 328 / 408 muestras | (sólo apertura) | OK | 0 |

El driver acepta 8…2048 muestras y 44.1…192 kHz; abre al tamaño pedido (64/128/256) sin pasar
por su panel. Restando la ida medida en loopback con dispositivos falsos (≈ 14–17 ms), el tramo
por hardware (salida + loopback + entrada + un salto más de red) suma ≈ 20 ms a 128 y ≈ 14 ms a
64, consistente con las latencias que el driver declara (9.7 ms y 6.3 ms) más el jitter buffer.

Hallazgos corregidos: (1) con la salida del driver realimentada a su entrada, el monitoreo local
se realimentaba a sí mismo (pico de salida 2.0, señal continua, ningún click detectable) → el
headless mute el monitor con `--mute-monitor`; (2) en la primera apertura el driver arrancó una
vez a 32 muestras (su valor de panel) antes de arrancar a 128, y el motor dejó guardado el error
"bloques de 64, 128 o 256" del arranque fallido aunque el segundo fuera válido → un arranque
soportado limpia el error pendiente (`AudioEngine::audioDeviceAboutToStart`).

GUI real (`Ensayo Virtual.exe` sin argumentos, `settings.json` con el MiniFuse, captura con
PrintWindow en `tests/integration/out/asio-gui/lobby-minifuse.png`): el lobby lista el driver,
muestra "1: MIC/LINE/INST 1", "2: MIC/LINE/INST 2", "Output Left + Output Right", "Latencia ASIO:
4.2 ms entrada / 5.5 ms salida" y la barra "Audio: MiniFuse ASIO Driver @ 48000 Hz, 128 muestras".

Suite completa tras estos cambios: **146 resultados unitarios, 0 fallos**; integración
two-instances 17.14 ms, embedded-host 15.25 ms, backing-track PASS, packet-loss 14.14 ms,
screenshots PASS, asio-loopback 37.67 ms (todas PASS).

## Backing track de YouTube sin instalar nada (revisión del 2026-11-04)

Primera prueba de uso real: "No se encontró yt-dlp.exe". Cambio: la app distribuye `tools\yt-dlp.exe`
(2026.10.26) y `tools\deno.exe` (2.9.8) y deja de necesitar ffmpeg: baja el AAC original (`m4a`,
contenedor DASH fragmentado) y el servidor lo decodifica con un `juce::AudioFormat` propio sobre
Media Foundation (`src/server/MediaFoundationAudioFormat.*`).

- Sonda manual con los binarios: sin `--js-runtimes` yt-dlp avisa "No supported JavaScript runtime"
  y aun así baja el itag 140; con `--js-runtimes deno:<tools\deno.exe>` (ruta Windows, no MSYS)
  imprime "[jsc:deno] Solving JS challenges using deno". El `.m4a` resultante no lo abría JUCE
  (`WindowsMediaAudioFormat` sólo lee mp3/wma/asf) → lector Media Foundation.
- Unitarias: `MediaFoundationAudioFormat` (WAV de dos senos decodificado con error < 1e-3; el m4a
  real: 44100 Hz, 2 canales, 840704 muestras = 19.06 s, pico 0.472; `BackingTrack::load` OK) y
  `YouTubeImporter` (comando sin `-x`, `--js-runtimes` con deno, `findDeno`). **151 resultados, 0 fallos.**
- Integración `youtube-track.ps1`: descarga real con las herramientas embebidas (302 KB, deno
  resolvió el desafío JS), C la sube y reproduce, B recibe `trackPlaying:true` en 13/13 segundos y
  pico de audio 0.471. Suite completa (7 scripts) PASS: two-instances 13.09 ms, embedded-host
  14.16 ms, packet-loss 13.45 ms, asio-loopback 37.46 ms.
- Zip 0.1.1: 72 MB (deno 98 MB y yt-dlp 24 MB sin comprimir).

## Uso real en la ventana (UI Automation, 2026-11-04)

Primer reporte de uso: "Descargar muestra Descargando, pasa a listo y no pasa nada". Para
reproducirlo sin intervención se automatizó la ventana real con UI Automation de Windows
(`AutomationElement` por nombre de botón, `InvokePattern`, SendKeys en los editores, capturas con
PrintWindow): crear sesión con el MiniFuse, pegar la URL, Descargar, esperar, Play, Salir.

Hallazgos y correcciones:

1. **Descarga repetida descartada**: con el video ya en caché yt-dlp imprime "`<archivo> has
   already been downloaded`" y deja el archivo con su fecha vieja; el importador sólo aceptaba
   archivos modificados después de arrancar → "yt-dlp falló (0)" aunque el proceso saliera bien.
   Ahora toma la ruta que yt-dlp imprime (`Destination:` / `has already been downloaded`) y
   recién si no hay ninguna busca el audio más nuevo de la carpeta sin filtrar por fecha.
2. **Link con IP inútil**: `firstLanAddress` devolvía la primera IPv4 no-loopback, que en esta
   máquina es `169.254.206.134` (adaptador sin red). Ahora prefiere direcciones privadas
   (10/8, 172.16/12, 192.168/16), ignora 169.254/16 y cae a cualquier otra ruteable.
3. **Editor VST desde la sesión**: nuevo botón `session.editor` ("Abrir editor VST" / "Cerrar
   editor VST") en la cabecera de la sesión, habilitado cuando hay plugin cargado; abre la
   misma ventana que el botón del lobby. Probado con TONE3000 real: aparece la ventana
   `'TONE3000' 1040x653`, el botón pasa a "Cerrar editor VST" y al cerrarlo desaparece.
4. **Estado tras cargar**: "Listo" se leía como "terminó"; ahora dice "Listo: Play para
   reproducir" (cargar no reproduce; cualquier participante aprieta Play).

Resultado del flujo completo con el build corregido (`tests/integration/out/gui-youtube/`):
sesión creada con link `ensayo://192.168.0.141:22140/…`, descarga del video de 4:09 (en caché),
nombre y duración en pantalla, Play → botón "Pausa" y posición 0:04 / 4:09 avanzando;
`settings.json` persiste nombre, buffer 64, plugin y su estado. Unitarias **153 / 0 fallos**;
integración 7/7 PASS (two-instances 13.63 ms, embedded-host 12.47 ms, packet-loss 11.70 ms,
asio-loopback 37.39 ms).

## Primera sesión real entre dos PCs (Tailscale, 2026-11-08)

Reporte: el amigo se unió (TCP OK) pero el host escuchaba **sólo ruido** de él y él no escuchaba nada ("Sin
audio del servidor"), aunque veía moverse su medidor.

Diagnóstico desde el host: firewall con UDP/TCP 22140 permitidos en todos los perfiles, Tailscale como red
Privada, sin reglas de bloqueo. Reproducción: un participante que se une por TCP (vía `100.116.198.119`, la IP
de Tailscale del host) y **nunca manda UDP** produce en el host un ruido constante de pico 0.10 (≈ -20 dBFS): el
servidor hacía PLC de Opus sobre un decodificador que nunca decodificó nada (ruido de confort). Y como el
servidor aprende el destino UDP de cada cliente del primer datagrama que recibe, al no recibir ninguno nunca le
manda la mezcla. Conclusión: el UDP del amigo no llegaba al host (TCP sí): firewall/antivirus de su PC o algo en
el camino; la app no tenía forma de decirlo.

Cambios:

1. `MixServer`: cada flujo (voz/instrumento) sólo se oculta con PLC si ya decodificó un bloque real y por a lo
   sumo ~50 ms; antes de eso o después, silencio.
2. Contadores por participante en el servidor (`packets` aceptados, `wrongFrame` descartados por tamaño de
   bloque) enviados en `stats`; el cliente cuenta las mezclas recibidas y registra si no pudo abrir su socket UDP.
3. `diagnoseAudio` (`src/ui/AudioDiagnosis.*`) lo traduce a un mensaje por dirección: "El host no recibe tu
   audio: el UDP a host:puerto no llega", "El host recibe tu audio pero el suyo no llega a esta PC", "No llega el
   audio de <nombre>" (en el host), buffer distinto y socket UDP fallido; vuelve a "Audio OK en las dos
   direcciones" cuando se arregla.
4. `tools/udp-test.ps1`: eco UDP sin la app para separar red de aplicación.

Verificación: unitarias **166 / 0 fallos** (nuevos `AudioDiagnosis` y tres de `MixServer`: sin ruido para quien
no manda UDP, flujo que se corta → silencio, datagramas de otro bloque contados); integración nueva
`udp-blocked.ps1` (participante sólo-TCP: el servidor reporta `packets 0` para él y 5152 para el host, que
escucha pico 0); suite completa 8/8 PASS; ventana real del host con un participante sólo-TCP: la barra de estado
muestra "No llega el audio de Amigo: su UDP no alcanza al servidor (firewall/antivirus en su PC)" a los 7 s.
Segunda prueba real con 0.1.3: conectan y suena, pero con cortes, ~6 % de pérdida, "Jitter 20" y "Sin audio del
servidor" intermitente (igual hosteando cualquiera de los dos). Medición desde el host durante la sesión:
`tailscale ping` al amigo arranca por el relay DERP de San Pablo (66 ms) y pasa a directo (`181.2.104.184`)
con 6–9 ms y **picos de 16 y 32 ms**; el host tenía buffer de **64 muestras**. El jitter buffer tenía un tope fijo
de 20 bloques = 27 ms a 64 muestras (53 ms a 128): "Jitter 20" era el buffer clavado en el tope y cada pico de
más de ~27 ms, un corte. Y "Sin audio del servidor" se mostraba cada vez que el buffer se vaciaba un instante.

Cambios: tope del jitter buffer por duración (`maxBufferMs = 80`: 60 bloques a 64, 30 a 128; ring de 128 slots)
y "Sin audio del servidor" sólo tras 2 s sin ninguna mezcla. Test nuevo `6e` que simula ese camino a 64
muestras: con el tope viejo el buffer queda en 21 bloques (28 ms) y pierde 1.56 % tras el arranque; con el nuevo
crece a 44 bloques (58.7 ms) y pierde 0.54 %. Unitarias **169 / 0**; integración headless 6/6 PASS con la
misma latencia en loopback (13–14 ms). Recomendación para internet: buffer 128 (la mitad de paquetes por segundo
por un túnel en espacio de usuario) y cable en las dos puntas.

## Pendiente de verificar con hardware

Con señal real por las entradas: Panel ASIO del driver, VST3 sobre el instrumento (editor,
bypass, estado guardado), monitoreo propio sin latencia escuchado en la salida, y una sesión
real entre dos máquinas (puerto 22140 abierto) midiendo ping y latencia estimada en la barra de
estado.
