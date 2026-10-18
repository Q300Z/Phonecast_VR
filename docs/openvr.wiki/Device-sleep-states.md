SteamVR attempts to manage the sleep state of devices based on user activity. This is particularly helpful for HMDs that may have limited lifetime on their display panels.  There are a few mechanisms that SteamVR will use to determine what constitutes "activity": 
* Motion of the device as determined by the tracking state of that device
* Changing the state of any boolean input component (see [Driver Input]()) on that device
* Launching a new application

Users control the timeouts between the time that activity stops and when the device is told to power off via settings. HMDs and controllers each have their own configurable timeout. The default value for an HMD is 5 seconds, and the default value for a controller is 5 minutes.

There is some special handling of proximity sensors on HMDs. They are communicated to SteamVR by calling `IVRDriverInput::CreateBooleanComponent` with the name "/proximity", and then updated via calls to `IVRDriverInput::UpdateBooleanComponent`. Creating a component with this name also sets the property `Prop_ContainsProximitySensor_Bool` so applications can query to see if a proximity sensor is present on the current HMD. A device will never go to sleep while this component is set to true, no matter how much time has passed since its value changed.


When SteamVR thinks that a device should turn off, it calls `ITrackedDeviceServerDriver::EnterStandby` on the interface provided for that driver. That function is called in three places:
* For all devices when SteamVR exits
* For a device that the user has elected to turn off via the power menu. This option will only be shown to the user if `Prop_DeviceCanPowerOff_Bool` is true for that device.
* When the device has been inactive for longer than its configured timeout.

It is up to the driver to decide what to do with these requests. For wired HMDs, it is probably appropriate to turn off the displays.  For wireless controllers, it may be appropriate to turn off the controller. 