# Ensayo Virtual — Diseño

Fecha: 2026-11-04
Estado: aprobado (sesión autónoma; el spec y el plan se dan por aprobados por instrucción del usuario)

## 1. Objetivo

Aplicación de escritorio para Windows que permite a una banda (2 a 6 músicos) ensayar en
tiempo real por internet con la menor latencia posible, al estilo de Jamulus pero escrita desde
cero. Cada músico entra con nombre y rol, tiene dos entradas de audio (micrófono e instrumento),
puede cargar un VST3 sobre el instrumento, usa drivers ASIO, controla el volumen de cada
integrante (voz e instrumento por separado), su propio monitoreo y un backing track (MP3 o audio
de YouTube) que suena sincronizado para todos.

## 2. Investigación previa y decisiones

Cada decisión lista la opción elegida y las alternativas evaluadas, con pros y contras.
Fuentes principales: código fuente de Jamulus (`src/client.cpp`, `server.cpp`, `buffer.cpp`,
`protocol.cpp`, `global.h`), wiki de Jamulus, JackTrip (`Regulator.cpp`, protocolo), SonoBus/AOO,
documentación de libopus 1.6, JUCE 8.0.17, documentación de Windows (MMCSS, waitable timers,
qWAVE), yt-dlp wiki (EJS), RFC 6716, papers de Adriaensen (DLL / adaptive resampling).

### 2.1 Cómo logra Jamulus latencia baja

- Todo corre a 48 kHz con bloques de 64 o 128 muestras (1.33 / 2.67 ms).
- Codec Opus en **modo custom** (CELT puro, `opus_custom_mode_create(48000, 64|128)`), CBR,
  `OPUS_APPLICATION_RESTRICTED_LOWDELAY`, complejidad 1, sin FEC. Un paquete UDP por bloque.
- Jitter buffer: anillo de bloques indexado por número de secuencia (1 byte). Tamaño automático
  por simulación de buffers "sombra" (2..11 bloques) eligiendo el menor con tasa de error
  < 0.025 %, filtrado IIR asimétrico (sube rápido, baja lento) con histéresis.
- Sin resampling: la deriva de reloj entre clientes se absorbe con el deslizamiento de la ventana
  del jitter buffer y el PLC de Opus (`opus_custom_decode(nullptr)`).
- Servidor con timer de alta precisión que cada tick decodifica todos los canales, arma **una
  mezcla personalizada por cliente** (ganancias que cada cliente envía) y la codifica.
- Presupuesto típico: ping + 1 bloque + 0.7·(bufferCliente + bufferServidor)·bloque + latencia
  ASIO (≈3 bloques) + medio frame de Opus → ≈ 25–35 ms con ping de 15 ms. Verde ≤ 43 ms.
- Uno se escucha a sí mismo **a través del servidor** (ida y vuelta completa).

### 2.2 ¿Hay algo mejor?

| Herramienta | Topología | Codec | Qué hace mejor | Qué hace peor |
|---|---|---|---|---|
| JackTrip | hub o P2P | PCM sin comprimir | 0 ms de codec; Regulator con PLC Burg-LPC | 768 kbit/s por stream mono; NAT estricto |
| SonoBus / AOO | malla P2P | Opus u PCM | sin salto por servidor; DLL + resampling para deriva | subida ×(N−1); falla con NAT simétrico |
| FarPlay | P2P | PCM | muy simple | sin solución para NAT |
| Elk LIVE | hardware dedicado | PCM 24 bit | sub-ms de I/O | requiere hardware propietario |
| NINJAM | servidor, por intervalos | Vorbis | inmune a latencia | no es tiempo real |

Conclusión: la malla P2P (SonoBus) da la menor latencia (evita el salto al servidor) pero exige
NAT traversal (ICE/STUN, relay de respaldo), N jitter buffers y corrección de deriva por par.
Para "crear sesión y compartir un link" que funcione detrás de cualquier router, el modelo de
servidor con mezcla personalizada sigue siendo la opción más robusta y simple; la mejora real
sobre Jamulus está en (a) monitoreo local propio sin latencia, (b) backing track inyectado en
el servidor, (c) timer del servidor con deadlines absolutos de alta resolución y (d) servidor
embebido en el cliente que crea la sesión, manejado por el reloj ASIO del host.

