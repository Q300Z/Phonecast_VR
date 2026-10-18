# Input Overview

To read controller input state in OpenVR, an application provides a list of the "actions" that represent the operations a user can perform in the application. SteamVR then binds those actions to actual inputs on a game/vr controller, or more specifically input components provided for the input devices that the user is using. Application developers can provide default bindings for any controller types they like. Users are able to modify those bindings or create new bindings for new devices. Users can also select bindings that were shared by other users.

# Getting Started

To use the IVRInput API in an application, a developer should take the following steps:

1. Check "Enable debugging options in the input binding user interface" under Developer settings in SteamVR. This will enable some options that will be used in later steps.
1. Create an [action manifest file](https://github.com/ValveSoftware/openvr/wiki/Action-manifest) to allow SteamVR to know what actions exist in the application.
2. Add a call to `vr::VRInput()->SetActionManifestPath()` to the application's initialization. This function takes an absolute path to the application's action manifest file. 
3. Add several calls to `vr::VRInput()->GetActionHandle()` to the application's initialization. These calls turn action paths (e.g. /actions/my_app/in/DoSomething) into handles that can be used at runtime.
4. Add one or more calls to `vr::VRInput()->GetActionSetHandle()` to the application's initialization. These calls turn action set paths (e.g. /actions/my_app) into handles that can be used at runtime.
5. Add a call to `vr::VRInput()->UpdateActionState()` at the top of the application's input processing code before reading any action state.
6. Add calls to `vr::VRInput()->Get*ActionData()` for each action used by the application. Make the results from those calls perform the action in the application.
7. Run the application.
8. From the SteamVR menu, Select "Devices"->"Controller Input Binding" and select your application, which should be listed as the current one. The system should list "No Bindings Found", but will allow you to create a new one for editing.
9. Make some bindings using the UI and then click "Replace Default Binding" at the bottom of the screen. The changes will be saved out to the folder where the action manifest JSON file was placed. Usually putting the bindings next to the action manifest file is he best choice.
10. Edit the action manifest and add a `default_bindings` section that refers to your default binding file. 
11. Repeat 8-10 for other controller types you want to provide default bindings for.
12. Include your action manifest and all your default binding files in your app when it releases.

# API Documentation


`EVRInputError IVRInput::SetActionManifestPath( const char *pchActionManifestPath )`

Sets the path to the action manifest JSON file that is used by this application. If this information
was set on the Steam partner site, calls to this function are ignored. If the Steam partner site
setting and the path provided by this call are different, `VRInputError_MismatchedActionManifest` is returned. 
This call must be made before the first call to `IVRInput::UpdateActionState` or `IVRSystem::PollNextEvent`.

* `pchActionManifestPath` - Absolute path to the action manifest file to use for this application. See [action manifest file](https://github.com/ValveSoftware/openvr/wiki/Action-manifest) documentation for the format of these files.


`EVRInputError IVRInput::GetActionSetHandle( const char *pchActionSetName, VRActionSetHandle_t *pHandle )`

Returns a handle for an action set. This handle is used for all performance-sensitive calls.

For best performance, applications should call this function once per action set name and save the returned handle for later use.

* `pchActionSetName` - The path to the action set. This is generally of the form /actions/action_set_name
* `pHandle` - A pointer to an action set variable to fill with the handle.

`EVRInputError IVRInput::GetActionHandle( const char *pchActionName, VRActionHandle_t *pHandle )`

Returns a handle for an action. This handle is used for all performance-sensitive calls.

For best performance, applications should call this function once per action name and save the returned handle for later use.

* `pchActionName` - The path to the action. This is generally of the form /actions/action_set_name/in/action_name or /actions/action_set_name/out/action_name.
* `pHandle` - A pointer to an action handle variable to fill with the handle.

`EVRInputError IVRInput::GetInputSourceHandle( const char *pchInputSourcePath, VRInputValueHandle_t  *pHandle )`

Returns a handle for an input source. This handle is used for all performance-sensitive calls.

For best performance, applications should call this function once per input source path they use.

* `pchInputSourcePath` - The path to the input source. Common input sources are /user/head, /user/hand/left, /user/hand/right, and /user/gamepad.
* `pHandle` - A pointer to an input source handle variable to fill with the handle.

`EVRInputError IVRInput::UpdateActionState( VRActiveActionSet_t *pSets, uint32_t unSizeOfVRSelectedActionSet_t, uint32_t unSetCount )`

Reads the current state into all actions. After this call, the results of Get*ActionData calls will be the same until the next call to UpdateActionState. Applications should typically call this function once per frame.

* `pSets` - A pointer to an array of one or more VRActiveActionSet_t structures that the application would like to be active this frame.
* `unSizeofVRSelectedActionSet_t` - This parameter should be sizeof(VRActiveActionSet_t).
* `unSetCount` - The number of action VRActiveActionSet_t structs in the array.

```
struct VRActiveActionSet_t
{
	VRActionSetHandle_t ulActionSet;
	VRInputValueHandle_t ulRestrictedToDevice;
	VRActionSetHandle_t ulSecondaryActionSet;
};
```

This structure describes one active action set for a given frame.

* `ulActionSet` - The handle of an action set to activate.
* `ulRestrictedToDevice` - The device to restrict `ulActionSet` to. This is generally k_ulInvalidInputValueHandle. If it set, it could be the handle for /user/hand/left or /user/hand/right. This lets you bind different action sets to individual controllers (hands).
* `ulSecondaryActionSet` - If ulRestrictedToDevice is set, this is the alternate action set to use for all other devices in the system.

`EVRInputError IVRInput::GetDigitalActionData( VRActionHandle_t action, InputDigitalActionData_t *pActionData, uint32_t unActionDataSize )`

Reads the state of a digital action given its handle. This will return VRInputError_WrongType if the type of
action is something other than digital.

* `action` - The handle of the action to retrieve state for
* `pActionData` - Pointer to an InputDigitalActionData_t struct. 
* `unActionDataSize` - This must be sizeof(InputDigitalActionData_t)


```
struct InputDigitalActionData_t
{
	bool bActive;
	VRInputValueHandle_t activeOrigin;
	bool bState;
	bool bChanged;
	float fUpdateTime;
};
```

* `bActive` - Is set to True if this action is bound to an input source that is present in the system and is in an action set that is active from the last UpdateActionState call.
* `activeOrigin` - The input source that this action state was generated by. If this action is bound to multiple inputs, this will be the input that changed most recently.
* `bState` - The current state of this digital action. True means the user wants to perform this action.
* `bChanged` - If this field is true, the digital action's state value has changed since the previous frame. As a result, applications can use this to detect rising edges (bState && bChanged) or falling edges (!bState && bChanged) to avoid duplicate activations on steady states.
* `fUpdateTime` - The time relative to now when the action was last changed. This can be passed to GetPoseActionData to get the pose of a user's hand at the time they pressed or released an input control.


`EVRInputError IVRInput::GetAnalogActionData( VRActionHandle_t action, InputAnalogActionData_t *pActionData, uint32_t unActionDataSize )`

Reads the state of an analog action given its handle. This will return VRInputError_WrongType if the type of
action is something other than analog.

* `action` - The handle of the action to retrieve state for
* `pActionData` - Pointer to an InputAnalogActionData_tstruct. 
* `unActionDataSize` - This must be sizeof(InputAnalogActionData_t)


```
struct InputAnalogActionData_t
{
	bool bActive;
	VRInputValueHandle_t activeOrigin;
	float x, y, z;
	float deltaX, deltaY, deltaZ;
	float fUpdateTime;
};
```

* `bActive` - Is set to True if this action is bound to an input source that is present in the system and is in an action set that is active from the last UpdateActionState call.
* `activeOrigin` - The input source that this action state was generated by. If this action is bound to multiple inputs, this will be the input that changed most recently.
* `x, y, z` - The current state of this axis of the analog action. x is valid for vector1, vector2, and vector3 actions. y is valid for vector2, and vector3 actions. z is valid for vector3 actions.
* `deltaX, deltaY, deltaZ` - The change in this axis for this action since the previous frame.
* `fUpdateTime` - The time relative to now when the action was last changed. This can be passed to GetPoseActionData to get the pose of a user's hand at the time they pressed or released an input control.


`EVRInputError IVRInput::GetPoseActionData( VRActionHandle_t action, ETrackingUniverseOrigin eOrigin, float fPredictedSecondsFromNow, InputPoseActionData_t *pActionData, uint32_t unActionDataSize )`

Gets the pose of the pose action as of the prediction time.

* `action` - The handle of the pose or skeleton action to read the position for.
* `eOrigin` - The tracking origin to return a pose relative to
* `fPredictedSecondsFromNow` - The time to generate a pose for. Pass in 0 to get poses relative to the most recent call to WaitGetPoses
* `pActionData` - A pointer to a InputPoseActionData_t struct
* `unActionDataSize` - Must be sizeof(InputPoseActionData_t)

```
struct InputPoseActionData_t
{
	bool bActive;

	VRInputValueHandle_t activeOrigin;

	TrackedDevicePose_t pose;
};
```

* `bActive` - Is set to True if this action is bound to an input source that is present in the system and is in an action set that is active from the last UpdateActionState call.
* `activeOrigin` - The input source that this action state was generated by. If this action is bound to multiple inputs, this will be the input that changed most recently.
* `pose` - The pose of the action.

```EVRInputError TriggerHapticVibrationAction( VRActionHandle_t action, float fStartSecondsFromNow, float fDurationSeconds, float fFrequency, float fAmplitude )```

Triggers a haptic vibration action.

* `action` - The action to trigger
* `fStartSecondsFromNow` - When to start the haptic event
* `fDurationSeconds` - How long to trigger the haptic event for
* `fFrequency` - The frequency in cycles per second of the haptic event
* `fAmplitude` - The magnitude of the haptic event. This value must be between 0.0 and 1.0.

`EVRInputError GetActionOrigins( VRActionSetHandle_t actionSetHandle, VRActionHandle_t digitalActionHandle,  VRInputValueHandle_t *originsOut, uint32_t originOutCount )`

Retrieves the action sources for an action. If the action has more origins than will fit in the array, only the number that will fit in the array are returned. If the action has fewer origins, the extra array entries will be set to k_ulInvalidInputValueHandle

`EVRInputError GetOriginLocalizedName( VRInputValueHandle_t origin, char *pchNameArray, uint32_t unNameArraySize )`

Retrieves the localized name of the action source in the current locale. This is of the form "device controller_type input_source". E.g. "Right Hand Knuckles Controller Trigger".

`EVRInputError GetOriginTrackedDeviceInfo( VRInputValueHandle_t origin, InputOriginInfo_t *pOriginInfo, uint32_t unOriginInfoSize )`

Returns information about the tracked device associated from the input source. 

* `origin` - The input source to retrieve information for.
* `pOriginInfo` - A pointer to an InputOriginInfo_t struct.
* `unOriginInfoSize` - This should be sizeof(InputOriginInfo_t)

```
struct InputOriginInfo_t
{
	VRInputValueHandle_t devicePath;
	TrackedDeviceIndex_t trackedDeviceIndex;
	char rchRenderModelComponentName[128];
};
```

* `devicePath` - Input source path for the device. This will typically be the handle for /user/hand/right, /user/hand/right, or /user/head.
* `trackedDeviceIndex` - The tracked device index for the device or k_unTrackedDeviceIndexInvalid.
* `rchRenderModelComponentName` - The name of the component of the tracked device's render model that represents this input source, or an empty string if there is no associated render model component.

`EVRInputError ShowActionOrigins( VRActionSetHandle_t actionSetHandle, VRActionHandle_t ulActionHandle )`

Shows the current binding for the specified action in-headset. At the moment this function shows the entire binding UI, but that behavior will likely change down the road.

`EVRInputError ShowBindingsForActionSet( VRActiveActionSet_t *pSets, uint32_t unSizeOfVRSelectedActionSet_t, uint32_t unSetCount, VRInputValueHandle_t originToHighlight )`

Shows the current binding for the specified action sets in-headset. At the moment this function shows the entire binding UI, but that behavior will likely change down the road.

