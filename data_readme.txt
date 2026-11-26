Borderlands 4 Trainer {version}
===========================

Single player only. Personal use.

This folder goes into the game's OakGame\Binaries\Win64 folder, next to dsound.dll (see INSTALL.txt
in the zip). Start the game from Steam as usual: the game loads the trainer by itself.
In game press INSERT to open or close the trainer (controller: LB + D-pad Down).

Back up your saves first: options that change your character (level, skill points, items,
save editor) end up in your save because the game saves them itself.
Saves: Documents\My Games\Borderlands 4\Saved\SaveGames

No game file is changed. To remove the trainer, close the game and delete dsound.dll and this
folder from Win64.

Files the trainer writes, all inside this folder:
  trainer.log            this game session's log
  logs\                  the last 20 sessions' logs
  logs\crashes\          the log of every game session that ended unexpectedly (kept)
  logs\reports\          trainer reports (Settings > Support > Create trainer report)
  data\                  options you switched on, edited values, positions
  settings.json          panel hotkey, scale, look
  save_backups\          a copy of your save before every save-editor change

Something not working? In the trainer open Settings > Support > "Create trainer report" and
send the report (it is also copied to your clipboard). User name and folders are removed from it.

No "Trainer ready" message and no trainer.log in this folder? Then the game did not load the
trainer. Check that dsound.dll and this BL4Trainer folder are both directly in
OakGame\Binaries\Win64 (not in a subfolder, and not in the game's top folder) and that
antivirus did not remove dsound.dll. Still nothing: send trainer.log (if there is one), how you
start the game and your game version.

Official download: https://github.com/RipXic/Borderlands4Trainer/releases
