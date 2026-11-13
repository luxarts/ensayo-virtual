# Ensayo Virtual — Grabación multipista colaborativa (diseño)

Fecha: 2026-11-13. Estado: aprobado por Lucas (diseño presentado en 5 partes y validado en conversación;
boceto de interfaz en `.nashe/brainstorm/*/content/daw-layout.html`, local).

## 1. Objetivo y cambio de enfoque

Tocar en vivo por internet chocó con la latencia y el jitter de la red (prueba real por Tailscale del
2026-11-08: 7 ms con picos de 32 ms, cortes y ~6 % de pérdida). El programa pasa a ser una **estación de
grabación multipista colaborativa**:

- Te unís a una sesión (link `ensayo://host:puerto/ID`, como hoy), apretás **Grabar** y suena la sesión
  completa; lo que tocás se graba en una pista tuya.
- Después cortás (split), borrás, movés y recortás regiones, como en Reaper.
- Los demás **ven en tiempo real** la grabación (la región crece) y la edición, y pueden reproducirla o grabar
  sus propias pistas.
- Cada integrante puede tener varias pistas; cada pista muestra su autor.
- Interfaz de DAW simplificado: una fila por pista con nombre, autor, mute, solo, volumen y paneo.
- **Prioridad: calidad de audio y cero clics/saltos/crujidos. La latencia no importa.**

Idea central: **la red sale del camino del audio**. Cada participante reproduce desde su copia local de las
tomas y graba en su propia PC; por la red sólo viajan archivos y operaciones de edición, por TCP (sin
pérdidas). Un retraso de red sólo hace que algo llegue más tarde, nunca que suene mal.

## 2. Decisiones (con alternativas descartadas)

| Tema | Elegido | Alternativa | Por qué |
|---|---|---|---|
| Modo en vivo | Se reemplaza la mezcla en vivo por la grabación; queda una **escucha en vivo opcional** de quien está grabando, que nunca afecta lo grabado. | Mantener ambos modos; reemplazarlo sin escucha | Ambos modos duplica UI y pruebas; la escucha opcional da la sensación de "estar juntos" sin tocar la calidad. |
| Dónde vive el proyecto | **PC del host** (servidor embebido, como hoy); cada invitado guarda una copia local en caché. Para colaborar el host debe estar conectado. | Servidor siempre prendido (VPS); ambos | Cero infraestructura; Tailscale ya probado. El mismo `EnsayoServer.exe` podrá alojar proyectos más adelante. |
| Sincronización | **Host con registro de operaciones**: el host valida, ordena (número de versión) y difunde cada operación; todos aplican en el mismo orden. | Réplica entre pares (CRDT); host que reproduce y transmite | CRDT agrega fusión de ediciones concurrentes y conexiones entre todos; transmitir la reproducción vuelve a meter la red en el audio. |
| Permisos | Cada pista la edita **su autor; el host puede editar cualquiera** (como productor). Conflictos autor/host: gana la última operación que aplica el host. | Sólo el autor; todos editan todo | Pedido explícito. |
| Mezcla | **Compartida**: volumen, paneo y mute son del proyecto (los cambia el autor o el host). **Solo** es local de cada uno. | Personal de cada uno; ambas | Todos escuchan la misma mezcla; solo local para aislar sin molestar. |
| Transporte | **Independiente**: cada uno da play/stop/graba sobre su copia; varios pueden grabar a la vez. | Compartido; seguir a otro | Grabar es local y alineado a la línea de tiempo; no hace falta sincronizar transportes. |
| Edición v1 | Cortar, borrar, mover, recortar bordes, **micro-fundidas anti-clic automáticas**. | Deshacer/rehacer, fundidas editables, ajuste a grilla | Fuera de la v1 por decisión de alcance. |
| Extras v1 | Importar audio/YouTube como pista, metrónomo + cuenta de 1 compás, exportar mezcla a WAV 24 bits. | Ajuste a grilla | Alcance. |
| Transferencia de tomas | **Progresiva mientras se graba**: segmentos de 1 s, cada uno un FLAC 24 bits independiente, por TCP. | Subir al final; Opus con pérdida | Sin espera al parar; TCP no pierde datos (sin glitches); FLAC concatena exacto. Opus no se corta en segmentos sin costuras: queda para más adelante si alguien tiene subida lenta. |
| Lectura de tomas al reproducir | Tomas decodificadas **en RAM en bloques de 1 s sólo-agregar** (el hilo de audio lee sin bloqueos aunque la toma siga creciendo); los segmentos FLAC quedan en disco para reabrir. ~46 MB por 4 min de pista mono. | Memoria mapeada + precarga (versión inicial de este diseño); `BufferingAudioReader` por región | Una toma que crece obliga a re-mapear y liberar el mapeo viejo desde el hilo de audio (llamada al sistema); los bloques en RAM no tienen ese problema y un proyecto típico entra holgado. |
| Grabación de instrumento | **Después del VST** (estéreo, efecto aplicado). | Señal limpia + VST al reproducir | No hay efectos por pista en v1. |
| Alineación | Latencia de entrada + salida reportada por el driver **más** desvío medido con "Calibrar latencia" (loopback). | Sólo lo reportado | Hay placas que reportan mal; es la técnica de los DAWs. |
| Escucha en vivo | Paquetes UDP Opus del formato actual (bloques de 128 muestras), **reenviados** por el host a quien la activó; sin mezcla en el servidor. | TCP | Es sólo para oír; si se pierde un paquete, no afecta nada grabado. |

