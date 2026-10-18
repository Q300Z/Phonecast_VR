`EVRScreenshotError SubmitScreenshot( ScreenshotHandle_t screenshotHandle, EVRScreenshotType type, const char *pchSourcePreviewFilename, const char *pchSourceVRFilename )`

Submit a new screenshot.

* `ScreenshotHandle_t screenshotHandle` - Handle of the screenshot being submitted.  This can be k_unScreenshotHandleInvalid if this is a new shot being directly submitted.
* `EVRScreenshotType type` - The format type of the screenshot being submitted.
* `const char *pchSourcePreviewFilename` - Full path of the preview of the screenshot image to be copied from.
* `const char *pchSourceVRFilename` - Full path of the screenshot image to be copied from.

**Description**

Submit a completed screenshot.  If Steam is running this will call into the Steam client and upload the screenshot to the screenshots section of the library for the running application.  If Steam is not running, this function will display a notification to the user that the screenshot was taken. The paths should be full paths with extensions.