Caso de dos personas (el uso habitual): como el servidor va embebido en el cliente que crea la
sesión, no hay salto de red extra respecto de P2P. El audio del invitado viaja directo a la
máquina del host (se mezcla en proceso y sale por su placa) y el del host sale directo al
invitado: un salto de red por sentido, igual que P2P. Lo único que agrega el servidor es su
jitter buffer (2–4 bloques), que P2P no evita porque el receptor necesita uno igual. P2P sólo
gana con 3+ participantes (los invitados entre sí pasan hoy por el host), y a cambio exige
ICE/STUN para atravesar routers domésticos y un relay TURN —otro servidor en el medio— cuando
el NAT no se deja perforar (CGNAT). Para dos personas no aporta latencia; lo que la define es el
RTT de internet entre ambos y quién hostea (el host tiene 0 ms de red para su propio audio).

### 2.3 Decisiones

| Tema | Elegido | Alternativa descartada | Por qué |
|---|---|---|---|
| Topología | Servidor con mezcla personalizada por cliente (estrella). El servidor corre embebido en el cliente que crea la sesión **o** como binario headless (`EnsayoServer.exe`) para un VPS. | Malla P2P (SonoBus) con libjuice/ICE | P2P ahorra ≈ un salto (5–15 ms) pero necesita rendezvous + hole punching + relay para NAT simétrico (~70 % de éxito), N buffers y N correcciones de deriva. Fuera de alcance v1; documentado como mejora futura. |
| Codec | libopus 1.6.1, **modos custom** 48 kHz con frame = bloque ASIO (64 o 128), CBR, `RESTRICTED_LOWDELAY`, complejidad 1–3, `PACKET_LOSS_PERC` 20 | Opus estándar con frames de 120 muestras (2.5 ms) | Con el frame igual al bloque ASIO no hay re-empaquetado ni FIFO extra: un callback = un paquete. El modo estándar es interoperable y de mejor calidad por bit, pero 120 ≠ 128 obliga a un FIFO y suma hasta un frame de latencia. PCM crudo (JackTrip) queda como opción futura para LAN. |
| Canales por músico | Dos streams mono (voz, instrumento) en un mismo datagrama; el rol decide cuáles van. Instrumento mono en red (post-VST se suma L+R), estéreo en el monitoreo local. | Un stream estéreo (L = voz, R = instrumento) | Dos mono permiten al servidor aplicar ganancia por voz e instrumento sin acoplar señales no relacionadas en el codificador estéreo. |
| Monitoreo propio | Local, dentro del callback ASIO: voz + instrumento (post-VST) con faders propios, mezclados con la mezcla que vuelve del servidor. El servidor **excluye** al cliente de su propia mezcla. | Como Jamulus (escucharse vía servidor) | Latencia cero para uno mismo; tocar se siente natural. |
| Jitter buffer | Anillo indexado por secuencia (estilo Jamulus) con tamaño automático por estadística de jitter de llegada (media + 3σ + margen, estilo JackTrip Regulator), 2..20 bloques, inicial 4, histéresis, sube rápido / baja lento. PLC de Opus en huecos. | Buffers sombra de Jamulus; DLL + resampler (AOO) | La estadística de llegada es más simple de testear que 10 buffers sombra. Resampling adaptativo da mejor calidad ante deriva pero agrega un resampler fraccional y un lazo de control; la deriva típica (≤ 50 ppm) produce un bloque perdido cada ~30 s, oculto por PLC. Queda documentado como mejora. |
| Reloj del servidor | Embebido: lo maneja el callback ASIO del host. Headless: hilo MMCSS "Pro Audio" con `CreateWaitableTimerExW(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)` y deadlines absolutos (sin deriva). | Servidor manejado por llegada de paquetes | Jamulus usa timer; el servidor dirigido por paquetes elimina el jitter del timer pero vuelve ambiguo el reloj maestro. El reloj ASIO del host es el mejor reloj disponible. |
| Control | TCP (JUCE `StreamingSocket`) para crear/unirse, lista de participantes, faders, transporte del backing track y subida de archivos. UDP para audio. | Todo por UDP con ACKs propios (Jamulus) | TCP da fiabilidad y orden sin reimplementarlos; la latencia de control no es crítica. |
| Backing track | El servidor decodifica el archivo y lo inyecta como un canal más en la mezcla de cada cliente, con ganancia por cliente. Transporte (play/pause/seek) centralizado. | (b) Cada cliente reproduce su copia con sincronía de reloj; (c) un cliente lo manda por su entrada | (a) es la única donde todos (incluido quien lo cargó) escuchan el track alineado con la banda; sin sincronía de relojes. (b) deja el track desalineado respecto de los demás; (c) depende de un cliente y pasa por su codec. |
| YouTube | El cliente que carga pega la URL; la app ejecuta `yt-dlp.exe` instalado por el usuario (con `deno` y `ffmpeg` en el PATH o ruta configurada) con `-x --audio-format mp3`, y sube el MP3 al servidor. Se muestra aviso de términos de uso. | Reproductor IFrame embebido sincronizado por comandos | El IFrame no entra en la mezcla ASIO, su precisión es de cientos de ms y sus términos prohíben uso solo-audio. yt-dlp no se distribuye con la app. |
| Formatos | WAV, FLAC, OGG, MP3 (`JUCE_USE_MP3AUDIOFORMAT=1`). m4a/webm no; yt-dlp convierte a MP3. | Lector Media Foundation propio | Fuera de alcance. |
| Link de sesión | `ensayo://host:puerto/ID`. Registro del esquema en `HKCU\Software\Classes\ensayo` al primer arranque; segunda instancia reenviada vía `anotherInstanceStarted`. También campo de texto para pegar. | Solo copiar/pegar | Ambos: el link clickeable es la UX pedida; el campo cubre teléfonos y navegadores que bloquean el handler. |
| Framework | C++20, JUCE 8.0.17-security (ASIO y VST3 SDK incluidos), CMake, MSVC 2022 | JUCE 9.0.5 | El proyecto de referencia (`discord-share-guitar`) está en JUCE 8; sus `PluginSlot`, `AsioDeviceManager` y patrones se reutilizan sin adaptar a los breaking changes de JUCE 9. |
| Red en Windows | Winsock crudo: hilo dedicado con `recvfrom` bloqueante, `SIO_UDP_CONNRESET` off, MMCSS "Pro Audio". DSCP vía qWAVE queda como mejora. | JUCE `DatagramSocket` | `DatagramSocket::read` hace `ioctlsocket` + try-lock + `select` por llamada; un `recvfrom` bloqueante despierta al llegar el paquete. |

