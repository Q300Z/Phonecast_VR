`uint32_t GetScreenshotPropertyFilename( ScreenshotHandle_t screenshotHandle, EVRScreenshotPropertyFilenames filenameType, char *pchFilename, uint32_t cchFilename, EVRScreenshotError *pError )`

Get the filename associated with a screenshot.

* `ScreenshotHandle_t screenshotHandle` - Handle to the screenshot that the filename is being requested. 
* `EVRScreenshotPropertyFilenames filenameType` - Which filename is being requested.  Can be either VRScreenshotPropertyFilenames_Preview or VRScreenshotPropertyFilenames_VR.
* `char *pchFilename` - An application-provided buffer to put the filename in. 
* `uint32_t cchFilename` - The size of the provided buffer.
* `EVRScreenshotError *pError` - Pointer to the error to be returned.  VRScreenshotError_BufferTooSmall will be set if the buffer is not large enough.  The return value will provide the necessary size.

**Description**

When your application receives a VREvent_RequestScreenshot event, call this function to get the filenames of the screenshot requested.  

Returns the size of buffer required to store the filename, including the terminating null character.
