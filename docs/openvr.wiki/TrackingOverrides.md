### TrackingOverrides

SteamVR has introduced beta support for a “TrackingOverrides” section within `steamvr.vrsettings` (typically in `C:\Program Files (x86)\Steam\config` on Windows and `~\Library\Application Support\Steam\config` on macOS). This section allows a developer to specify that a particular device overrides the tracking of another device at the system's raw pose level. For example, you can turn SteamVR Lighthouse tracking all the way off for a headset or a controller and provide poses from your own driver. In this example:

```
"steamvr" : {
    "activateMultipleDrivers" : true
},
"TrackingOverrides" : {
    "/devices/mydrivername/1234" : "/user/head"
},
"driver_mydrivername" : {
    "enable" : true,
},
```

A custom `my_driver` which creates a tracked device with serial number `1234` will be used to override poses for the device “/user/head," which is the abstract/semantic name for the currently active headset. In the case of a Lighthouse-tracked headset, this would effectively turns off 6DOF tracking to track using `my_driver`'s tracking mechanism. Other useful 'semantic paths' include “/user/hand/right” and “/user/hand/left” - these are semantic paths to the roles that different devices may take in the system over time.

### How is this useful?

This capability in OpenVR is useful for accurately prototyping new hardware and new tracking techniques.
What is important to understand about this form of tracking overriding is that it is happening at the correct level of the system for forward pose prediction. A common means for developers to prototype an alternate tracking system is to replace the pose returned from WaitGetPoses in C/C++ code or in the SteamVR.unitypackage C# libraries with a pose that is calculated by another tracking system. Although this appears to "work" visually at first, the results are in reality an apples-to-oranges comparison of tracking techniques because overriding just the application's pose does not help the runtime and compositor adjust to different poses. Drivers delivering properly time-stamped raw poses (with as accurate as possible position, orientation and spatial velocity) will be feeding into the correct spot of the forward prediction engine of SteamVR and various parts of the application runtime (WaitGetPoses and other pose APIs) and the compositor (pinning overlays, fine tuning last-minute presents with reprojection).

Several known issues with the current implementation of tracking overrides:

  * **Losing access to the original pose** of the device. E.g. if "/devices/my_driver/1234" is tracked device 3 and "/user/head" is tracked device 0 being tracked by lighthouse, then when "/devices/my_driver/1234" overrides "/user/head" callers will be seeing identical poses for tracked devices 0 and 3 (the poses of tracked device 3) when calling WaitGetPoses. This currently makes it difficult to analyze an alternate tracking mechanism to lighthouse as a ground-truth.
  * **No dynamic handoff**. Ideally this wouldn't be a static override, rather when one tracking mechanism started yielding invalid poses the system could choose an alternate provider of poses, or there could be N poses with different confidence values which the system could weigh. This initial simple override lets us explore what the requirements of dynamic handoff or weighted providers might look be.
  * **No universe rectification**. Each driver has a "universe" representing the coordinate system of the poses their devices exist in. More configuration and override data will be needed from overriding drivers to correlate the coordinate systems of the different coordinate systems / universes.