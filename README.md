# 125A PluginScaler — technischer Prototyp 0.1

Eigenständiger Windows-x64-VST3-Wrapper mit eigenem x64-/x86-Plugin-Helper und einer skalierbaren Darstellung des originalen Plugin-Editors. **Experimenteller Entwicklungsstand, kein Ersatz für die funktionierende Pro-53-Lösung.**

Die bisherige Lösung und `main` bleiben unverändert. Entwicklung erfolgt auf `feat/isolated-vst3-scaler-prototype`, ausgehend von `c460576e18a574c373bf73af3ca4cc37107e8bdb`. Es gibt noch keine freigegebene Produktversion.

## Praktischer Test auf Windows 11

1. Das vollständige Verzeichnis `PluginScaler.vst3` aus dem ZIP nach `%LOCALAPPDATA%\Programs\Common\VST3` kopieren oder einen eigenen VST3-Testordner im Host einrichten. Helfer und Test-DLLs müssen im Unterordner `Contents\x86_64-win` bleiben. Keine vorhandene Plugin-Datei ersetzen.
2. Der Standard ist das mitgelieferte **synthetische Testplugin**: Stereo-Eingang × Gain, zusätzlich ein konstanter Testton-Pegel bei MIDI-Noten. Das ist kein Instrument für Musikproduktion. `Test fault (0=off)` bei **0** belassen; dieser Testparameter löst absichtlich einen Helper-Abbruch oder eine Verzögerung aus.
3. Im VST3-Host „125A PluginScaler Prototype“ als Stereo-Effekt öffnen. Über „Zoom“ die Größenstufe auswählen; anschließend Gain durch Ziehen bedienen. Der Balken stammt aus dem originalen Editor der geladenen Test-DLL.
4. Audio, Automation sowie Öffnen/Schließen/Wiederöffnen prüfen. **State-Speicherung funktioniert in diesem Stand nur bei vom Host deaktivierter Verarbeitung (`setProcessing(false)`).** Einfacher Transport-Stop genügt dafür nicht in jedem Host. Projekt-Recall im laufenden DAW-Betrieb ist noch kein erfülltes Produktmerkmal.
5. Für einen eigenen Plugin-Versuch `Contents\x86_64-win\PluginScaler.ini` bearbeiten, bevor der Host scannt:

```ini
[plugin]
path=C:\Plugins\MeinPlugin.dll
```

Ein `.vst3`-Bundlepfad oder dessen Windows-Binärdatei ist ebenfalls möglich. Die PE-Architektur bestimmt automatisch den Helper. Die Original-DLL wird ausschließlich gelesen/geladen und niemals verändert. VST2-x86-Dateien werden niemals in den x64-DAW-Prozess geladen.

**Grenzen:** höchstens 128 Parameter, 0–2 VST2-Eingänge bzw. 0 oder 2 VST3-Eingänge, genau 2 Ausgänge, Float32, maximal 2048 Samples/Block, 8–384 kHz. Editor höchstens 1024×768 native Pixel. Es gibt eine feste Plugin-Zuordnung pro Wrapper-Konfiguration; kein Plugin-Browser oder Wechsel während einer Session. Nicht nachträglich die Ziel-DLL wechseln und bestehende Projekte als kompatibel ansehen.

Pro-53/FM7/FM8 und Studio One wurden hier **nicht** praktisch getestet. Insbesondere große Parameterlisten, cursorabfragende Regler, Popup-Menüs, GPU-Editoren, Transport-/Tempo-Abhängigkeiten und Live-State sind noch offene Hürden. Ein Screenshot oder ein synthetischer Test ersetzt diese Prüfung nicht.

Die Paket-Selbstprüfung lässt sich ohne DAW im Unterordner `Contents\x86_64-win` starten:

```powershell
.\integration-tests.exe "$pwd\PluginScaler.vst3"
```

Sie öffnet kurz einen eigenen Testhost, prüft synthetisches Audio und Editor-Eingaben und beendet sich mit 0 bei Erfolg. Das bestätigt keine Kompatibilität mit einem kommerziellen Plugin oder mit Studio One.

## Bauen und testen

Windows: Visual Studio 2022 C++-Werkzeuge, Windows SDK, CMake ≥3.24 und Python 3 für das Paket. Nur die offiziellen, auf einen Commit fixierten MIT-lizenzierten VST3-Interfaces werden automatisch geladen.

```powershell
cmake -S . -B build64 -A x64
cmake --build build64 --config Release --parallel 4
ctest --test-dir build64 -C Release --output-on-failure
cmake -S . -B build32 -A Win32
cmake --build build32 --config Release --parallel 4
ctest --test-dir build32 -C Release --output-on-failure
python tools/package.py --x64 build64/Release --x86 build32/Release --out artifacts
```

Zusätzlicher x64→x86-Hosttest: siehe die Schritte im manuellen Workflow `.github/workflows/windows-prototype.yml`. Der Workflow baut beide Architekturen in einem Windows-Job und führt auch den vollständigen VST3-Hosttest mit dem x86-Testplugin aus. Kein automatischer Build bei jedem Push.

Linux: Die Geometrie-/Filtertests benötigen nur C++17 und CMake. Die Windows-Ziele lassen sich mit MinGW und den Toolchain-Dateien in `cmake/` bauen. Ein solcher Build ist kein MSVC- oder Windows-11-Test.

```sh
cmake -S . -B build-native -DCMAKE_BUILD_TYPE=Release
cmake --build build-native --parallel 4
ctest --test-dir build-native --output-on-failure
cmake -S . -B build-win64 -DCMAKE_TOOLCHAIN_FILE=cmake/mingw64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-win64 --parallel 4
```

Die Cloud enthält bereits lokale Werkzeuge unter `/workspace/tooling/root/usr/bin`. Für CMake dort zusätzlich `LD_LIBRARY_PATH=/workspace/tooling/root/usr/lib/x86_64-linux-gnu` setzen. Die SDK-Interfaces können mit `-DSCALER_VST3_ROOT=/workspace/tooling/sdk` wiederverwendet werden. Keine zusätzliche Git-Worktree anlegen: Cloud-Tasks sind bereits isoliert.

Architektur und Forschungsbewertung: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). Prüfnachweise und offene Verträge: [docs/VALIDATION.md](docs/VALIDATION.md). Lizenzen und Quellen: [docs/RESEARCH.md](docs/RESEARCH.md).
