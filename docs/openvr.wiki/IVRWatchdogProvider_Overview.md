Implement `IVRWatchdogProvider` when you want to run some code inside `steam.exe`. This means that when SteamVR is closed, the watchdog driver can still run as long as `steam.exe` process is still running. One example use case is to create a thread inside `Init()` function that will listen to a wake up event for a HMD. Check sample driver for how to do it.

Any driver that implements this interface must build for both 32-bit and 64-bit versions.
`steam.exe` is responsible for loading the watchdog driver (in 32-bit), not `vrserver.exe`. You have to close both Steam and SteamVR before building the DLL.

One trick to build both 32-bit and 64-bit versions is to use **Build ⇾ Batch Build** function in Visual Studio.

Check this issue for more info: https://github.com/ValveSoftware/openvr/issues/1486