### 2.4 Presupuesto de latencia objetivo (ida, de un músico a otro)

Bloque 128 @ 48 kHz, ping 10 ms al servidor:

| Etapa | ms |
|---|---|
| Captura ASIO (≈1 bloque + latencia de entrada del driver) | 2.7 + ~1.5 |
| Codec Opus custom (≈ medio frame + overlap) | ~1.5 |
| Red cliente→servidor (ping/2) | 5 |
| Jitter buffer del servidor (4 bloques · 0.7) | 7.5 |
| Tick del servidor (≤ 1 bloque) | 2.7 |
| Red servidor→cliente | 5 |
| Jitter buffer del cliente (4 bloques · 0.7) | 7.5 |
| Salida ASIO (≈1 bloque + latencia de salida) | 2.7 + ~1.5 |
| **Total** | **≈ 38 ms** (≈ 30 ms con bloques de 64) |

Uno mismo: 0 ms (monitoreo local). El backing track llega con la misma latencia que los demás
músicos, por lo que la banda queda alineada entre sí y con el track en todos los oyentes.

## 3. Arquitectura

```
                 ┌────────────────────── cliente A (EnsayoVirtual.exe) ──────────────────────┐
ASIO in mic ─────┤► voz ──────────────┬─► monitor (fader voz) ─────────┐                      │
ASIO in instr ───┤► VST3 ─► instr L/R ┼─► monitor (fader instr) ───────┼─► ASIO out L/R       │
                 │                    └─► L+R mono ─┐                  │                      │
                 │      voz mono ───────────────────┼─► Opus x2 ─► UDP ─┼─► servidor           │
                 │      UDP ◄── mezcla estéreo Opus ◄── jitter buffer ──┘                      │
                 │      TCP ◄──► control (unirse, participantes, faders, transporte, upload)   │
                 └────────────────────────────────────────────────────────────────────────────┘

 servidor (embebido en el host o EnsayoServer.exe)
   por tick (= 1 bloque): decodificar voz/instr de cada cliente desde su jitter buffer,
   avanzar backing track, y para cada cliente c:
       mix_c = Σ_{p≠c} g_c[p].voz·voz_p + g_c[p].instr·instr_p  (centrados)  +  g_c.track·track(L/R)
       codificar estéreo → UDP a c
```