## 3. Arquitectura

```
 Invitado / host (misma app)                                Host (servidor embebido)
 ┌──────────────────────────────────────────┐               ┌─────────────────────────────────┐
 │ DawView (UI)                              │               │ ProjectHost                     │
 │   ↕ ops, estado                            │  TCP control  │   project.json (versión N)      │
 │ ProjectClient ── réplica Project ─────────┼──────────────▶│   valida permisos, ordena,      │
 │   │  cola de subidas (disco)              │  ops/estado   │   guarda atómico, difunde        │
 │   │  caché de tomas (WAV float + picos)   │  TCP datos    │ TakeStore                       │
 │   └─ segmentos FLAC ↑↓ ───────────────────┼──────────────▶│   takes/<id>/<n>.flac           │
 │ AudioEngine (callback ASIO)               │  UDP preview  │ PreviewRelay (UDP, sin mezcla)  │
 │   PlaybackRenderer ← Project inmutable    │◀─────────────▶│                                 │
 │   Recorder → hilo escritor → segmentos    │               └─────────────────────────────────┘
 │   Metrónomo, monitoreo propio 0 ms        │
 └──────────────────────────────────────────┘
```

Unidades (cada una con una responsabilidad y una interfaz que se prueba sola):

| Unidad | Hace | Depende de |
|---|---|---|
| `core/Project` | Modelo (proyecto, pistas, regiones, tomas), JSON, operaciones con validación de permisos e invariantes. Puro. | juce_core |
| `core/Segments` | Codificar/decodificar un segmento FLAC 24 bits; picos min/max. Puro. | juce_audio_formats |
| `core/Render` | Renderizar un bloque de la línea de tiempo desde un `Project` y fuentes de muestras: fundidas, superposiciones, ganancias suavizadas, paneo, solo, metrónomo. Puro, determinista. | core/Project |
| `client/TakeCache` | Por toma: anexa segmentos decodificados a un WAV float, picos, lector mapeado en memoria, precarga. | core/Segments |
| `client/ProjectClient` | Conexiones control + datos, réplica del proyecto, envío de ops, cola persistente de subidas, descargas. | core/Project, TakeCache |
| `client/Recorder` | Captura en el hilo de audio → cola sin bloqueos → hilo escritor: WAV local + segmentos + picos en vivo. | core/Segments |
| `client/AudioEngine` | Callback ASIO: monitoreo propio, reproducción, grabación, metrónomo, escucha en vivo; transporte. | Render, TakeCache, Recorder |
| `client/Offline` | Importar (decodificar + remuestrear sinc a 48 kHz → segmentos) y exportar mezcla (render offline → WAV 24 + TPDF). | Render, Segments |
| `client/LatencyCalibrator` | Click por la salida, detección en la entrada, desvío respecto de lo reportado. | — |
| `server/ProjectHost` | Proyectos en disco, sesiones, control, datos, relé UDP de escucha. | core/Project |
| `ui/DawView` y amigos | Barra superior, transporte, encabezados de pista, línea de tiempo con regiones y formas de onda. | modelo + callbacks |

