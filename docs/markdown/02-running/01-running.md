# Running the game

## Game selection

**Medal of Honor: Allied Assault** is the default game, but expansions are also supported.

### Start using launchers

Base game and expansions can be started from one of the 3 launchers:

- `launch_openmohaa_base`, use this to play **Medal of Honor: Allied Assault**
- `launch_openmohaa_spearhead`, use this to play **Medal of Honor: Allied Assault: Spearhead**
- `launch_openmohaa_breakthrough`, use this to play **Medal of Honor: Allied Assault: Breakthrough**

### Start from the command-line

**Spearhead** and **Breakthrough** are supported in OpenMoHAA using the `com_target_game` variable.

To change the target game, append the following command-line arguments to the `openmohaa` and `omohaaded` executable:

- `+set com_target_game 0` for the default base game (mohaa, uses `main` folder)
- `+set com_target_game 1` for the Spearhead expansion (mohaas, uses `mainta` folder)
- `+set com_target_game 2` for the Breakthrough expansion (mohaab, uses `maintt` folder)

OpenMoHAA will also use the correct network protocol version accordingly. The default value of `com_target_game` is 0.

On Windows, a shortcut can be created to the `openmohaa` executable, with the command-line argument appended from above to play an expansion.

### Using a demo version

The argument `+set com_target_demo 1` must be appended to command-line to play the game or host a server using demo assets. Allied Assault, Spearhead and Breakthrough demos are supported.

## User data location

By default, Project: Omaha stores user-writable data (configs, screenshots, logs,
demos) in the **game install directory** (next to the `openmohaa` binary) — the
same portable layout as retail MOH:AA and zip unpacks. Subfolders match the
active game: `main`, `mainta`, or `maintt`.

Override with a command-line `fs_homepath` if you want a separate writable tree
(for example a dedicated server data dir, or a classic XDG/AppData path):

- `+set fs_homepath Z:\omaha_data` — absolute path
- `+set fs_homepath homedata` — subdirectory under the process working directory
- `+set fs_homepath %APPDATA%\openmohaa` / `+set fs_homepath ~/.local/share/openmohaa` — OS user profile (optional)

Note that the configuration file isn't created nor written automatically on a dedicated server (**omohaaded**).

## Configuration

For more settings such as configuring bots, see [Configuration and commands](../03-configuration/01-configuration.md).