### 3.1 Componentes (unidades aisladas)

| Unidad | Hace | Usa | Lo usa |
|---|---|---|---|
| `Protocol` | Serializa/parsea paquetes UDP de audio y mensajes TCP de control (JSON por línea). Puro. | juce_core | todo |
| `OpusCodec` | `OpusEncoderMono/Stereo`, `OpusDecoder…` sobre modos custom; PLC. | libopus | cliente, servidor |
| `JitterBuffer` | Anillo indexado por secuencia, tamaño automático, estadísticas. Puro, tiempo real. | — | cliente, servidor |
| `BackingTrack` | Carga archivo → buffer 48 kHz estéreo en memoria (resamplea si hace falta); transporte: play/pause/stop/seek/loop; `render(numSamples)`. | juce_audio_formats | servidor |
| `MixServer` | Estado de una sesión: participantes, ganancias por cliente, jitter buffers, `tick()` que produce un paquete por cliente. No sabe de sockets. | Opus, JitterBuffer, BackingTrack | `ServerHost` |
| `ServerHost` | Sockets del servidor: TCP de control (crear/unirse/mensajes/upload), UDP de audio, mapa sesión↔clientes, invoca `MixServer::tick()` desde un `Clock` (timer o callback externo). | MixServer, Winsock | `EnsayoServer` main, `AudioEngine` (host embebido) |
| `AudioEngine` | Callback ASIO: entradas, `PluginSlot`, monitor local, encode/send, receive/decode/mix. Dispositivo ASIO o `FakeAudioDevice`. | OpusCodec, JitterBuffer, PluginSlot, SessionClient | UI, cliente headless |
| `PluginSlot` | Un VST3 sobre el instrumento (portado del proyecto de referencia). | juce_audio_processors | AudioEngine |
| `SessionClient` | TCP de control del cliente: crear/unirse, estado de sesión (participantes, transporte), enviar faders, subir archivo; socket UDP de audio (send/recv con hilo). Emite eventos al hilo de mensajes. | Protocol, Winsock | AudioEngine, UI |
| `SessionLink` | Parsear/formatear `ensayo://host:puerto/ID`; registro del esquema; reenvío de segunda instancia. | juce_core, registro Windows | UI, Main |
| `YouTubeImporter` | Localiza `yt-dlp.exe`, ejecuta la descarga a MP3 en un hilo, reporta progreso/errores. | juce_core (ChildProcess) | UI |
| `Settings` | JSON en `%APPDATA%\EnsayoVirtual\settings.json`. | juce_core | UI |
| `MainComponent` | UI única: lobby (nombre, rol, crear/unirse, audio) y sesión (link, participantes, mezclador, backing track, estado). `--screenshot` y `--demo`. | todo | Main |
| `HeadlessClient` | `EnsayoVirtual.exe --headless …`: cliente sin UI con `FakeAudioDevice`, señal de prueba y detector de latencia. | AudioEngine, SessionClient | pruebas de integración |

### 3.2 Protocolo