Se **eliminan**: `server/MixServer`, `server/BackingTrack`, `ui/SessionView`, `ui/AudioDiagnosis` (todo
específico de la mezcla en vivo) y sus pruebas. Se **reutilizan**: sesiones/links/registro del esquema
`ensayo://`, `SessionLink`, `Settings`, `PluginSlot`, `FakeAudioDevice`, `YouTubeImporter`,
`MediaFoundationAudioFormat`, `OpusCodec`, `JitterBuffer`, `SpscQueue`, `Sha1`, lobby.

## 4. Modelo del proyecto

Identidad: cada instalación genera una vez un **userId** (UUID) que se guarda en `settings.json` junto al
nombre. El autor de una pista es un userId; la UI muestra el nombre (el último conocido de ese userId).

`project.json` (host, `%APPDATA%\EnsayoVirtual\projects\<projectId>\`):

```json
{
  "format": 1, "id": "<uuid>", "name": "Tema nuevo", "hostUser": "<uuid>",
  "sampleRate": 48000, "bpm": 92.0, "beatsPerBar": 4, "version": 128,
  "users": [ { "id": "<uuid>", "name": "Lucas" } ],
  "takes": [ { "id": "<uuid>", "author": "<uuid>", "channels": 1, "segments": 37,
               "samples": 1774080, "complete": true } ],
  "tracks": [ { "id": "<uuid>", "name": "Guitarra rítmica", "author": "<uuid>", "kind": "instrument",
                "gainDb": -3.0, "pan": -0.2, "muted": false,
                "regions": [ { "id": "<uuid>", "take": "<uuid>", "start": 192000, "offset": 0, "length": 960000 } ] } ]
}
```

- Tiempos en **muestras a 48 kHz** (int64). `start` = posición en la línea de tiempo; `offset` = desde dónde
  de la toma; `length` = duración. Invariantes: `offset ≥ 0`, `length ≥ 1`, `offset + length ≤ take.samples`
  (para tomas completas; para tomas en curso, ≤ muestras recibidas hasta el momento).
- `kind`: `voice` (mono, canal de micrófono), `instrument` (estéreo, salida del VST), `imported` (1 o 2 canales).
- Orden dentro de la pista: el **orden del array** es el orden de apilado; la última región es la de arriba.
  Mover o recortar una región la pasa al final (queda arriba).
- `gainDb` en [−60, +12]; `pan` en [−1, +1]; `bpm` en [30, 300]; `beatsPerBar` en [1, 12].

### Operaciones

Cada operación es un objeto JSON con `"op"`. El host valida, aplica, incrementa `version` y difunde.

| op | Campos | Permiso |
|---|---|---|
| `add_track` | `track` (id, name, kind) — el autor es quien la manda | cualquiera |
| `rename_track` | `track`, `name` | autor o host |
| `delete_track` | `track` | autor o host |
| `set_mix` | `track`, `gainDb?`, `pan?`, `muted?` | autor o host |
| `add_region` | `track`, `region` (id, take, start, offset, length) | autor de la pista o host; la toma debe existir |
| `split_region` | `track`, `region`, `at` (muestra de línea de tiempo, estrictamente dentro), `newRegion` (id para la mitad derecha) | autor o host |
| `delete_region` | `track`, `region` | autor o host |
| `move_region` | `track`, `region`, `start` (≥ 0) | autor o host |
| `trim_region` | `track`, `region`, `start`, `offset`, `length` (el resultado debe cumplir invariantes y no exceder la toma) | autor o host |
| `set_tempo` | `bpm`, `beatsPerBar` | host |
| `rename_project` | `name` | host |

Una operación inválida o sin permiso se rechaza con un motivo y no cambia la versión.

## 5. Protocolo

Se mantiene el canal de control actual (TCP, un objeto JSON por línea, campo `"t"`). `protocol: 2` en el
saludo; un cliente con otra versión se rechaza con "versión incompatible".

**Control** (cliente → host): `hello {protocol, session, user, name}` (el id del host sólo se acepta desde
127.0.0.1), `op {seq, op}`, `rec_start {take, track, start}`, `rec_peaks {take, from, peaks:[[min,max]...]}`,
`rec_stop {take}`, `take_complete {take}`, `listen {enabled}`, `ping {ms}`. (host → cliente):
`welcome {clientId, token, udpPort, isHost, project}`, `applied {version, by, op}`, `rejected {seq, reason}`,
`presence {users:[{id, name, online, recording?:{take, track, start}}]}`, `rec_start/rec_peaks/rec_stop`
reenviados, `pong {ms}`, `error {message}` (y se cierra la conexión).

**Datos** (segunda conexión TCP, mismo puerto, primera línea `data_hello {clientId, token, have}` con el
token recibido en `welcome` y `have = {take: segmentos que ya tiene, ...}`):

- Subida: línea `segment {take, index, samples, channels, bytes, sha1}` + `bytes` de FLAC. El host verifica
  autor, orden (`index` = segmentos que ya tiene; todos los segmentos menos el último miden exactamente 1 s,
  así que después de uno corto no se acepta otro), SHA-1 y audio; guarda `takes/<take>/<index>.flac`, responde
  `segment_ok {take, index}` y lo reenvía a los demás. Si no, `segment_bad {take, index, expected, reason}`.
- Descarga: al conectarse, el host le envía al cliente los segmentos que no tenga según `have` y después, en
  vivo, cada segmento nuevo que reciba de cualquiera. Si falla el envío (conexión caída), el host corta la
  conexión de datos (el cliente reconecta y vuelve a pedir desde lo que tiene). Si un segmento guardado no se
  puede leer (archivo faltante o dañado), la conexión sigue: el host no le envía nada más de esa toma a ese
  cliente (ni de la descarga inicial ni en vivo, para que nunca falte un índice), le manda
  `take_damaged {take, message}` ("La toma de <autor> está dañada en el anfitrión; no se puede descargar
  completa"), que el cliente muestra como aviso, y sigue con las demás tomas.
- `take_complete {take}` (en la conexión de control) cuando el autor terminó de subir todos los segmentos; el
  host aplica `update_take` con `complete: true` y los `samples`/`segments` que ya tiene.

**Escucha en vivo (UDP)**: el formato de paquete actual (20 bytes de cabecera, Opus custom a 128 muestras;
campo `voice` = mono, `instrument` = estéreo según la pista). El host reenvía cada paquete del que graba a
los clientes con `listen` activado (sin mezclar ni decodificar). El receptor usa el `JitterBuffer` existente.

## 6. Motor de audio

Todo a **48 kHz**; cualquier tamaño de buffer ASIO (recomendado 256). El callback nunca bloquea, no asigna
memoria y no toca disco ni red.

**Reproducción** (`core/Render`): para cada pista y cada región que cruza el bloque, lee muestras de su toma
(`SampleSource::read(take, offset, n, dest)`), aplica:
- **Fundidas de borde**: 3 ms (144 muestras) de medio coseno al inicio y al final de cada región, salvo en una
  **unión continua** (otra región de la misma pista que termina/empieza exactamente ahí, sobre la misma toma y
  con offset contiguo: un corte que dejó las dos mitades suena idéntico al original). Si la región dura menos
  de 2 × 144 muestras, las fundidas se acortan a la mitad de la región.
- **Superposiciones**: la región de arriba reemplaza a las de abajo; en sus bordes, las de abajo se
  atenúan con la curva complementaria (fundido cruzado de 3 ms).
- **Ganancia y paneo** con rampas lineales de 20 ms (por pista, persisten entre bloques); paneo de potencia
  constante (−3 dB al centro); mute = ganancia 0 con la misma rampa; solo local: si hay alguna pista en solo,
  las demás van a 0 con rampa.
- **Transporte**: play/stop/salto con fundido de 5 ms del bus maestro.
- **Metrónomo**: click sintetizado (1 kHz acentuado, 800 Hz resto, 20 ms con envolvente) en la grilla de
  `bpm`/`beatsPerBar`, sólo a la salida local y nunca a la grabación ni a la exportación.
- Las tomas mono se reproducen centradas antes del paneo; estéreo con balance.

**Tomas en caché** (`client/TakeCache`): cada segmento recibido se decodifica y se **anexa** en RAM (bloques
de 48000 muestras por canal, tabla de punteros preasignada para 4 h, publicada con atómicos: el hilo de audio
lee sin bloqueos aunque la toma crezca) y se guarda en disco tal cual en `cache\<proyecto>\takes\<id>\<n>.flac`
(para reabrir sin volver a bajar); se calculan picos (min/max cada 256 muestras). Si una muestra pedida todavía
no llegó (toma en curso), se lee silencio.

**Grabación** (`client/Recorder`): al apretar Grabar con una pista propia armada, el transporte arranca un
compás antes del cursor (cuenta) y desde el cursor se capturan las entradas (pista de voz: canal de mic
mono; instrumento: salida estéreo del VST). El callback copia a una cola sin bloqueos (SPSC); un hilo
escritor arma el WAV local (`recordings\<take>.wav`), corta segmentos de 48000 muestras, los codifica a FLAC
24 bits y los encola para subir (cola persistente en disco), y publica picos cada 100 ms.

**Alineación**: la muestra de entrada capturada en el instante del callback `k` corresponde a lo que sonó en
la línea de tiempo `L = latOut + latIn + calibración` muestras antes. La región se crea con
`start = cursorDeGrabación` y `offset = L` sobre la toma (la toma arranca a capturarse `L` muestras antes del
cursor, durante la cuenta), de modo que la primera muestra de la región es exactamente lo tocado sobre el
cursor. Si `L` supera la cuenta (no debería: un compás a 300 BPM son 9600 muestras), se corre el comienzo.

**Calibrar latencia** (lobby): emite 8 clicks por la salida del par elegido y los busca en la entrada
elegida (que el usuario conecta con un cable o con el loopback de la placa); mediana del retraso medido
menos `latIn + latOut` = calibración (en muestras, se guarda en `settings.json` por dispositivo y buffer).

**Importar**: archivo (wav/flac/ogg/mp3/m4a/mp4/aac) o YouTube (yt-dlp + deno ya embebidos) → decodificar
completo → remuestrear a 48 kHz con `WindowedSincInterpolator` si hace falta → segmentos FLAC → toma
`complete` + `add_track(kind=imported)` + `add_region` en la posición 0.

**Exportar**: render offline con el mismo `core/Render` (sin metrónomo, sin solo, mezcla del proyecto) del
rango [0, fin de la última región] a WAV 24 bits con dither TPDF.

**Escucha en vivo**: mientras grabás, el hilo escritor también codifica bloques de 128 muestras con Opus y
los manda por UDP; el que escucha los suma a su salida tras un `JitterBuffer` propio.

## 7. Interfaz

Lobby (se mantiene) + `lobby.open` "Abrir proyecto…", `lobby.calibrate` "Calibrar latencia", etiqueta con
la calibración actual. "Crear sesión" crea un proyecto nuevo (o abre el elegido) y lo hostea.

Vista de sesión `DawView` (boceto aprobado):
- **Barra superior** (`daw.project`, `daw.link`, `daw.copy`, `daw.users`, `daw.listen`, `daw.editor`,
  `daw.leave`).
- **Transporte** (`transport.home`, `transport.play`, `transport.stop`, `transport.record`, `transport.time`,
  `transport.bpm`, `transport.meter`, `transport.click`, `transport.countin`, `edit.split`, `edit.delete`,
  `daw.export`).
- **Pistas**: por fila `track.N.color`, `track.N.name` (doble click renombra), `track.N.author`,
  `track.N.kind`, `track.N.mute`, `track.N.solo`, `track.N.arm` (sólo pistas propias; en ajenas, candado
  `track.N.lock`), `track.N.volume` (+ etiqueta dB), `track.N.pan`.
- **Línea de tiempo** (`timeline`): regla de compases, cursor, regiones con forma de onda (picos), región en
  grabación que crece en rojo, región con descarga pendiente en gris con %. Click selecciona; arrastrar el
  centro mueve; arrastrar a <6 px de un borde recorta; S corta en el cursor; Supr borra; rueda + Ctrl hace
  zoom; rueda desplaza.
- **Agregar**: `tracks.addVoice`, `tracks.addInstrument`, `tracks.import`, `tracks.youtube`.
- **Estado** (`status.text`, `status.sync`, `status.ping`, `status.transfers`, `status.device`, `status.cpu`).

## 8. Errores

- Caída del invitado mientras graba: la grabación sigue local; los segmentos quedan en la cola en disco y se
  suben al reconectar (reconexión manual con "Reintentar", reenvío automático de la cola).
- Caída del host: los invitados quedan en **sólo lectura** (reproducción permitida) hasta reconectar.
- Cierre del host: `project.json` se escribe atómico (temporal + renombrar) tras cada operación; tomas en
  curso quedan `complete:false` con lo recibido y se completan cuando el autor vuelve.
- Salto de versión en un cliente → pide el proyecto completo (`resync`).
- SHA-1 incorrecto en un segmento → se rechaza y el autor lo reenvía.
- Disco lleno o error de dispositivo grabando → se detiene y se conserva lo grabado, con mensaje.
- Importación ilegible → mensaje; no se crea la pista.

## 9. Pruebas

- Unitarias: operaciones (permisos, invariantes, orden), JSON ida y vuelta, segmentos FLAC (concatenar ==
  original, bit a bit en 24 bits), render (**detector de clics**: sobre un seno, ninguna diferencia entre
  muestras consecutivas supera la del seno + 1e-3 en cortes, recortes, movimientos, superposiciones, cambios
  de ganancia/paneo/mute, play/stop), alineación (un click en el dispositivo falso con latencias conocidas cae
  en ±1 muestra), metrónomo en grilla, exportación == render en tiempo real, importación con remuestreo.
- Integración (procesos reales, headless): host + 2 clientes; B graba clicks sobre la reproducción, A recibe
  los segmentos a medida que se graban y la toma completa, la región queda alineada ±1 muestra; ediciones
  de B llegan iguales a A y C (mismo hash del proyecto); edición sin permiso rechazada; corte de red a mitad
  de grabación y reanudación; reapertura del proyecto por el host; subida limitada que se encola y se vacía.
- Hardware: calibración con el loopback del MiniFuse y toma alineada ±1 ms.
- UI: capturas + verificación de todos los controles nombrados; automatización de la ventana real (crear
  pista, grabar, cortar, mover).

## 10. Fuera de alcance (v1)

Deshacer/rehacer, fundidas editables, ajuste a grilla, efectos por pista, automatización, MIDI, punch-in,
grabación por tomas en loop, servidor siempre prendido para proyectos, Opus para subidas lentas, reproducción
con el host apagado de proyectos que nunca se bajaron.
