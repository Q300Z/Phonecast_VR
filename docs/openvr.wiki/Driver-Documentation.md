This is an overview of the steps to write a new OpenVR driver. In the examples, the driver is simply called "mydriver".

It is advisable to use the built-in SteamVR drivers as references. For example, the Oculus driver is maintained by Valve and demonstrates nearly all of the below features (excluding driver registration). The built-in drivers can be found in your Steam install directory in the sub-path 'Steam\steamapps\common\SteamVR\drivers'.

1. Put all the files for your driver under a directory. Let's say that directory is "<install dir>/mydriver"
2. Add a new DLL to "mydriver/bin/win64/driver_mydriver.dll" (For Linux use the appropriate platform directory and the appropriate shared library extension.)
3. Implement the [driver factory function](https://github.com/ValveSoftware/openvr/wiki/Driver-Factory-Function) in that DLL.
4. ~~Add an implementation of [`vr::IClientTrackedDeviceProvider`](https://github.com/ValveSoftware/openvr/wiki/IClientTrackedDeviceProvider_Overview) to the DLL and return it from the factory. This provider will be phased out in the next SDK udpate, but for now you need it.~~ This was removed in [OpenVR version 1.0.6](https://github.com/ValveSoftware/openvr/commit/70acfe9262290ddb789588a7390e5fc60bb20080#diff-614ced34b3fbb27d875cdae21a8a16e6).
5. Add an implementation of [`vr::IServerTrackedDeviceProvider`](https://github.com/ValveSoftware/openvr/wiki/IServerTrackedDeviceProvider_Overview) and have that return implementations of [`vr::ITrackedDeviceServerDriver`](https://github.com/ValveSoftware/openvr/wiki/vr::ITrackedDeviceServerDriver-Overview) for each tracked device.
6. Add a [driver manifest file](https://github.com/ValveSoftware/openvr/wiki/DriverManifest) to "<installdir>/mydriver"
7. Add and remove your driver to SteamVR's config file using [vrpathreg](https://github.com/ValveSoftware/openvr/wiki/Local-Driver-Registration).
8. If you are deploying your driver through Steam and do not have an executable to put in your Steam launch options, you can use a Steam URL to launch SteamVR. This way, if a user launches your driver through Steam, it will automatically start SteamVR. The launch option is: `steam://run/250820`

For devices with buttons, triggers, joysticks, and other kinds of input controls, please refer to the [`vr::IVRDriverInput`](https://github.com/ValveSoftware/openvr/wiki/IVRDriverInput-Overview) API. Reference the [`Input Profiles`](https://github.com/ValveSoftware/openvr/wiki/Input-Profiles) page for information on icons, images, and localization.

For devices that wish to provide animation data through the Skeletal Input system, please refer to the [Skeletal Input Driver](https://github.com/ValveSoftware/openvr/wiki/Creating-a-Skeletal-Input-Driver) documentation.

The source code for a sample driver is available [here](https://github.com/ValveSoftware/openvr/tree/master/samples/driver_sample). The sample implements these interfaces and includes an example HMD/controller.

Other driver documentation:
* [Device Sleep States](https://github.com/ValveSoftware/openvr/wiki/Device-sleep-states)
* [Driver Direct Mode](https://github.com/ValveSoftware/openvr/wiki/Driver-direct-mode)

