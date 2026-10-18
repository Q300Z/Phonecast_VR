The IVRDriverInput interface is used to create and update input-related components. Each tracked device creates input and haptic components to match the controls present on the physical device.

Components are identified by paths that group them together in a hierarchy. Where appropriate, devices should use paths that match existing devices to provide maximum compatibility. 

Devices should also refrain from simulating one kind of input with another (for instance applying a threshold to provide a /input/trigger/click value when no physical switch exists on the hardware.) This will allow SteamVR to provide the user the ability to set the scalar to analog conversion behavior per application instead of using a global hard-coded value.

The component path /input/system/click is a special case that is used to summon or dismiss the SteamVR dashboard. The value of this component will not be available to applications.

Drivers should provide an [Input Profile](https://github.com/ValveSoftware/openvr/wiki/Input-Profiles) for each type of controller they describe. This file describes how the rest of the input system will interact with that device.

**`EVRInputError CreateBooleanComponent( PropertyContainerHandle_t ulContainer, const char *pchName, VRInputComponentHandle_t *pHandle )`**

Creates an input component to represent a single boolean value on a controller or other tracked device. Returns VRInputError_None and sets the value pointed to by pHandle to a valid component handle on success. After creating a component the driver can update it with repeated calls to UpdateBooleanComponent.

* ulContainer - The property container handle of the device that is the parent of this component.
* pchName - The name of the component. All names should be in the form "/input/<control name>/<event name>". See below for examples.
* pHandle - Points to the handle value to set with the new component's handle.



**`EVRInputError CreateScalarComponent( PropertyContainerHandle_t ulContainer, const char *pchName, VRInputComponentHandle_t *pHandle, EVRScalarType eType, EVRScalarUnits eUnits )`**

Creates an input component to represent a single scalar value on a controller or other tracked device. Returns VRInputError_None and sets the value pointed to by pHandle to a valid component handle on success. After creating a component the driver can update it with repeated calls to UpdateScalarComponent.

* ulContainer - The property container handle of the device that is the parent of this component.
* pchName - The name of the component. All names should be in the form "/input/<control name>/<value name>". See below for examples.
* pHandle - Points to the handle value to set with the new component's handle.
* eType - This parameter must be one of:
  * VRScalarType_Absolute - The scalar values are updated with values on an absolute scale. Joysticks, trackpads, and triggers are all examples of absolute scalar values.
  * VRScalarType_Relative - The scalar values are updated with incremental values since the last update. Mice and trackballs are examples of relative scalar values.
* eUnits - Specifies the unit of measurement for the scalar values. Must be one of:
  * VRScalarUnits_NormalizedOneSided - Scalar values range from 0 to 1 inclusively. Triggers and throttles generally use this value.
  * VRScalarUnits_NormalizedTwoSided - Scalar values range from -1 to 1 inclusively. Joysticks and trackpads generally use this value.


**`EVRInputError CreateHapticComponent( PropertyContainerHandle_t ulContainer, const char *pchName, VRInputComponentHandle_t *pHandle )`**

Creates an output component to represent a single haptic on a controller or other tracked device. Returns VRInputError_None and sets the value pointed to by pHandle to a valid component handle on success. 

When the application requests a haptic event, the driver will receive an event of type `VREvent_Input_HapticVibration` and the details of the haptic event will be in the `hapticVibration` member of the event data union.

Note: Applications that use the current haptic API will always target the first haptic component created on a given tracked device. Future APIs will support multiple haptic components per device.

* ulContainer - The property container handle of the device that is the parent of this component.
* pchName - The name of the component. All names should be in the form "/output/<haptic name>". See below for examples.
* pHandle - Points to the handle value to set with the new component's handle.


**`EVRInputError UpdateBooleanComponent( VRInputComponentHandle_t ulComponent, bool bNewValue, double fTimeOffset )`**

Updates the value of a boolean component. This should be called whenever the current state of an input component changes.

* ulComponent - The component handle of the component to update.
* bNewValue - The new boolean value of the component.
* fTimeOffset - The time of the state change in the component relative to now. Negative times are in the past and positive times are in the future. This time offset should include transmission latency from the physical hardware.

**`EVRInputError UpdateScalarComponent( VRInputComponentHandle_t ulComponent, float fNewValue, double fTimeOffset )`**

Updates the value of a scalar component. This should be called whenever the current state of an input component changes.

* ulComponent - The component handle of the component to update.
* fNewValue - The new scalar value of the component.
* fTimeOffset - The time of the state change in the component relative to now. Negative times are in the past and positive times are in the future. This time offset should include transmission latency from the physical hardware.

**Existing input component paths**

Index Controller (from index_controller_profile.json)
* /input/a/click
* /input/a/touch
* /input/b/click
* /input/b/touch
* /input/system/click
* /input/system/touch
* /input/trackpad/x
* /input/trackpad/y
* /input/trackpad/force
* /input/trackpad/touch
* /input/grip/force
* /input/grip/touch
* /input/grip/value
* /input/trigger/touch
* /input/trigger/value
* /input/trigger/click
* /input/thumbstick/click
* /input/thumbstick/touch
* /input/thumbstick/x
* /input/thumbstick/y
* /input/pinch/grab
* /output/haptic

Vive controller:
* /input/system/click
* /input/grip/click
* /input/application_menu/click
* /input/trigger/click
* /input/trigger/value
* /input/trackpad/x
* /input/trackpad/y
* /input/trackpad/click
* /input/trackpad/touch
* /output/haptic

Touch controller:
* /input/system/click
* Left controller only:
  * /input/x/click
  * /input/x/touch
  * /input/y/click
  * /input/y/touch
* Right controller only:
  * /input/a/click
  * /input/a/touch
  * /input/b/click
  * /input/b/touch
* /input/grip/value
* /input/trigger/value
* /input/trigger/touch
* /input/joystick/x
* /input/joystick/y
* /input/joystick/click
* /input/joystick/touch
* /output/haptic

XInput-style controllers:
* /input/a/click
* /input/b/click
* /input/x/click
* /input/y/click
* /input/back/click
* /input/guide/click
* /input/start/click
* /input/dpad_up/click
* /input/dpad_down/click
* /input/dpad_right/click
* /input/dpad_left/click
* /input/joystick_left/x
* /input/joystick_left/y
* /input/joystick_left/click
* /input/joystick_right/x
* /input/joystick_right/y
* /input/joystick_right/click
* /input/trigger_left/value
* /input/trigger_right/value
* /input shoulder_left/click
* /input shoulder_right/click







