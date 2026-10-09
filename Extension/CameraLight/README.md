# Camera light

A light on the camera, the way a filmer's lamp lights the skater at night. It is one of the game's
own lights, so it lights the skater and the ground as the game's street lamps do.

## Using it

Pause menu › **CUSTOM STUFF › PLAYER** › **Camera light**. With it on, the rows under it set:
- **Light intensity**: 25% to 400%, a step a press;
- **Light color**: a named colour, from White, Warm and Cool to Red, Blue or Pink;
- **Red**, **Green**, **Blue**: one channel at a time, 0 to 255 in steps of 32.

The settings are kept between sessions. The console has the same:

- `light on` / `light off`
- `light gain <0.25-4>`: brighter or dimmer than usual (1)
- `light color <red> <green> <blue>`: 0 to 255 each (255 255 255, white, by default)
- `light status`

The light needs the game's character light rig, which the game keeps loaded in its maps (the city
and the megapark have it). Where it is missing, the log says so and the light stays off.

## How it works

Skate has no light that follows the camera. Its character screens use a light rig instead:
`Lighting/Prefabs/Character/PF_LightRig_Ambient`, a prefab of six spot lights. While the camera
light is on, `camera_light_native.cpp` does the following on the client tick:

1. It creates that rig through the engine, the way ReSkate creates its AI skaters
   (`Extension/Skater/ai_skaters.cpp`):
   - finds the blueprint by name in the loaded assets;
   - gives it an entity bus of its own;
   - collects the rig's entities in a creation list and initializes them together.
2. For the creation only, it gives the rig's first light a filmer's cone, range, brightness and
   colour, and makes the other five dark. About a second later it sets the rig's data back, so the
   character screens keep their own lighting.
3. Every client tick it places that light on the camera, a little above and to the right of the
   lens, tipped slightly down. The other five lights are put away under the map.
4. Off, or on a brightness or colour change, it destroys the rig. A change then creates a new one.

The light's values (`lamp_*` in `camera_light_native.cpp`) were tuned in play. The rig's layout and
the light entity's transform setter belong to this build of the game:
`Engine/Game/Build/20260929/camera_light.h`, whose setter is checked by its code before use. The
spot light data's fields and their offsets (`Color` at +0x90 among them) come from
`dingosdk_hall_of_meat_skeleton_dump <Skate folder> --dump Lighting/Prefabs/Character/PF_LightRig_Ambient`.

`light status` and the pause menu read `camera_light.cpp`, which keeps the settings in
`%LOCALAPPDATA%\ReSkate\cameralight.json`.