**UDP — paquete de audio cliente→servidor** (little-endian):

```
u8  magic = 0xEA   u8 version = 1   u8 flags (bit0 voz, bit1 instr, bit2 mezcla)   u8 frameCode (0 = 64, 1 = 128)
u16 seq            u32 clientId     u32 sampleTime (contador de muestras del emisor)
u32 trackPosition (muestras; 0 en cliente→servidor)
u8  lenVoz   u8 lenInstr   bytes voz   bytes instr          (encabezado fijo de 20 bytes)
```

**UDP — servidor→cliente**: mismo encabezado con `flags` bit2 = mezcla estéreo, `clientId` = 0,
`lenVoz` = largo del payload estéreo, `lenInstr` = 0 y `trackPosition` con la posición del
backing track (para la UI).

Tamaños CBR por frame (mono): 64 → 24 B, 128 → 48 B; estéreo: 64 → 48 B, 128 → 96 B
(≈ 144–288 kbit/s por stream). El cliente aprende `clientId` al unirse por TCP; el servidor
asocia el `clientId` al `addr:port` del primer datagrama válido (NAT keep-alive por tráfico
continuo; el servidor responde a la dirección observada).

**TCP — control**: una línea JSON por mensaje, campo `"t"` = tipo.

| Dirección | Tipo | Campos |
|---|---|---|
| C→S | `create` | `name`, `role` | 
| C→S | `join` | `session`, `name`, `role` (`voice` \| `instrument` \| `both`) |
| S→C | `joined` | `session`, `clientId`, `udpPort`, `frameSize`, `participants[]` |
| S→C | `error` | `message` |
| S→C | `participants` | `[{id, name, role}]` (cada cambio) |
| C→S | `gain` | `target` (clientId o `"track"`), `voice` (dB), `instrument` (dB), `mute` |
| C→S | `track_upload_begin` | `name`, `size`, `sha1` → luego bytes crudos de `size`; S→C `track_ready` {`name`, `lengthSamples`} o `error` |
| C→S | `transport` | `cmd` (`play` \| `pause` \| `stop` \| `seek` \| `loop`), `position` (muestras), `loop` (bool) |
| S→C | `transport_state` | `playing`, `position`, `loop`, `name`, `lengthSamples` (cada cambio y cada 1 s) |
| C↔S | `ping` / `pong` | `t0` (ms del cliente) → RTT |
| C→S | `jitter` | `clientBlocks` (lo que el cliente usa; informativo) |
| S→C | `stats` | por participante: `jitterBlocks`, `loss`; cada 1 s |

La sesión muere cuando se va el último participante. ID de sesión: 6 caracteres `[A-Z2-9]`.
Un servidor headless aloja muchas sesiones en un mismo puerto UDP; el embebido, una.

### 3.3 Motor de audio del cliente (callback ASIO)

1. Leer `voz = in[micCh]`, `instr = in[instrCh]`. Instrumento → `PluginSlot::process` (mono→estéreo).
2. Monitoreo local: `out = monVoz·voz (centrado) + monInstr·instr(L/R)`.
3. Según rol, codificar voz (mono) e instrumento (L+R)/2 (mono) y pushear el datagrama a una
   cola SPSC; el hilo de red lo envía (el callback nunca hace syscalls).
4. Sacar un bloque del jitter buffer de recepción (llenado por el hilo UDP), decodificar
   (o PLC) y sumar `out += recvGain·mezcla`.
5. Publicar picos y contadores vía atomics.

Los bloques de 256 muestras se parten en dos frames de 128. Parámetros compartidos con el hilo de
audio son `std::atomic`. Swap de plugin con `SpinLock` try-lock como en el proyecto de referencia.

`FakeAudioDevice` (`juce::AudioIODevice`) corre un hilo que llama al callback cada bloque con un
waitable timer de alta resolución; su entrada se alimenta de un `TestSignal` (seno, click
periódico o silencio) y su salida va a un `OutputProbe` (detector de click) — se usa en el
cliente headless y en pruebas.

### 3.4 Servidor

