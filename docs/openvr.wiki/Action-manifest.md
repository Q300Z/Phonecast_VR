# Action Manifest files

OpenVR uses a JSON file called the "action manifest" to allow developers to provide certain input details about their applications. Specifically, this file contains:

* The list of action sets used by the application
* The list of actions in each of those action sets
* Default binding files for each controller type
* Localized strings for each action set and action name

This file can be located anywhere on disk under the application's install directory. The application tells OpenVR where the file is with the `IVRInput::SetActionManifestPath()` function. The full path to the file must be provided; relative paths are not accepted. 

If the path to the action manifest is also set in the Steam Partner site, the two paths must match. If there is a mismatch, the path provided in the partner site is used, and the call to `IVRInput::SetActionManifestPath` returns `VRInputError_MismatchedActionManifest`. In this mismatch case the manifest path defined by the Steam Partner site will be used by the Application.

# Telling SteamVR about your action manifest when the app isn't running

While you application is running and has called `vr::VRInput()->SetApplicationManifestPath(...)`, SteamVR knows that your application is using the new input system and where the action manifest file is located. At all other times, SteamVR assumes that the application is using the legacy input system unless it is told otherwise. That means users will see one set of bindable actions when the app is running and another when it is not running. To fix this, you need to tell SteamVR where to find the action manifest path for your application.

If your application is distributed on Steam, you should let Steam know where the your action manifest file is. Steam will share that information with SteamVR automatically.  This setting is under Application->Virtual Reality in the Steamworks settings for your application on the Steam partner site. Click the option labelled "This game uses the SteamVR Input System and an action manifest file" option, and then put in the path of your action manifest file relative to your app's install directory.

If your application is distributed outside of Steam, you can set the action manifest path in the entry for your application in a .vrmanifest file:
> `"action_manifest_path": "TheLab\\win64\\actions.json"`

In this case, both forward and backward slashes are accepted. Relative paths are relative to the directory that contains the .vrmanifest file itself. Absolute paths are also accepted here.


# Action manifest file format
```JSON
{
  "default_bindings": [
   {
      "controller_type": "some_controller",
      "binding_url": "mygame_bindings_some_controller.json"
   }
  ], 
  "actions": [
    {
      "name": "/actions/main/in/OpenInventory",
      "requirement": "mandatory",
      "type": "boolean"
    },
    {
      "name": "/actions/driving/in/HonkHorn",
      "requirement": "optional",
      "type": "boolean"
    },
    {
      "name": "/actions/driving/out/SpeedBump",
      "type": "vibration"
    },
    {
      "name": "/actions/driving/in/Throttle",
      "requirement" : "suggested",
      "type": "vector1"
    },
    {
      "name": "/actions/main/in/RightHand",
      "type": "pose"
    },
    {
      "name": "/actions/main/in/RightHand_Anim",
      "type": "skeleton",
      "skeleton": "/skeleton/hand/right"
    }
  ],
  "action_sets": [
    {
      "name": "/actions/main",
      "usage": "leftright"
    },
    {
      "name": "/actions/driving",
      "usage": "single"
    }
  ],
  "localization" : [
   {
       "language_tag": "en_us",

        "/actions/main" : "My Game Actions",
        "/actions/driving" : "Driving",

        "/actions/main/in/OpenInventory" : "Open Inventory",
        "/actions/main/in/RightHand" : "Right Hand",
        "/actions/main/in/RightHand_Anim" : "Right Hand Animation",

        "/actions/driving/in/HonkHorn" : "Honk Horn",
        "/actions/driving/out/SpeedBump" : "Hit Speed Bump",
        "/actions/driving/in/Throttle" : "Throttle"
    }
  ]
}
```

## Default_bindings
This array contains the list of binding files to load for this application. 

**controller_type** - This is the name of the controller type that this binding file is for. Common controller types are:
* frame_controller
* knuckles
* vive_controller
* vive_cosmos_controller
* oculus_touch
* holographic_controller
* gamepad
* hpmotioncontroller

Controller types are defined by controller device vendors. For your Application you should attempt to author a binding for common controllers being used.

**binding_url** - The URL or relative file path of the binding config file for this controller type. Relative paths are relative to the action manifest JSON file itself, so files in the same directory only need to provide a filename. Relative paths may not contain ".." , you must load your binding from the same directory as your action file or a sub-directory.

## Actions
This array contains a list of all actions used by the application. Actions can be listed in any order.

**name** - The path to an action. Paths take the form /actions/_actionsetname_/in/_actionname_ for input actions or /actions/_actionsetname_/out/_actionname_ for output actions (like haptics.) This path is passed to `IVRInput::GetActionHandle`

**requirement** - The degree to which the user should be prompted to bind this action in the binding editor. This must be one of:
* mandatory - The action must be bound or the binding file cannot be saved. Use this sparingly.
* suggested - The user will get a warning if this action is not bound. This is the default if no requirement is specified.
* optional - The user can bind this action, but will not be warned if it is not bound. This is often used for actions that are cannot be bound on all hardware, or actions of a secondary nature.

**type** - The type of the action. This must be one of:
* boolean - The action is a simple on/off event like pulling a trigger or pressing a switch.
* vector1, vector2, vector3 - The action is a 1, 2, or 3 dimensional float. These are used for throttles, smooth motion, position on a trackpad surface, etc.
* vibration - The action is a output haptic vibration. Include one of these per type of haptic output in your application so they can be bound to different output devices if the user has them available. Examples include haptics due to firing a gun (that would usually be bound directly to a controller) or haptics from getting shot (which could be bound to a feedback vest worn on the chest).
* pose - The action is the 6-DOF position and orientation of a device tracked by the tracking system. 
* skeleton - The action is used to retrieve bone transform data from controllers that support skeletal animation. Actions of type Skeleton also need to set the **skeleton** parameter to identify which skeleton they are expecting. The supported skeletons are /skeleton/hand/left and /skeleton/hand/right.
* eyetracking - The action is used to retrieve eye tracking data. This action can be used to call two functions in IVRInput GetEyeTrackingDataRelativeToNow and GetEyeTrackingDataForNextFrame.

## Action Sets
This array contains information about each action set that is used to drive the binding editor.

**name** - The path of the action set. Action set names are of the form /actions/_action set name_

**usage** - The style that should be used in the binding UI for this action set. This must be one of:
* leftright - The user will see left and right hand controllers and be able to bind each one independently.
* single - The user will see just one controller by default and anything bound on the left controller will automatically be copied to the right controller.
* hidden - This action set will not be shown to the user.


## Localization
This array contains localized strings for action and action set names for any number of locales. 

**language_tag** - The language tag is the ISO-639-1 + ISO-3166-1 alpha-2 code for the locale that this section of the action manifest file refers to.

All localization entries use the path of the action or action set as the key and the localized string as the value. These strings will be shown to the user instead of the action or action set name whenever the user is using that language. If the user's language is not present, English strings will be used. Steam supports over 25 languages, users have come to expect that Applications present details in their native language.


## Action Binding Versioning

You can optionally set version information with:

```
"version": x,
"minimum_required_version": y,
```

Where `x` and `y` are positive Integers.

If you set the `minimum_required_version` property, you will also need to update the `action_manifest_version` property in each controller's binding file. The controller's `action_manifest_version` is compared to the `minimum_required_version` so that old bindings (such as ones on the Steam workshop) can be rejected.

The action manifests 'version' number is used in the bindings menu to display the message `These bindings were made with an older version of <appname> and need to be updated.`
