Borderlands 4 Trainer {version}
===========================

Single player only. Personal use.

1. Keep every file in this folder together (anywhere you like).
2. Run BL4Trainer.exe. If the game is not running it starts it through Steam and attaches
   by itself; if it is running it attaches right away.
3. In game press INSERT to open or close the trainer (controller: LB + D-pad Down).

Back up your saves first: options that change your character (level, skill points, items,
save editor) end up in your save because the game saves them itself.
Saves: Documents\My Games\Borderlands 4\Saved\SaveGames

Nothing in the game folder is changed. To remove the trainer, close the game and delete this folder.

Files the trainer writes, all inside this folder:
  loader.log             what BL4Trainer.exe did on its last run (send it if the trainer does not attach)
  trainer.log            this game session's log
  logs\                  the last 20 sessions' logs
  logs\crashes\          the log of every game session that ended unexpectedly (kept)
  logs\reports\          trainer reports (Settings > Support > Create trainer report)
  data\                  options you switched on, edited values, positions
  settings.json          panel hotkey, scale, look
  save_backups\          a copy of your save before every save-editor change

Something not working? In the trainer open Settings > Support > "Create trainer report" and
send the report (it is also copied to your clipboard). User name and folders are removed from it.

The trainer does not attach ("Windows refused access")? If Steam or the game runs as administrator,
BL4Trainer.exe asks to restart as administrator - click Yes. If it still fails, antivirus is
probably blocking it: add this folder as an exception. Send loader.log if you need help.

Attached but nothing opens? The game restarts itself after a game patch: leave the BL4Trainer.exe
window open until the main menu so it can attach again. Still nothing: send loader.log and trainer.log.

Official download: https://github.com/RipXic/Borderlands4Trainer/releases