`MixServer::tick()` corre una vez por bloque, en el hilo que lo maneje (callback ASIO del host
o hilo de timer). Por participante p: `jitter[p].pop()` → decode voz/instr (PLC si falta).
Backing track: `track.render(block)`. Por cliente c: mezcla según `gains[c]` (excluyendo c),
codificar estéreo, emitir vía `std::function<void(clientId, bytes)>`. Nuevos participantes
entran con fade-in de 200 ms. Los jitter buffers del servidor adaptan su tamaño igual que en el
cliente. `MixServer` no toca sockets ni hilos: se testea en proceso con `tick()` manual.

`ServerHost` hilos: aceptación TCP + un hilo por conexión (lectura de líneas), un hilo UDP
(`recvfrom` bloqueante → `MixServer::push`), y el hilo del reloj (solo headless). Las mutaciones
de estado (join/leave/gains/transport) entran a `MixServer` bajo un `SpinLock` que `tick()` toma
con try-lock, cediendo un tick si hay contención.

### 3.5 Backing track

`BackingTrack::load(file)` decodifica todo a memoria (48 kHz estéreo float; `LagrangeInterpolator`
si el archivo no está a 48 kHz) en el hilo de mensajes/trabajo; el swap al transporte es atómico.
Límite: 30 minutos (≈ 690 MB float; el servidor rechaza más). Upload: tras `track_upload_begin`
el cliente manda los bytes por el mismo socket TCP; el servidor guarda en `%TEMP%\EnsayoVirtual\`,
carga y responde `track_ready`. Cualquier participante puede cargar y controlar el transporte.
El `transport_state` se difunde a todos; la UI muestra nombre, posición y estado.

`YouTubeImporter`: busca `yt-dlp.exe` en la ruta configurada, junto al ejecutable o en el PATH;
ejecuta `yt-dlp -x --audio-format mp3 --audio-quality 0 -o "<tmp>\%(title)s.%(ext)s" <url>` con
`ChildProcess`, muestra progreso parseando la salida y, al terminar, dispara la subida. Si falta
`yt-dlp`, la UI explica cómo instalarlo (yt-dlp + deno + ffmpeg) y recuerda los términos de uso.

### 3.6 UI (JUCE, en español, una ventana, 1100×760 mínimo)

**Lobby**
- Nombre (TextEditor), Rol (ComboBox: Voz / Instrumento / Instrumento + Voz).
- Botón **Crear sesión** (servidor embebido en puerto configurable, default 22140) y campo
  **Link de sesión** + botón **Unirse**.
- Panel **Audio**: dispositivo ASIO, frecuencia, buffer (64/128/256), botón **Panel ASIO**, canal
  micrófono, canal instrumento, par de salida, latencia reportada.
- Panel **VST (instrumento)**: lista de `.vst3`, **Examinar…**, **Abrir editor**, **Bypass**, **Quitar**.
- Barra de estado.

**Sesión**
- Link con botón **Copiar link**; botón **Salir**.
- Franja **Yo (monitoreo)**: fader Voz + Mute, fader Instrumento + Mute, medidores.
- Por participante remoto: nombre, rol, fader Voz + Mute, fader Instrumento + Mute (deshabilitados
  los que su rol no envía), medidor de la señal recibida (del `stats`).
- **Backing track**: **Cargar archivo…**, campo URL de YouTube + **Descargar**, nombre del track,
  **Play/Pausa**, **Stop**, **Loop** (toggle), slider de posición, tiempo `mm:ss / mm:ss`, fader
  volumen + Mute (propio), barra de progreso de descarga/subida.
- Barra de estado: ping, bloques de jitter (cliente/servidor), latencia estimada total,
  pérdida de paquetes, CPU, errores + **Reintentar**.

Faders en dB (−60..+12, doble clic = 0 dB). Todos los controles tienen `setName` para que las
capturas automáticas puedan verificarse por nombre.

Modos de línea de comandos:
- `--screenshot <dir>`: renderiza lobby y sesión (con participantes de demostración) a PNG vía
  `createComponentSnapshot` y escribe `<dir>\controls.json` con nombre, tipo, bounds y visibilidad
  de cada control; luego sale.
- `--demo`: abre la ventana en estado de sesión con participantes falsos (para capturas reales).
- `--headless --join <link> --name N --role R --fake-audio <signal> [--probe <log>] [--duration s]`.
- `ensayo://…` como argumento: unirse directo.

