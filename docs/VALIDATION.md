# Prüfstand und Anforderungsmatrix — 0.1 Prototyp

Kein FULL AUDIT, keine Produktfreigabe. Die folgenden Ergebnisse sind ausschließlich die tatsächlich ausgeführten Prüfungen. Die Windows-Programme wurden auf Debian 13 mit MinGW-w64 GCC 14 POSIX gebaut und unter Wine 10/Xvfb (1920×1080) ausgeführt; CPU-/Scheduler-Werte dieses virtuellen Systems sind keine ASIO-Zertifizierung. Source-Branch: `feat/isolated-vst3-scaler-prototype`. Den exakten Paketstand und Dateihashes enthält `manifest.json` im ZIP.

## Ausgeführt

- Native Linux-Release-CTest: **2/2 bestanden**, davon ein absichtlich scheiternder Aufruf als erwarteter Negativtest für wirksame Release-Assertions.
- Windows-x64-Release-Programme unter Wine: **4/4 CTests bestanden**: Geometrie/Filter, Assertion-Negativtest, VST3-Host→Helper→VST2-Test-DLL und Helper→VST3-Testplugin.
- x64-VST3 und Helper sowie x86-Helper/Test-DLL: **Cross-Build bestanden**, gemeinsame ABI-Größen statisch für beide Architekturen geprüft.
- Geometrie-/Filtertests zusätzlich unter AddressSanitizer und UndefinedBehaviorSanitizer: **bestanden**.
- Beide Testplugins sind selbst geschriebene **synthetische Fixtures/Mocks mit echten Plugin-ABIs**. Es wurden keine Native-Instruments-Plugins ausgeführt.
- Ein BMP wird aus dem tatsächlich angehängten, auf **150 %** geschalteten Proxy-Editor erfasst. Die Tests prüfen zusätzlich Fenstergröße, dargestellte Farbpixel und veränderte Parameter. Ein bloßes hochskaliertes Bild zählt nicht als Eingabenachweis.
- Gepaceter GUI-/Audio-Test: **100 × 1024 Samples bei 48 kHz**, gleichzeitig Mausgesten über den skalierten Pfad; **0 falsche/stumme Blöcke, 0 Deadline-Überschreitungen**. Die genaue aktuelle p95/p99/max-Ausgabe steht in `evidence/ctest-wine-results.txt`. Kein belastbarer plattformübergreifender Performance-Vergleich.

## Vertrag → Nachweis

