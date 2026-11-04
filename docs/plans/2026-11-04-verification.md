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
proceso: mediana 23–28 ms), Ui (lobby 20 controles, sesión 49), Screenshot.

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

## Pendiente de verificar con hardware

Con una interfaz ASIO real: apertura del driver, canales, Panel ASIO, VST3 sobre el instrumento
(editor, bypass, estado guardado), monitoreo propio sin latencia, y una sesión real entre dos
máquinas (puerto 22140 abierto) midiendo ping y latencia estimada en la barra de estado.
