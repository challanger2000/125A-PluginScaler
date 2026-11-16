125A PluginScaler — einfacher VST3-GUI-Test (Windows 11 x64)

WICHTIG: EXPERIMENT. Kein fertiger, universeller Plugin-Scaler.
Kein VST3 wird geladen oder verändert. Kein Installer, keine DLL-Injektion.

1) Plugin-GUI normal in Cubase oder Fender Studio Pro öffnen.
2) Plugin-Fenster sichtbar auf dem Desktop lassen (nicht minimieren).
3) PluginScalerVST3Window.exe starten.
4) In der kleinen Scaler-Oberfläche 125, 150, 175 oder 200 % wählen.
5) Maus auf die eigentliche Plugin-Grafik setzen, nicht auf Cubase-Kopfleiste.
6) STRG+ALT+F8 drücken.
7) Das zweite Fenster zeigt eine vergrößerte Kopie. Regler, Buttons,
   Ziehen und Mausrad im größeren Fenster ausprobieren.
8) Bei Problemen Plugin-Fenster schließen oder den Scaler beenden.

FÜR JEDES GETESTETE PLUGIN:
- DAW-Version, Plugin-Name und Version
- Zoom-Prozent; Windows-Bildschirmskalierung
- Bild sichtbar / schwarz / überlagert?
- Klick, Reglerziehen, Mausrad: ja/nein?
- Plugin weiter bedienbar, wenn anderes Fenster davorliegt?
- Windows- oder DAW-Absturz? (Falls ja, vollständige Fehlermeldung)

BEKANNTE EINSCHRÄNKUNGEN:
- Nur 64-Bit-Windows-EXE; VST-Format selbst ist für das Verfahren egal.
  Ein VST2-Absturz beim Laden wird hierdurch NICHT behoben.
- Ein Klon der Desktopregion, KEINE native GUI-Resize-Schnittstelle.
- Das Originalfenster muss sichtbar bleiben. Bei Overlays und GPU-
  gerenderten Oberflächen kann das Bild leer oder fehlerhaft sein.
- Mausklicks werden als Win32-Nachrichten weitergereicht; einige Plugins
  verlangen echtes Cursor-Tracking, Raw Input oder Fokus und reagieren
  deshalb nicht oder falsch. Tastatureingaben sind nicht umgesetzt.
- Popup-Menüs können außerhalb der vergrößerten Fläche erscheinen.
- Hohe Vergrößerung kann ein Fenster größer als den Monitor machen.
- Ein auf der normalen Windows-Desktop-Sitzung laufender Scaler kann
  weder Administrator-Plugins noch fremde Desktops sicher bedienen.
- Die CI prüft nur x64-Kompilierung und Koordinatenberechnung, NICHT
  die praktische Bedienbarkeit mit Cubase oder fremden Plugins.

Soll ein Plugin garantiert ohne Mausprobleme skalieren, braucht es
eine aufwendigere Lösung. Dieser Test soll genau das früh klären.
