# Blur Sabers

Adds a configurable runtime blur/trail layer to Beat Saber sabers on Quest.

Use `Settings > Mod Settings > Blur Sabers` in-game to change:

* enabled/disabled
* trail time
* trail width
* alpha
* tip offset
* RGB trail color

Your custom saber model still needs to be exported from Unity/Qosmetics as a Quest saber asset. Beat Saber on Quest cannot import a raw `.fbx` file at runtime from this native mod by itself.

## Build note

Run `qpm restore` before building. QPM needs Git available on `PATH` so it can clone dependency packages into its cache.

## Credits

* [zoller27osu](https://github.com/zoller27osu), [Sc2ad](https://github.com/Sc2ad) and [jakibaki](https://github.com/jakibaki) - [beatsaber-hook](https://github.com/sc2ad/beatsaber-hook)
* [raftario](https://github.com/raftario)
* [Lauriethefish](https://github.com/Lauriethefish), [danrouse](https://github.com/danrouse) and [Bobby Shmurner](https://github.com/BobbyShmurner) for [this template](https://github.com/Lauriethefish/quest-mod-template)
