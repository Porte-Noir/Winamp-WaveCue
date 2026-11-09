# Winamp WaveCue

Projet de plugin Winamp 5.x classique (32 bits).

## Ajouter le code source

Le prototype complet se trouve dans l'archive `Winamp_WaveCue_source.zip` fournie dans la conversation. Décompressez-la et ajoutez son contenu à ce dépôt (en conservant les dossiers `src`, `tests` et `.github/workflows`).

## Compilation automatique

À chaque ajout de code sur `main`, le workflow **Build Winamp WaveCue 32-bit** compile `gen_wavecue.dll` sur Windows (Visual Studio 2022, Win32). Dans l'onglet **Actions**, ouvrez le dernier lancement et téléchargez l'artefact `gen_wavecue-win32` après une compilation réussie.

**Statut :** compilation et tests dans Winamp non encore validés.
