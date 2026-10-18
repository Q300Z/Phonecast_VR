The driver manifest file is a json file with the name "driver.vrdrivermanifest" that lives at the root of the driver path. It identifies the location of the driver binaries (relative to the manifest file) and several other attributes of the driver.

Here is the driver manifest file from the sample driver:
```json
{
	"alwaysActivate": false,
	"name" : "sample",
	"directory" : "",
	"resourceOnly" : false,
	"hmd_presence" :
	[
		"*.*"
	]
}
```

The fields available to set in the manifest file are:
* `name` - The globally-unique name of the driver. The driver DLL(s) must be called driver_<name>.dll (or the equivalent extension for other platforms. The driver name is the name of the directory that contains the driver.vrdrivermanifest file if it is not specified.
* `directory` - The name of the directory that contains the rest of the driver files. If this is a relative path it is relative to the directory that contains driver.vrdrivermanifest. Defaults to the full path contains driver.vrdrivermanifest.
* `alwaysActivate` - If this is true this driver will be activated even if the active HMD is from another driver. Defaults to false.
* `resourceOnly` - The driver contains a resources directory, but no any actual binaries. 
* `hmd_presence` - This is an array of strings that identify the USB VID and PID combinations that indicate an HMD from this driver is probably present. Each entry should be hex values in this form:
  * 28DE.*  - Any USB device with a VID of 28DE will cause OpenVR to return true from VR_IsHmdPresent
  * 28DE.2000 - Any USE device with a VID of 28DE and a PID of 2000 will cause OpenVR to return true from VR_IsHmdPresent
  * \*.* - OpenVR will always return true from VR_IsHmdPresent if this driver is installed.
* `other_presence` - An array of strings in the same format as hmd_presence that indicates that there is a non-HMD device plugged in.

