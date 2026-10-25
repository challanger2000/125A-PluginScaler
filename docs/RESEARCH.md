# Quellen, Herkunft und Lizenzgrenzen

Gelesen: `125A-Engineering/START-HERE.md` vollständig und relevante Engineering-Guardrails, Workflow-/GUI-/Plugin-Verhaltensregeln sowie QA-Standards, Stand `b6df84b0a04d01e240cff745ccebf60d67750486`. Dieses Projekt ist eine neue Entwicklung im ausdrücklich bestimmten Repository, kein Update einer freigegebenen Final-Version. `125A-Branding/README.md` wurde konsultiert; der technische Prüfstand enthält keine nachgezeichneten Markenlogos oder neue Produktgrafik.

Die im Auftrag erwähnte Datei `RESEARCH/EXTERNAL-PROJECTS/FM8-PLUS-PLUGINSCALER.md` existierte im untersuchten Engineering-HEAD nicht. Die fehlende Notiz wird nicht als gelesen ausgegeben.

FM8.plus: <https://github.com/musicastudio/FM8.plus>, Forschungsstand `db49d48b597f07356c6c64846cb3738a7cef8113`. README und Windows-Design-/GUI-Dokumentation erklären native DPI-Freischaltung, versionsgebundene Win32-Anpassung, Bildschirm-Fit und Pixel-Art-Skalierung. GPLv3: kein Sourcecode, Header, Shader, Ressourcen oder Binärdateien dieses Projekts befinden sich in unserem Produkt. Die Filter- und Koordinatenimplementierung ist eigenständig. Kein Zugriff auf oder Patchen von proprietären jBridge-/NI-Dateien.

Offizielle VST3-Schnittstellen: <https://github.com/steinbergmedia/vst3_pluginterfaces>, fest auf `4f547e8e102b47de4a8b8aaf343c73b700786372`, SDK-Version aus `kVstVersionString`. MIT-Lizenz siehe `licenses/vst3-pluginterfaces-MIT.txt`. Keine alten VST2-SDK-Header werden vertrieben. Die minimale VST2-Interoperabilitätsdefinition stammt aus dem eigenen vorherigen PluginScaler-Branch `588b83aa86e608ac282e43486a1f8a605e42c6d3`; die neue Host-/Proxy-/Renderimplementierung ist davon getrennt.

Primäre API-Verträge:

- Steinberg `IComponent`, `IAudioProcessor`, `IEditController`, `IEventList`, `IParameterChanges`, `IBStream`, `IPlugView`, `IPlugFrame`, `IConnectionPoint`: die tatsächlich fixierten offiziellen Header und <https://steinbergmedia.github.io/vst3_doc/>.
- Microsoft `PrintWindow` / `WM_PRINTCLIENT`: <https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-printwindow>, <https://learn.microsoft.com/windows/win32/gdi/wm-printclient>.
- Mouse Capture und signed Client-Koordinaten: <https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-setcapture>, <https://learn.microsoft.com/windows/win32/inputdev/wm-mousemove>.
- Job-Lebenszeit: <https://learn.microsoft.com/windows/win32/api/winnt/ns-winnt-jobobject_extended_limit_information>.

Die lokalen Windows-Binaries werden mit MinGW-w64 GCC 14 POSIX und statischer GCC-/C++-Laufzeit erzeugt. Deren Runtime-Lizenzen und GCC Runtime Library Exception sind in `licenses/` enthalten. Die Runtime Exception erlaubt die Verbindung unabhängiger Software; FM8.plus-GPL-Code ist weder Bestandteil noch Buildabhängigkeit. Wine dient ausschließlich als Testumgebung und wird nicht mit dem ZIP verteilt.

Evidenzklassen: API-/Layout-Verträge **DOCUMENTED**; Geometrie und Filter **mathematisch abgeleitet + automatisiert geprüft**; tatsächlich ausgeführte PCM-/UI-Tests **MEASURED**, mit synthetischen eigenen Plugins. Bildqualität auf realen Legacy-Plugins, Windows-/DAW-Verträglichkeit und künftige Backends bleiben **NOT VERIFIED**.
