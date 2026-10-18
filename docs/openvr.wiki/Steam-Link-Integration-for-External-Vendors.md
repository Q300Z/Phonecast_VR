There is experimental support for third-party vendors to provide their own resources in SteamVR to support Steam Link streaming from their respective headsets.

The integration is provided through a SteamVR driver called a "resource-only driver". This is needed for VRLink to find the appropriate resources for loading the third-party provided items. These drivers are well-supported by SteamVR, and can either be shipped independently and registered through [VRPathReg](https://github.com/ValveSoftware/openvr/blob/master/docs/Driver_API_Documentation.md#building--development-environment) to automatically be loaded when SteamVR starts, or can be shipped directly on Steam, which will handle registering the driver for you. When going through the Steam app process, make it clear in the submissions that this is a SteamVR driver.

Examples of resource-only drivers can be found in the drivers folder of SteamVR, specifically:

    indexcontroller
    tundra_labs

## Setup

Create a folder, and give it the name that can represent your family of headsets. For example, Pico's would be pico , Meta's meta , etc. This will be the main driver folder where all your resources will live. Inside it you will need a driver.vrdrivermanifest file. Specify the following contents:
```
{
	"alwaysActivate": true,
	"name" : "<your root driver folder name>",
	"directory" : "",
	"resourceOnly" : true,
	"hmd_presence" : []
}
```
A resource-only driver is specified by having resourceOnly  set to true.

The driver file structure for these files, icons, rendermodels, etc. follows the same structure as all drivers. See https://github.com/ValveSoftware/openvr/blob/master/docs/Driver_API_Documentation.md#driver-files

## Settings

Steam Link knows when it's running on an unknown headset and will pass back the value provided from android/os/Build/PRODUCT as the headset product name.

You can configure how SteamVR on the host side deals with an unknown headset, by configuring specific settings in your driver. You can add this in your driver's default settings file: `<your root driver folder name>/settings/default.vrsettings`

Steam Link will search for a section in the settings file matching vrlink_<product name> to take settings from. The available settings that are able to be configured per-headset in this way are:

```
"vrlink_<product name>": {
        "enable": true,
        "modelNumber": "My Model Number",
        "trackingSystemName": "shortvendorname",
        "manufacturerName": "Vendor Name",
        "renderModelName": "generic_hmd",
        "inputProfilePath": "{oculus}/input/rift_profile.json",
        "controllerType": "rift",
        "deviceType": "shortvendorname/type",
        "serialNumber": "MYSERIALNUMBER001",
        "resourceRoot": "driver_name",

    	"hasEyeTracking": true,
        "supportsEyeTracking": true,
    	"supports10bit": true,
    	"recommendedRenderWidth": xxx,
    	"recommendedRenderHeight": xxx,
    	"minStreamFormatWidth": xxxx,
    	"maxStreamFormatWidth": xxxx
}
```

recommendedRenderWidth and height should be the panel resolution of the headset. Max stream format width is clamped to a maximum of 1536.

There are common misconceptions about how foveation for streaming on Steam Link works, and many guides online get it wrong resulting in an overall worse experience for users. The size of the foveated region is affected by both resolution and stream width. Keeping the stream width the same while increasing the overall resolution will reduce the size of the region. 

Note that the inputProfilePath and controllerType are keyed and specific for the headset, depsite the confusing name. controllerType should be all lowercase, no spaces (replace with _).

Controllers support can be added in a similar way - on the headset, when Steam Link falls back to XR_KHR_generic_controller , Steam Link knows it's using unsupported controllers and will then search for setting sections matching vrlink_<hmd product name>_<left/right> . The available properties are:

```
"vrlink_<hmd produt name>_controller_<left/right>": {
        "modelNumber": "My Controller Name (Left Controller)",
        "serialNumber": "MySerialNumber001",
        "trackingSystemName": "shortvendorname",
        "renderModelName": "{<driver_name>}/rendermodels/myleftcontroller",
        "inputProfilePath": "{<driver_name>}/input/myinput_profile.json",
        "controllerType": "mydevice_controller",
    	"providesOffsetOverride": true,
    	"offsetPosX": 0.00,
    	"offsetPosY": 0.025,
    	"offsetPosZ": -0.035,
    	"offsetDegX": 25.00,
    	"offsetDegY": 0.00,
    	"offsetDegZ": 0.00
}
```

Input profiles and render models can then be specified in a similar manner to regular SteamVR drivers. It's important you create your own controller profiles and don't rely on existing profiles for contorllers that SteamVR doesn't currently support. Please refer to the openvr driver documentation for further information and how to configure this.

The renderModelName is taken from a folder in the rendermodels  folder of the driver directory, where the name of the rendermodel is a folder containing the rendermodel assets in rendermodels/ . For example, this could be valve_controller_knu_1_0_left in the case of the indexcontroller  driver. Controller type should be specific to a controller set, for example pico_controller . inputProfilePath  is the path to the input profile, usuaully in <driver dir>/input/<controller name>_profile.json . You may use the wildcard {<driver name>} for specifying the driver directory base path. For example, {indexcontroller}/input/index_controller_profile.json  See https://github.com/ValveSoftware/openvr/blob/master/docs/Driver_API_Documentation.md#input-profiles for further details on how this is structured. 
Icons

Icons can be provided through the driver.vrresources  file in the resources/  directory. See the OpenVR driver documentation for the structure of this.