### 3.7 Manejo de errores

- ASIO no abre / se cae: estado con error y **Reintentar**; la sesión se mantiene (se envía silencio).
- Plugin falla: slot vacío, instrumento pasa limpio, plugin anterior se conserva.
- Sin red / servidor caído: TCP cerrado → UI vuelve al lobby con mensaje; UDP sin paquetes 3 s →
  "sin audio del servidor".
- Paquete malformado: se descarta y se cuenta.
- Link inválido: mensaje; nunca se ejecuta nada del link más allá de parsear host/puerto/ID.
- Upload: tamaño/sha1 inválidos → `error`, archivo temporal borrado.
- yt-dlp ausente o falla: mensaje con la salida del proceso.
- Puerto en uso al crear sesión: mensaje con sugerencia de otro puerto.

## 4. Pruebas

### 4.1 Unitarias (`EnsayoTests.exe`, JUCE UnitTest, categoría "EnsayoVirtual")

- `Protocol`: round-trip de paquetes UDP, rechazo de magic/versión/largos inválidos; mensajes JSON.
- `OpusCodec`: encode/decode de seno con SNR > 20 dB; tamaño CBR exacto; PLC no explota.
- `JitterBuffer`: orden, pérdida, duplicados, tardíos, auto-tamaño sube con jitter y baja al
  estabilizarse, histéresis, underrun/overrun contados.
- `BackingTrack`: carga WAV 44.1 k → 48 k, play/pause/seek/loop, render exacto.
- `MixServer`: 3 participantes en proceso, ganancias por cliente, exclusión propia, fade-in, track.
- `AudioEngine` con `FakeAudioDevice`: monitoreo local aplica faders; con rol voz no codifica instr.
- `SessionLink`: parseo/formateo, casos inválidos.
- `Settings`: round-trip, defaults.
- `MixMath` (mezcla pura): ganancias dB, mute, centrado, clipping.

### 4.2 Integración (scripts PowerShell en `tests/integration/`)

1. **Dos instancias**: lanzar `EnsayoServer.exe --port 22150`, cliente A headless (`--fake-audio
   click`, rol `both`), cliente B headless (`--fake-audio silence --probe B.log`). Afirmar que B
   recibe `participants` con A, que B detecta ≥ 10 clicks y reporta latencia ida (QPC emisión en A
   → detección en B; mismo reloj por ser la misma máquina) con mediana < 40 ms a 128 muestras.
2. **Servidor embebido**: cliente A headless con `--host` (crea sesión) + B se une al link impreso.
3. **Backing track**: A sube `tests/fixtures/click.wav` y manda `play`; B detecta los clicks del
   track y recibe `transport_state` con `playing=true`.
4. **Pérdida de paquetes**: `--simulate-loss 5` en B: sin crash, PLC, estadísticas reportadas.
5. **Capturas**: `EnsayoVirtual.exe --screenshot out\` → PNGs + `controls.json`; el script verifica
   que exista cada control esperado (lista en `tests/integration/expected-controls.json`), que sea
   visible y tenga tamaño > 0. Además captura real de la ventana con `--demo` (PowerShell +
   `System.Drawing`) para inspección visual.

### 4.3 Manual (hardware)

Con la interfaz ASIO real: entrar a una sesión, escuchar monitoreo propio sin latencia, ping y
latencia estimada en la barra de estado, VST sobre el instrumento, backing track desde MP3 y
desde YouTube.

## 5. Fuera de alcance (v1)

Malla P2P/ICE, relay TURN, resampling adaptativo por deriva, PCM crudo, pan por participante,
grabación, chat, lista pública de servidores, instalador, macOS/Linux, VST2, cadenas de plugins,
cifrado, salas con contraseña, video.
