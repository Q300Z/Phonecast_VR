# Overview

The vr::IVRScreenshots interface provides access to request and submit screenshots that users can view and share in areas such as the VR Dashboard, Steam and other display applications.

The most common screenshot that is a stereoscopic view stored as a side-by-side image.  Other formats such as stereo panoramic, cubemap and mono panoramic are supported.

Each screenshot request has an unique handle that is associated with the screenshot. Users can initiate a screenshot request using a specific input pattern (such as System Button + Trigger button) that will result in an internal call to [RequestScreenshot](https://github.com/ValveSoftware/openvr/wiki/IVRScreenshots::RequestScreenshot).  Applications can also initiate screenshots by calling RequestScreenshot directly as well.  This can be done for common events like when an achievement is earned, etc.

The basic screenshot type of a stereoscopic image is handled internally by the VRCompositor and does not require cooperation from the current VR application.  More sophisticated VR screenshots such as stereo panoramic do require the application to generate them and submit them.  Applications can indicate which screenshots they support by calling [HookScreenshot](https://github.com/ValveSoftware/openvr/wiki/IVRScreenshots::HookScreenshot) with the types they support.  When a screenshot is requested, it will downgrade to the supported types, eventually defaulting to stereoscopic if no other type was hooked.
# Interface Functions

The vr::IVRScreenshots interface provides the following functions:
* [RequestScreenshot](https://github.com/ValveSoftware/openvr/wiki/IVRScreenshots::RequestScreenshot)
* [HookScreenshot](https://github.com/ValveSoftware/openvr/wiki/IVRScreenshots::HookScreenshot)
* [GetScreenshotPropertyType](https://github.com/ValveSoftware/openvr/wiki/IVRScreenshots::GetScreenshotPropertyType)
* [GetScreenshotPropertyFilename](https://github.com/ValveSoftware/openvr/wiki/IVRScreenshots::GetScreenshotPropertyFilename)
* [UpdateScreenshotProgress](https://github.com/ValveSoftware/openvr/wiki/IVRScreenshots::UpdateScreenshotProgress)
* [TakeStereoScreenshot](https://github.com/ValveSoftware/openvr/wiki/IVRScreenshots::TakeStereoScreenshot)
* [SubmitScreenshot](https://github.com/ValveSoftware/openvr/wiki/IVRScreenshots::SubmitScreenshot)
