If you have your own compositor that you want to interface with SteamVR, you will want your driver to implement IVRDriverDirectModeComponent.

In your implementation of IVRDriverDirectModeComponent::Present, you need to call AcquireSync on the provided syncTexture handle before using the textures submitted via SubmitLayer, and then call ReleaseSync after.

Shared texture handles should only be opened once, rather than repeatedly each frame.  Otherwise, I’ve seen drivers get confused after a while and things stop working.

You can see a similar example in my IVRVirtualDisplay sample implementation here:
https://github.com/ValveSoftware/virtual_display/blob/master/driver_virtual_display/driver_virtual_display.cpp#L508

Note: This is a different interface than IVRDriverDirectModeComponent, but the above two rules still apply (just to different textures).  You only need to call AcquireSync/ReleaseSync on the one provided syncTexture handle (as opposed to each of the individual ones submitted through SubmitLayer).

The sample also provides a simple example of caching shared texture handles here:
https://github.com/ValveSoftware/virtual_display/blob/master/shared/d3drender.cpp#L207

Then in your implementation of PostPresent, you will want to do three things:
1)	Block until “running start”.
2)	Sample and submit your tracked device poses via IVRServerDriverHost::TrackedDevicePoseUpdated.
3)	Call IVRServerDriverHost::VsyncEvent with the time in seconds until the upcoming vsync (so SteamVR can apply accurate prediction to the supplied poses for rendering).
