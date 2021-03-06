# Introduction
The weapon config allows for a modular configuration of the weapon used during an [experiment](experimentConfigReadme.md). Weapon configurations can either be inlined into the [`experimentconfig.Any`](../data-files/SAMPLEexperimentconfig.Any#L16) or included as independent `.Any` file using the `#include("file.Any")` directive in the experiment configuration file to refer to a `[filename].weapon.Any` file (or any other `.Any` file including a weapon). If you plan to regularly change weapons we suggest using the later approach.

In the future support for a weapon editor might be offered as part of the application.

## File Location(s)
The [`data-files/weapon` directory](../data-files/weapon) contains some default weapon configurations as stand-alone `.Any` files. These file can be included using the supported `.Any` `#include` construct as shown below:

```
"weapon" : #include("[weapon file name].Any");
```

## Interaction with Experiment/Session Level Specification
The "parameter inheritance" structure used for experiment/session level parameters does not carry into the weapon configuration. That is to say, weapon configurations must be _completely_ specified wherever they are provided. For this reason we suggest encapsulating weapons in their own `*.weapon.Any` files and including them using a `#include()` directive.

As an example of this concept, you **cannot** (currently) specify a weapon at the experiment level, then change just one field within this weapon configuration on a per-session basis. Instead you need to _redefine_ this weapon for each session. Using `#include()` directives for weapons avoids this issue since the full weapon configuration is always specified.

# Weapon Config Field Descriptions

This file provides information about the weapon to be used in the experiment. Detailed field descriptions are provided below.

| Parameter Name        |Units      | Description                                                                               |
|-----------------------|-----------|-------------------------------------------------------------------------------------------|
|`maxAmmo`              |shots      | The maximum number of clicks a user can make in any given trial                           |
|`firePeriod`           |s/shot     | The minimum fire period allowed in the session (set this to 0 if you want "laser" mode)   |
|`autoFire`             |bool       | Whether or not the weapon fires when the left mouse is held, or requires release between fire |
|`damagePerSecond`      |damage/s   | The damage done by the weapon per second, when `firePeriod` > 0 the damage per round is set by `damagePerSecond`*`firePeriod`, when `firePeriod` is 0 and `autoFire` is `True` damage is computed based on time hitting the target. |
|`fireSound`            |file       | The filename/location of the audio clip to use for the weapon firing                      |
|`fireSoundVol`         |ratio      | The volume to play the `fireSound` clip with                                              |
|`renderModel`          |bool       | Whether or not a weapon model is rendered in the first-person view                        |
|`modelSpec`            |`ArticulatedModel::Specification` | Any-based specification for the weapon being used                  |
|`renderBullets`        |bool       | Whether or not bullets are rendered from the barrel of the weapon when fired              |
|`bulletSpeed`          |m/s        | The speed of rendered bullets                                                             |
|`renderDecals`         |bool       | Whether or not bullet hole decals are put on misses when `autoFire` is `False` and `firePeriod` > 0 (i.e. not in laser mode) |
|`renderMuzzleFlash`    |bool       | Whether or not a muzzle flash is rendered for the weapon                                  |
|`muzzleOffset`         |`Vector3`(m)| The offset of the muzzle within the weapon frame                                         |

**Example**
```
"maxAmmo": 10000,           // Maximum ammo for the weapon
"firePeriod": 0.5,          // Fire twice a second max
"autoFire": false,          // Automatic firing if mouse is held
"damagePerSecond": 2.0,     // 2 damage/s * 0.5 s/shot = 1 damage/shot
"fireSound": "sound/42108__marcuslee__Laser_Wrath_6.wav",       // The sound to fire
"fireSoundVol": 0.5f,
"renderModel": true,        // Default is false,
"modelSpec": ArticulatedModel::Specification{			        // Default model
	filename = "model/sniper/sniper.obj";
	preprocess = {
		transformGeometry(all(), Matrix4::yawDegrees(90));
		transformGeometry(all(), Matrix4::scale(1.2,1,0.4));
	};
	scale = 0.25;
},
"renderBullets": true,      // Default is false
"bulletSpeed": 1.0,         // Bullets move at 1m/s
"renderDecals": true,       // Draw hit decals on walls for misses
"renderMuzzleFlash": false, // Draw a muzzle flash
"muzzleOffset": Vector3(0.0,-0.8,0.0)        // Add an offset here to correct the barrel location (default is [0,0,0])
```
 
# Weapon Modes and Damage
There were several common "modes" that motivated the weapon configuration information provided in the experimentconfig. These are outlined below:

|  | autoFire = True | autoFire = False |
|-----------------|------------------------|--------------------------|
| **firePeriod = 0** | "laser" mode | unlimited rate semi-auto |
| **firePeriod > 0** | limited rate full auto | limited rate semi-auto |

When using a non "laser" mode weapon, the damage done (per fired round) is computed as `damagePerSecond` * `firePeriod` where damage/s * s/fire = damage/fire.

When using a "laser" mode weapon the `damagePerSecond` is applied iteratively (the weapon does no damage instantaneously). Instead for each frame the laser stays over the target the health is reduced based on the time since the last frame.

# ArticulatedModel Specification
If you want to render a 3D model as the weapon from the FPS view you will need to provide a .Any serializable `ArticulatedModel` specification as the `modelSpec` field. The example below gives some info on how to do this.

    "modelSpec": ArticulatedModel::Specification{
        filename = "model/sniper/sniper.obj";
        preprocess = {
            transformGeometry(all(), Matrix4::yawDegrees(90));>
            transformGeometry(all(), Matrix4::scale(1.2,1,0.4));
            transformGeometry(all(), Matrix4::translation(0.5, 0, 0));
        };
        scale = 0.25;
    }

The `modelSpec` above provides a filename for a `.obj` file for the model, as well as several preprocess steps intended to orient, position, and scale the model correctly.

If the `renderModel` flag within a weapon configuration is `true` a `modelSpec` must be provided in the configuration. If a `modelSpec` is not provided an exception will be thrown at runtime stating this.