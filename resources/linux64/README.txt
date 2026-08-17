pokeemerald-recomp v0.1.0-alpha - Linux x64

Run ./pokeemerald-recomp directly from this folder. The images directory must
remain beside the executable. The system SDL2 and SDL2_image runtime
libraries are required.

On first launch, select a compatible Pokemon Emerald ROM that you legally own.
The ROM is validated locally and used to install a versioned game-data package.
The ROM is not copied into this folder or uploaded. Gameplay remains unavailable
until valid game data has been installed.

Player data uses SDL's per-user application-data path, normally:
  $XDG_DATA_HOME/pokeemerald/pokeemerald/
or, when XDG_DATA_HOME is unset:
  ~/.local/share/pokeemerald/pokeemerald/

Shared game data is stored under games/emerald/ and profile data under profiles/.
Normal game saves use the vanilla 128 KiB game.sav format. Native save states
are build- and platform-validated; do not expect Windows native states to load
in this Linux build.