| Anforderung / tatsächlicher Pfad | Positive / negative / Grenzprüfung | Stand |
|---|---|---|
| VST3-Ausgabe, echte Plugin-Schnittstellen | DLL-Fabrik enumeriert, IComponent/IAudioProcessor/IEditController abgefragt, Lifecycle und PCM durch getrennten Helper | PASS unter Wine mit VST2-Fixture; Steinberg-Compliance noch NOT VERIFIED |
| VST2-x64-Eingang | Exakte Stereo-PCM, 44.1/48/96 kHz; 1/31/64/257/1024/2048 Frames | PASS synthetisches Plugin; winzige Blöcke nicht realtime-qualifiziert |
| VST2-x86 im eigenen Prozess | x86-Helper und ABI/Test-DLL bauen; vollständiger x64→x86-Hosttest im manuellen Windows-Workflow vorgesehen | BUILD PASS; lokaler x86-Runtime-Test NOT RUN (keine nutzbare 32-Bit-Wine-Laufzeit) |
| VST3-x64-Eingang | DLL-Fabrik, Lifecycle, nativer ParamID **42** statt Index 0, Sample-Offset-Noten/Automation, Stereo-PCM und State→PCM | PASS direkt über Helper mit synthetischem VST3; Wrapper→VST3→GUI-Gesamtkette NOT RUN |
| Audio-Formate / Busse | Stereo akzeptiert, Mono/Float64 abgelehnt; ungültige Events liefern Fehler und Nullen, anschließend funktioniert derselbe Bridge-Prozess weiter | PASS für geprüfte Fälle; Multibus/Float64 nicht implementiert |
| MIDI NoteOn/Off | Im echten VST3-Prozessblock NoteOn bei Sample 17, NoteOff bei 43; jedes Ausgangssample mit Sollwert verglichen | PASS VST2-Fixture; VST3-Fixture separat mit anderen Offsets |
| MIDI-CC, Pitchbend, SysEx, MIDI-Ausgang, polyphoner Ausdruck | Keine entsprechende vollständige API-/Mappingkette | NICHT IMPLEMENTIERT |
| Sample-Offset-Automation | Gain-Wechsel bei 19 und 47, beide Kanäle mit unabhängig berechneten Segmentwerten | PASS Fixtures; Parametertyp-/Unit-/Listenmetadaten noch eingeschränkt |
| NaN-/Überlauf-Eingabepakete | NaN-Wert und 257 Queue-Punkte abgelehnt, keine Anwendung des Pakets; gültiger Folgeblock funktioniert | PASS; generelle Audiodaten-Sanitierung nicht nachgewiesen |
| Preset-/Projektzustand | Deaktivieren, Snapshot, Gain ändern, Restore, gleiche PCM; trunkierter Wrapper-State abgelehnt ohne Änderung des vorherigen Gains | PASS VST2-Fixture; VST3-State separat PASS; echte Plugin-Chunks/Presets NOT RUN |
| Live-State / Projekt speichern bei aktivem Prozessor | State-API lehnt aktive Verarbeitung explizit ab | NICHT ERFÜLLT; blockiert praktische universelle DAW-Freigabe |
| 100/125/150/175/200/300/400 % | 1000 mathematische Rundreisen; ganze Pixel-Faktoren exakt, konstante Farben erhalten, definierte 150-%-Filterkante | PASS Core |
| Angehängter Originaleditor in allen passenden Größen | Menükommandos über Win32 `WM_COMMAND`; tatsächliche Fenstermaße, Bildfarbpixel, skalierte Klicks→Gain≈0.75 | PASS alle 7 bei 1920×1080; physische Menübedienung/DAW NOT RUN |
| Nicht passende Stufen gesperrt | Fit-Orakel für Arbeitsfläche, unpassende Geometrie wird ohne Maßstabsänderung abgelehnt | PASS Core; Hosttest prüft Ablehnung, falls eine Stufe auf dem Testmonitor nicht passt |
| Host lehnt Größenwechsel ab | IPlugFrame-Testhost verweigert Wechsel; alte Größe bleibt bestehen | PASS Testhost; Studio-One-Verhalten NOT RUN |
| Drag, Mouse Capture, signed Außengrenzen | Ein Down→Move(-80)→Move(400)→Up erreicht Gain-Ende; Proxy hält Capture; keine Begrenzung aktiver Koordinaten | PASS nachrichtenbasiertes Fixture; native Cursor-/Raw-Input-Regler NOT VERIFIED |
| Keyboard / Wheel / Fokus | VK_DOWN und negativer Wheel-Schritt ändern echten Plugin-Gain; Fokus-/Nachrichtenroute vorhanden | PASS Fixture; DAW-Fokus, Text-/IME-/Shortcut-Integration NOT VERIFIED |
| GUI→Host-Automation | Plugin-Geste→Callback-Mirror→IComponentHandler begin/perform/end mit übereinstimmenden Zählern und tatsächlichem Wert | PASS koaleszierte Werte; originale Gestendauer noch nicht erhalten |
| Öffnen/Schließen/Wiederöffnen | Editor tatsächlich attached/removed, drei vollständige Wiederöffnungen, vollständiges DLL-Neuladen sowie noch lebende View bei Component-Terminierung | PASS Fixture |
| Audio unter GUI-Last | Gepacete 100 Blöcke, jedes Sample positiv und Stereo-Beziehung exakt, Laufzeit gegen Blockdeadline geprüft | PASS Wine 48k/1024; kein 64-Sample-ASIO-/Pro-53-Nachweis |
| Helper-Absturz und Hänger | Fixture beendet Prozess oder blockiert 200 ms; Proxy kehrt begrenzt zurück, Folgeblöcke stumm, kein Poison-Replay | PASS begrenztes Fehlerverhalten; Stille ist ausdrücklich **kein** Audio-Continuity-PASS |
| Prozess-Rückbau | Prozessanzahl vor/nach Instanzlebenszyklus sowie Fault-Fixtures identisch | PASS Wine; echte fremde Plugin-Threads noch NOT RUN |
| Native Popups, GPU-/OpenGL-/D3D-Editoren, Cursor-Polling, dynamischer Editor-Resize, Hot-DPI-Wechsel | Erfordern andere Backends/Kompatibilitätsprofile | NICHT IMPLEMENTIERT bzw. NOT VERIFIED |
| Transport/Tempo/Offline-Rendern | Keine vollständige Kontextweiterleitung; Offline-Setup wird abgelehnt | NICHT IMPLEMENTIERT |
| Steinberg Validator / 125A Plugin Tester | Nicht in diesem Cloud-Prüfstand vorhanden/ausgeführt | NOT RUN |
| Native MSVC-/Windows-11-Ausführung / Studio One / Pro-53 / FM7 / FM8 | Workflow vorhanden; weder Windows-Maschine noch lizenzierte Ziel-Plugins vorhanden | NOT RUN; praktische Abnahme bleibt erforderlich |

