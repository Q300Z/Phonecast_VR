Be sure to read and follow the instructions on creating [driver render models](https://github.com/ValveSoftware/openvr/wiki/Driver-Render-Models). This page is merely a reference document and is not a set of instructions.

## Render Model Guidelines
Each render model OBJ file cannot contain more than 65,000 vertices.
Each render model OBJ file must contain only one object.
Each movable part, such as buttons, joysticks, and triggers, should be in its own OBJ file.
Each OBJ should be associated with an MTL material file, and a PNG or TGA texture file.
Each OBJ should have a correct UV map.

# JSON Render Model Description
A complete render model is described by a JSON file. The JSON file, and all OBJ, MTL, PNG, and TGA files should be in the same directory within your driver.

The render model JSON file has two top-level properties: a thumbnail and a collection of components. We'll use SteamVR's built-in right Knuckles controller (ie. the Valve Index's controller).
The JSON file is `valve_controller_knu_1_0_right.json` and is located in the directory `SteamVR\drivers\indexcontroller\resources\rendermodels\valve_controller_knu_1_0_right`.

```
{
    "thumbnail" : "valve_controller_knu_1_0_right_thumbnail.png",
    "components": {
        ... A big list ...
    }
}
```

The `thumbnail` property should point to a small PNG image of your hardware. This property can be read by applications using the OpenVR IVRRenderModel interface, and may be used outside of SteamVR itself.

The `components` property is a list your OBJ files and describes how they relate to each other. Here is summary of the components used to build the right knuckles controller:

```
// Starts with a list of all the basic parts of the model
"body": {
    "filename": "body.obj",
},
"trigger": {
    "filename": "trigger.obj",
},
"trackpad": {
    "filename": "trackpad.obj",
    "visibility": {},
},
"trackpad_touch": {
    "filename": "trackpad_touch.obj",
},
"status": {
    "filename": "status_right.obj",
},
"led": {
    "filename": "led_right.obj",
},

// These components do not have OBJ files because they are only transforms/locations
// These locations are exposed to applications through OpenVR interfaces
// These poses are documented in ivrrendermodels.h
"tip": { 
},
"base": {
},
"gdc2015": { 
},
"handgrip": {
},
"grip": { 
},
"openxr_handmodel": { 
},

// More basic parts
"squeeze": {
    "filename": "gripsqueeze.obj",
},
"thumbstick": {
    "filename": "thumbstick.obj",
},
"sys_button": {
    "filename": "button_system.obj",
},
"button_a": {
    "filename": "button_a.obj",
},

// These components with the "visibility" property are only displayed conditionally
"trackpad_scroll_wheel": {
    "filename": "trackpad_scroll_wheel.obj",
    "visibility": {},
},
"trackpad_scroll_cut": {
    "filename": "trackpad_scroll_cut.obj",
    "visibility": {},
}
```
This table gives the mapping of OpenVR poses in your JSON file to OpenXR poses:
| OpenVR | OpenXR |
| ----------- | ----------- |
| pose/tip | aim/pose |
| pose/grip | grip/pose |
| pose/Openxr_handmodel | palm_ext/pose |

This table gives the properties for animating the render model:

| Property | Child Property | Notes |
| -------- | -------------- | ----- |
| component_local |         | Describes a transform in global coordinate space to a component's location and local space. Useful for tutorials that want to point at the component. |
|                 | origin  | [x, y, z] - The translation from global to component space |
|                 | rotate_xyz | [x, y, z] - The rotation from global to component space |
| motion |                  | How a component moves based on input |
|        | type | "rotate", "trackpad", "translate" - A motion profile |
|        | controller_axis | The input axis used to control the animation |
|        | controller_axis_component | The component of the input axis used to control the animation |
|        | trigger_path | For triggers, the input path used |
|        | component_path | For non-triggers, the input path used |
|        | value_mapping | How far the component moves in units, mapped from the inputs 0-1 scale. |
|        | pivot | For rotating components, the center of rotation. |
|        | axis | For rotating components, the axis of rotation that passes through the pivot. |
|        | pressed_value_override | Like 'value_mapping' above, but this value is used only when the input is fully depressed. |
|        | center | For trackpads, the pivot point at the center
|        | rotate_xyz | [x, y, z] - For trackpads, the rotation of the plane of the trackpad |
|        | touch_translate_x | [min, max] - For trackpads, the extent of the area to move the trackpad indicator, ie. the bounds of the trackpad. |
|        | touch_translate_y | [min, max] For trackpads, the extent of the area to move the trackpad indicator, ie. the bounds of the trackpad. |
| visibility |              | Describes rules determining if the part should be visible. |
|        | default | true, false - The default visibility (the value should not be in quotes) |
|        | touch | true, false - Should this component be visible if the input is being touched? |
|        | press | true, false - Should this component be visible if the input is being pressed? |
|        | scroll | (trackpad only) true, false - Should this component be visible while the trackpad is scrolling? |

"Trackpad" motion is used to describe a component that translates across a 2D plane. On the knuckles controller, this motion is used to move a small sphere on the trackpad to indicate to the user where their finger is. Note that it is the small sphere that moves (trackpad_touch.obj), not the trackpad itself.