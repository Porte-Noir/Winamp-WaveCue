# WaveCue pour Winamp classique — prototype v0.1

Plugin **General Purpose** (Winamp 5.x **32 bits**) offrant une fenêtre de visualisation des formes d'onde et un système de repères. Code C++17 / Win32 / Media Foundation, sans dépendances tierces.

## Installation sur Windows

1. Sur un PC **Windows 10/11** de compilation, installer **Visual Studio 2022 Community** avec la charge de travail « Développement Desktop en C++ » (prise en charge CMake et outils x86). La DLL résultante cible aussi Windows 7 SP1, sous réserve de la présence de Media Foundation.
2. Double-cliquer sur `build_windows.bat` ou exécuter :

   ```bat
   cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
   cmake --build build --config Release
   ```

3. Fermer Winamp (y compris sur un autre PC, par exemple sous Windows 7). Copier `build\Release\gen_wavecue.dll` dans le dossier `Plugins` de Winamp (souvent `C:\Program Files (x86)\Winamp\Plugins\`).
4. Relancer Winamp. La fenêtre WaveCue s'ouvre; la rubrique **Options > Préférences > Plugins > General Purpose** permet également de retrouver le plugin et d'ouvrir sa configuration.

## Utilisation

- Cliquer sur un morceau **dans le Playlist Editor**. La fenêtre WaveCue suit la première sélection, avec une zone vide.
- Cliquer **Show/Load Waveform**. La première fois, l'analyse du fichier se fait en tâche de fond. Les ouvertures suivantes utilisent un cache local.
- **Add Cue**, **Set IN**, **Set OUT** puis clic sur la waveform pour poser le repère. Les marques sont enregistrées automatiquement.
- Clic sur un marqueur : saute à ce point si **ce morceau joue**. Les clics sur un autre morceau ne perturbent pas la lecture.
- Faire glisser un repère pour le repositionner; sélectionner un marqueur puis appuyer sur **Suppr** pour l'effacer.
- **Auto IN/OUT** : quand un nouveau morceau commence, sa lecture est avancée jusqu'au IN (le cas échéant); une fois OUT atteint, Winamp passe au titre suivant. Activation volontaire; OFF par défaut.
- Fermer la fenêtre la masque sans désactiver le plugin; la rouvrir via sa configuration Winamp.

## Performance et compatibilité

- L'analyse démarre **uniquement** à la demande; un thread séparé décode le fichier.
- Le cache est dans `%APPDATA%\Winamp_WaveCue\cache` et contient des pics par blocs de ~50 ms (**~72 Ko par 30 minutes**).
- La première analyse décode le morceau entier : sa durée dépend du format, de l'ordinateur et de la taille du morceau. La lecture ultérieure du cache est rapide.
- Formats décodables via **Windows Media Foundation** installé sur l'ordinateur (MP3, WAV, WMA; AAC selon Windows). Certains formats compatibles avec Winamp, notamment FLAC sur Windows 7, requièrent une prise en charge supplémentaire.
- Fichiers locaux seulement; les flux réseau ne sont pas pris en charge.
- Les cues sont conservés sous forme de fichiers texte dans `%APPDATA%\Winamp_WaveCue`. Les données audio originales ne sont jamais modifiées.
- Interface pensée pour Winamp 5.x classique **32 bits**; pas encore validée sur Winamp 5.666, 5.9 ni WACUP en situation réelle.

## Statut du prototype

La logique centrale dispose de tests unitaires, mais la DLL n'a pas été compilée ni exécutée sous Windows dans cet environnement. Le fonctionnement dans Winamp doit donc être validé après compilation. Une fenêtre indépendante se lance au démarrage : elle n'est pas intégrée visuellement au Playlist Editor.

## Limitations volontaires

La version initiale ne possède pas de détection BPM / tonalité, préécoute isolée, zoom fin, historique d'annulation ou prise en charge des fichiers déplacés. La gestion IN/OUT est une automatisation de saut et de passage au titre suivant; elle n'édite pas physiquement le morceau. Un OUT inférieur à la durée ne coupe pas l'audio de façon parfaitement sample-accurate (polling toutes les 170 ms).

## Compilation sans Visual Studio local

Un workflow GitHub Actions est fourni dans `.github/workflows/windows-build.yml`. En publiant ce dossier dans un dépôt GitHub, lancer l'action « Build Winamp WaveCue 32-bit » puis récupérer l'artefact `gen_wavecue-win32`. Cette procédure exécute la compilation sur Windows 2022 dans GitHub Actions.
