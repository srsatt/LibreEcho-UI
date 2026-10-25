# Home Assistant ESPHome voice mode (0.14)

Home Assistant mode keeps LibreEcho's local audio front end and wake detector.
Home Assistant owns the STT, conversation/intent and TTS pipeline after wake:

```text
Echo microphones -> calibration / AEC / beam selection
                 -> local openWakeWord
                 -> ESPHome native API (Noise, TCP 6053)
                 -> Home Assistant Assist
                    -> selected STT / conversation agent / TTS
                 -> local audiod speaker bus
```

The satellite is `libreecho-esphomed`, implemented in C99 for ARM32; it does not
require the Python Linux Voice Assistant runtime. The Wyoming satellite daemon
is removed for 0.14. Wyoming Whisper and Piper **clients** remain supported in
Custom mode, independently of Home Assistant mode.

## Home Assistant setup

Enable **Home Assistant** on the Integrations page. Add the device through Home
Assistant's **ESPHome** integration, using the device's LAN address and TCP port
`6053` if discovery is unavailable. Select an Assist pipeline in Home Assistant.
Local STT, local assistant dispatch and local TTS stay stopped while HA owns
voice. `waked`, `micd`, `audiod`, calibration and AEC remain device-side owners.
Only final post-AEC mono 16-bit PCM at 16 kHz is sent upstream; raw microphone
lanes are not exposed.

Disabling HA restores the previously selected **Local** or **Custom** mode and
its settings. Custom Whisper/Piper addresses, model/voice choices and assistant
configuration are not erased. There is no protocol selector, legacy satellite
banner, or staged compatibility state. Integration bit 1 remains HA-enabled;
missing/old `ha_protocol` values normalize to `esphome`.

## Protocol contract

The reviewed interoperability baseline is Home Assistant **2026.9.4**, which
pins **aioesphomeapi 46.2.0**, and ESPHome schema commit
`fb65096e`. Native API version is **1.14**. Voice-assistant feature flags are
**61**: the `SPEAKER` bit (2) is deliberately omitted because it selects streamed
TTS PCM instead of URL playback. The connection uses Noise; there is no legacy
AuthRequest gate. Conversation continuation metadata is handled at `INTENT_END`,
not `RUN_END`.

URL playback can opt into progressive PCM WAV decoding with `STREAM_WAV=1` in
`/etc/default/libreecho-esphomed` (or `--stream-wav` on the daemon). The default
still downloads a complete response before playback. With the flag, PCM16 WAV
at 8–48 kHz, mono or stereo, starts after 250 ms of audio is buffered (or the
entire response for shorter clips); HTTP `Content-Length`, chunked and EOF
framing are accepted. A 64 KiB PCM buffer bounds audio buffering regardless of response
length. MP3 URLs continue through the finite 2 MiB download path. The flag does
not advertise the ESPHome `SPEAKER` capability or change the HA media URL
contract. Streaming requires the URL server to send WAV bytes before synthesis
finishes; a pre-rendered HA media URL cannot reduce generation latency.

These protocol contracts are host-verification targets, not a claim that this
image has been deployed or acoustically tested. Protocol/framing, audio and
media-event implementation are separate from the Web control-plane checks.

## Private key and status

The canonical configuration stores `esphome_noise_key` as a canonical base64
encoding of exactly 32 bytes (44 characters including one trailing `=`). Empty
is an explicit **unprovisioned** state: the daemon initially uses the all-zero
Noise PSK, and HA provisions the device. Restrict provisioning to a trusted LAN.
Config saves preserve an existing provisioned key. Config and its backups are
mode `0600`; ordinary config exports, voice status and diagnostics never reveal
the key. There is no web reveal/rotation endpoint in this change.

The daemon consumes `--config` through the normal `LE_CONFIG_PATH` and writes
bounded status to `/run/libreecho/esphome-status.json`. Its init script is
`/etc/init.d/libreecho-esphomed.init`, with pidfile
`/var/run/libreecho-esphomed.pid`. Stable factory/persisted device identity is
retained; this migration does not change network or kernel policy.

`GET /api/v1/voice-pipeline` reports `home_assistant.protocol: "esphome"`,
`port: 6053`, `ready`, and `connected`. Readiness binds the trusted daemon
executable, PID/start time/boot identity and its own listening-socket inode to
validated status. Connected additionally requires an actual native API session,
not simply a listening port. Missing/malformed/stale status, an unrelated PID or
an unrelated listener reports false. Audio/capture readiness remains separately
gated by adapter and privacy state; neither field probes dormant Custom
Whisper/Piper endpoints in HA mode.

Native wake selection is persisted independently as `esphome_active_wake_word`
(default Alexa, empty disabled, or `alexa_v0.1`) and survives ordinary config
saves without replacing Local/Custom wake settings. Validated `set_word` reloads
the existing inference worker with a bounded five-second wait; the satellite
allows six seconds for that transaction while ordinary adapter deadlines stay
at 500 ms. Host lifecycle tests script only ONNX compute; trained-model quality
and hardware capture through reload remain separate acceptance gates.

## Transitions and failures

On Linux, a requested transition returns **202/pending** while a tracked worker
stops the old owner and starts the new one. Poll voice status; concurrent voice
changes return **409**, while ordinary APIs continue to operate. A failed
activation restores the prior persisted voice intent before restoring its
service graph, without reverting unrelated settings accepted in the meantime.
The completed failure is visible as `restart.state: "failed"`; a rollback
failure is called out explicitly. Missing ESPHome installation returns **501**
before any local services are stopped. A failed HA stop never activates a
competing local owner.

Focused host checks:

```sh
python3 tests/test_esphome_control.py
python3 tests/test_esphome_control_http.py
node tests/test_voice_assistant_ha_mode_ui.js
```

The HTTP test requires `build/libreecho-web`. Tests use injected private paths
or a private mock configuration, and clean up their fixture processes. They do
not contact hardware or prove speaker/microphone performance.
