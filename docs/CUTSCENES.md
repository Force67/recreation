# Cutscenes

Skyrim's cutscenes are not movies. There is no camera data, no timeline, no baked
animation for a conversation: a cutscene is a **SCEN record**, which is a cast of
quest aliases, a list of phases, and per-phase actions (speak a line, run an AI
package, wait). The game plays each line for exactly as long as its voice recording,
frames whoever is speaking procedurally, and moves actors by handing them packages.
The opening cart ride into Helgen is that and nothing more: travel packages on a
horse's alias, a cart towed by its enable parent, prisoners held in place by a
no-travel package, and a 29-phase scene speaking 20 lines over the top.

So the engine plays cutscenes by running that machinery, not by scripting scenes:

| System | Where | What it does |
| --- | --- | --- |
| Scene phase machine | `engine/quest/scene_runtime.{h,cc}` | Lowers a parsed SCEN into a `ScenePlan` and drives it: phases in order, one line at a time for its full length, packages held across their phase window, phase completion gated on the record's CTDA (with a timeout so an unreachable gate cannot wedge a scene). Pure, unit tested (`scene_runtimetest`). |
| Dialogue camera | `engine/world/cine_camera.{h,cc}` | The shot language: over-shoulder, reverse, two-shot, close-up, wide, solved from the speaker's and listener's head positions, plus a cutting policy that cuts on a new speaker, holds a minimum, and breaks up a long speech. Pure, unit tested (`cine_cameratest`). |
| Voice + pacing | `engine/dialogue/voice.{h,cc}` | Indexes the voice archive by the INFO id every file name carries, so a line finds its recording even when the clip was exported under another quest's name, and reads the clip's exact length out of the xWMA header, so a scene is paced by the recording. Unit tested (`voicetest`). |
| AI packages | `runtime/ai_package_director.{h,cc}` | Runs the alias package stacks: picks the first package whose conditions pass, walks travel packages to their target, fires the package's Papyrus fragment on arrival, tows objects behind the actor they are enable-parented to, and carries riders (with the vehicle's seated idles) on a moving cart. |
| Cutscene director | `runtime/cutscene_director.{h,cc}` | The live layer: indexes every SCEN by owning quest, starts scenes the way the game does, plays their lines (voice + caption + the INFO's own Papyrus fragment), fires the SCEN phase fragments that advance the journal, hands scene packages to the package driver, switches on cast that is still disabled, and drives the camera. |
| Quest mirror | `runtime/quest_state_cache.h` | A main-thread mirror of live quest state (stage, running, stages done) so the systems that own the world can evaluate the records' condition gates without marshalling onto the Papyrus guest thread. |

## How a scene starts

The same two ways the game starts one:

* **`Scene.Start` from Papyrus.** A quest stage fragment calls `MyScene.Start()`
  (MQ101 alone does this for 14 of its scenes). The native queues the call for the
  main thread, because playing a scene means moving actors, playing audio and taking
  the camera. `Scene.Stop` / `Scene.IsPlaying` answer against live playback.
* **The "begin on quest start" flag.** 956 of the game's 2130 scenes carry SCEN
  flag `0x01` and need no script at all: they start when their quest comes online.
  Skyrim's cart ride (`MQ101Scene1`) is one of them, and so is nearly every ambient
  town conversation.

## Running one

```
RX_CUTSCENE=MQ101 ./run-skyrim.sh          # start a quest and watch its scenes
RX_CUTSCENE=DialogueRiftenKeepScene11 ./run-skyrim.sh --interior RiftenMistveilKeep
RX_CUTSCENE_REPORT=MQ1 ./build/nix/runtime/recreation --headless --data-dir <Data>
```

| Knob | Default | Effect |
| --- | --- | --- |
| `RX_CUTSCENE=<quest editor id>` | off | Starts that quest at load, boots the world where its first scene plays, and lets its scenes take the camera at any range (any quest, scripted or not). |
| `RX_CUTSCENE_REPORT=<prefix\|all>` | off | Headless: lowers every matching quest's scenes and reports cast, phases, beats, spoken lines, voiced lines, package beats, running time and where the scene plays. |
| `RX_SCENE_CAMERA` | on | The dialogue camera. Off leaves scenes playing in the gameplay view. |
| `RX_SCENE_VOICE` | on | Voice playback. Off falls back to reading-time pacing. |
| `RX_SCENE_LETTERBOX` | on | Cinematic bars while a scene owns the view. |
| `RX_CAPTURE_OFFSCREEN=1` | off | Renders captures offscreen, for screenshots on an unattended desktop (a compositor that stops compositing the window otherwise hands back a garbage frame). |

Esc hands the camera back mid-scene; the scene keeps playing.

## Coverage

`RX_CUTSCENE_REPORT=all` over Skyrim + Update + Dawnguard + HearthFires + Dragonborn:

```
2130 scene(s) over 1281 quest(s), 956 begin with their quest
8329 spoken line(s), 6947 with a voice clip (83%)
  12 of the rest name a clip the archives do not hold; the other 1371 have no
  recording at all
```

Every one of those 2130 scenes lowers to a runnable plan: phases sorted, cast
resolved through the quest's alias table, dialogue resolved to real INFO records and
subtitle text, packages resolved to PACK handles, phase gates transpiled from CTDA.
An unvoiced line still plays, timed by its reading length.

![Mistveil Keep](cutscene-mistveil-keep.png)

`RiftenKeepScene11` playing in Mistveil Keep: the scene started itself with its
quest, the dialogue camera framed the exchange, and the caption is the INFO's own
text under the speaker's real name, timed by her voice clip.

## Verified quests

"Played live" means the engine was run on the real game data, the scene started
itself, its lines were spoken in the authored order with their voice clips, and the
journal moved where the scene's fragments move it. "Content verified" means the same
data path was exercised headlessly by the coverage report (cast, phases, lines,
clips, packages all resolved) without a live playthrough of that quest.

| Quest | Name | Scenes | Lines | Voiced | Status | Evidence |
| --- | --- | --- | --- | --- | --- | --- |
| MQ101 | Unbound | 17 | 197 | 152 | Played live | `MQ101Scene1` (29 phases) speaks the whole cart ride in order, from "Hey, you. You're finally awake." to "Sovngarde awaits."; journal runs 0 -> 10 -> 15; the cart horse runs its patrol packages and tows the cart |
| DialogueRiftenKeepScene11 | Riften Keep Scene 11 | 1 | 10 | 10 | Played live | Laila and Maven's audience plays on entering Mistveil Keep, voiced |
| DialogueRiftenBeeAndBarbScene01 | Bee and Barb Scene 01 | 1 | 4 | 4 | Played live | Talen-Jei and Keerava behind the bar, framed and voiced (screenshot below) |
| MG01 | First Lessons | 7 | 43 | 43 | Played live | Mirabelle/Ancano scene starts with the quest and speaks its lines |
| DialogueMorthalInitialScene | Morthal | 1 | 7 | 0 | Played live | Jorgen/Thonnir/Aslfur argue in Highmoon Hall (unvoiced, reading-time paced) |
| DialogueRiftenSS02Scene | Riften | 1 | 4 | 4 | Played live | Mjoll and Aerin on the Thieves Guild |
| DialogueIvarsteadSSScene | Ivarstead | 1 | 4 | 0 | Played live | Klimmek and Gwilin on the 7000 Steps |
| DialogueRiftenMaul | Riften Maul | 1 | 1 | 1 | Played live | Maul's forcegreet scene starts and completes |
| T01 | The Book of Love | 1 | 2 | 2 | Played live | Markarth Temple of Dibella protocol scene |
| MS08 | In My Time Of Need | 5 | 12 | 9 | Played live | `RJBanditScene` plays in Swindlers Den with its cast framed and upright |
| CW00SolitudeMapTableScene | Civil War council | 1 | 15 | 14 | Played live | Legate Rikke opens the war-table scene in Castle Dour ("Ulfric's planning an attack on Whiterun") and the camera takes it |
| MQ302 | Season Unending | 26 | 178 | 170 | Content verified | Report: every scene of the peace council lowers, 170 clips resolve |
| MQ104 | Dragon Rising | 7 | 99 | 59 | Content verified | |
| MQ203 | Alduin's Wall | 5 | 82 | 79 | Content verified | |
| MQ201 | Diplomatic Immunity | 10 | 57 | 49 | Content verified | Party scenes carry their own quest (`MQ201Party`, 7 scenes) |
| MQ206 | Alduin's Bane | 2 | 53 | 48 | Content verified | |
| MQ105 | The Way of the Voice | 5 | 46 | 44 | Content verified | |
| CW02A/B | The Jagged Crown | 15 | 133 | 99 | Content verified | |
| CW03 | Message to Whiterun | 3 | 73 | 45 | Content verified | Includes `CW03SiegeScene` (29 phases) |
| MG07 | The Staff of Magnus | 18 | 86 | 77 | Content verified | |
| MS04 | Unfathomable Depths | 13 | 50 | 50 | Content verified | |
| DarkBrotherhood | Dark Brotherhood | 6 | 42 | 42 | Content verified | |
| TG08B | Thieves Guild | 19 | 43 | 42 | Content verified | |
| DLC2SV01 | Dragonborn (Skaal) | 27 | 64 | 64 | Content verified | |
| DialogueWhiterun | Whiterun City Dialogue | 9 | 48 | 48 | Content verified | The town's ambient conversations |

`RX_HELGEN_INTRO` (the hand-directed cart ride that predates this) still exists and
still runs; it spawns its own cart, horse and riders instead of using the placed
references. It goes away once the staging gap below is closed.

![Bee and Barb](cutscene-bee-and-barb.png)

`RiftenBeeAndBarbScene01`: Talen-Jei mid-line in the inn, on the shot the director
picked. His four lines were unvoiced until the archive index went in.

## Fixed along the way

The frame's skinning palette is a fixed budget (8192 matrices) and a dense exterior
holds more actors than fits; whoever overflowed it drew from memory nothing had
written. Actors are now emitted nearest-camera-first and the loop stops at the
budget, so the ones on screen are the ones that get skinned.

The cutscene camera also lifts until its line to the subject is clear, so a shot
solved off two heads on a slope does not frame the hillside in front of them.

Every placed actor in every game was lying on its back. A REFR's rotation was
converted with the axis change that turns Bethesda's Z-up space into the engine's
Y-up, but a skinned actor already converts its Bethesda-space skeleton itself, so the
two composed and tipped the body 90 degrees. Placed actors now carry a plain yaw
about engine up (the only axis the games rotate an actor about), and the shared NPC
template holds the game's own standing idle (`mt_idle_a_base.hkx`) instead of the
procedural gait, which is authored against the builtin biped's bones.

## Known gaps

* **17% of spoken lines have no recording.** 1371 of 8329 are lines Bethesda never
  voiced, and 12 name a file the archives do not hold. They play, paced by their
  reading length.
* **Find-matching aliases still do not bind.** Forced references, unique actors and
  created objects (ALCO) bind to the instance in the world, so their scenes get a
  camera. A find-matching alias (ALFA, "any reference of this ref type inside this
  location") needs the location's ref-type table walked before it can point at
  anyone; the director now hands the scene's location to the quest system's
  find-matching fill, which covers the ones whose location carries a ref-type table,
  and the rest still play as voice and subtitles in the gameplay view.
* **MQ101's staged cast does not draw.** The scene runs (dialogue, packages,
  journal), and the actor it stages on the mountain road resolves, is enabled, is
  streamed, is upright, holds seven drawable parts with a valid mesh, sits on the
  ground and is handed to the renderer with the right model matrix
  (`RX_ACTOR_DUMP=1` / `RX_SKEL_DUMP=1` print it: `model t=(324.39 199.06 1361.98)`),
  yet nothing appears there, from a metre away, from above, or from any angle. Ruled
  out along the way: the pose, the rotation, burial in the terrain, duplicate
  entities, physics occlusion, the entity/instance mapping, and the renderer's
  per-frame bone budget. What is left is inside the renderer's handling of that
  draw, which lives in the `rx` repository rather than this one.
* **The cart ride stops after its first leg.** The horse walks `MQ101CartHorse1Patrol1`
  and arrives; the next leg is gated on a journal stage that vanilla reaches with
  the player aboard the cart tripping the quest's own triggers. Riding the cart as
  the player is the missing gameplay path, not a missing system.
* **Phase completion conditions** are evaluated against the main-thread quest
  mirror, which answers stage functions exactly and distance functions from live
  positions; anything else reads as 0 and falls to the phase timeout.
