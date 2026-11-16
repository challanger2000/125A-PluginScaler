# 125A PluginScaler — minimaler Windows-GUI-Prototyp

**Branch:** `feature/vst3-window-scaler-20261116`

Kleines externes Windows-Programm zur Vergrößerung bereits geöffneter
Pluginfenster. Es baut **keinen** VST3-Proxy und verändert weder Audio
noch MIDI, DAW oder Originalplugin.

## Bedienung

Das Testpaket aus GitHub Actions herunterladen, ZIP entpacken, EXE
starten und die beiliegende `START-HERE.txt` befolgen.

Der Viewer benutzt die Windows Magnification API. Das Originalfenster
bleibt geöffnet. Standardmäßig liegt ein vergrößertes Fenster mit Titelleiste als Overlay
darüber; alternativ lässt sich wieder ein separates Viewer-Fenster wählen.
Die Kopie kann zwischen 125 und 200 % vergrößert werden.
Der Viewer ist auch im Overlay-Modus an der normalen Windows-Titelleiste
verschiebbar. "Auto-Fit" hält das komplette Fenster innerhalb des
Monitor-Arbeitsbereichs und passt die Mauskoordinaten an den tatsächlich
verwendeten Zoom an. Die neue optische Original-Abdeckung ist bewusst
abschaltbar, da die Magnification-Filterliste nicht auf jeder GPU gleich
funktioniert. Sie verbirgt das Original NICHT über SW_HIDE.
Mit Strg+Alt+F9 werden Viewer und Abdeckung geschlossen. Der Prototyp arbeitet
formatunabhängig mit sichtbaren Fenstern: VST2 funktioniert in ersten
Nutzertests ebenfalls. Die DAW-interne Einbettung ist damit noch nicht
implementiert. Ein Win32-Maus-Nachrichtenadapter setzt Viewer-Koordinaten
auf Quellkoordinaten um. Dieser Adapter ist **nicht universell**.

## Entwicklung

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Keine externen Dependencies, kein VST-SDK, kein Installer.

## Stand und Grenzen

Automatisiert prüfbar: Windows-x64-Build und deterministische
Koordinaten-Tests. Noch nicht automatisiert prüfbar: realer
Plugin-Fensterinhalt, echte Drag-Bedienung, Mausfokus und Rendering
in Cubase oder Fender Studio Pro.

Der vorhandene jBridge-Entwicklungszweig bleibt unverändert.