## Kontrollelemente des Prüfstands

| Element | UI → Wirkung → Rückmeldung → State | Nachweis / Lücke |
|---|---|---|
| Zoom-Toolbar / Auswahl | UI-Kommando → Geometrie → IPlugFrame → Bildfilter; Prozentanzeige; Maßstab im Wrapper-State | Automatisierte Kommandos und Screenshot; physisches Menü und Zoom-Recall nach DAW-Neustart offen |
| Originaler Fixture-Gain-Balken | scaled down/move/up → Helper-Kindfenster → Plugin-Gain → `audioMasterAutomate` → VST3-Host; Originalbild zeigt neuen Balken | Parameter und Bild geprüft; Audio bei gespeicherten Werten und unter GUI-Stress geprüft |
| Fixture Keyboard/Wheel | Nachrichten → Gain-Schritt → Callback / Bild | Echte Wertänderung geprüft; Hardware-/DAW-Fokus offen |
| Host-Gain-Parameter ID 0 | Queue-Punkte → Shared Events → nativer Gain → PCM | Samplegenaue Segmentwerte / State-Recall geprüft |
| Host-Test-fault ID 1 | Nur synthetische Fixture: 0=normal, 0.5=Hänger, 1=Helper-Exit | Timeout, Stille, keine Wiederholung und Prozessende geprüft; kein Produktparameter |

## Reproduktion und verbleibende Abnahme

Die CTests und `tools/package.py` stehen im Repository; der manuelle GitHub-Workflow erzeugt erst nach tatsächlicher Ausführung native MSVC-Artefakte. Seine bloße Existenz ist kein Windows-PASS. Keine CI-Ausführung und kein GitHub-Release wurden in diesem Cloud-Turn behauptet oder gestartet.

Vor einem praktischen Pro-53-/FM7-Versuch müssen deren Parameteranzahl, Host-Callback-/Idle-Verhalten und GUI-Eingabemethode geprüft werden. Kein universelles Kompatibilitätsversprechen für diese Dateien. Als nächster unabhängiger Prüfstand ist eine Windows-11-Maschine mit autorisierten x86/x64-Plugins und Studio One erforderlich. Erst dann lassen sich die produktrelevanten offenen Zeilen schließen.
