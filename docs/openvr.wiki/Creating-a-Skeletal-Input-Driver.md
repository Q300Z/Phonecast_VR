# Overview

If you are developing an OpenVR driver for a device that is capable of detecting what the user's hand is doing, either implicitly or explicitly, you can provide that information to applications as a stream of skeletal animation through the [IVRDriverInput](https://github.com/ValveSoftware/openvr/wiki/IVRDriverInput-Overview) API.  This page walks through how to use the part of the API that is specific to skeletal animation, and discusses some of the things to consider when implementing this functionality.


# Grip Limit Pose
One of the pieces of data that the driver API requires when creating a skeletal component is a grip limit pose.  This is the pose of the hand skeleton (provided as an array of `vr::VRBoneTransform_t` in each bone's parent space) as if it is holding the controller.  The pose is used as a reference both by OpenVR and by applications about how far the user is able to close their hand while holding the controller.  If your input device is not held in the hand or does not limit the range of motion of the user's hand at all, you can pass NULL for the grip limit pose array and the system will know to use the default fist pose when applications request the grip limit.  

# API Documentation
### CreateSkeletonComponent
```
EVRInputError vr::IVRDriverInput::CreateSkeletonComponent( PropertyContainerHandle_t ulContainer, const char *pchName, const char *pchSkeletonPath, const char *pchBasePosePath, EVRSkeletalTrackingLevel eSkeletalTrackingLevel, const VRBoneTransform_t *pGripLimitTransforms, uint32_t unGripLimitTransformCount, VRInputComponentHandle_t *pHandle )
```
Creates a input component to represent skeletal data from the controller or tracked device.  Returns VRInputError_None and sets the value pointed to by pHandle to a valid component handle on success. After creating a component the driver can update it with repeated calls to UpdateSkeletalComponent.

* `ulContainer` - The property container handle of the device that is the parent of this component.
* `pchName` - The name of the component. Valid choices for this option are in the list of Skeletal Input Paths below.  
* `pchSkeletonPath` - The path to the skeleton to use.  Valid choices for this are option are in the list of Skeleton Paths below.  
* `pchBasePosePath` - The path of the location on the controller model that the skeleton should use as its origin
* `eSkeletalTrackingLevel` - This value lets applications understand the capabilities of the controller as far as how it tracks the pose of the user's body.  If the controller supports higher tracking levels, then apps will know that they can take advantage of that functionality.  See [EVRSkeletalTrackingLevel](https://github.com/ValveSoftware/openvr/wiki/SteamVR-Skeletal-Input#evrskeletaltrackinglevel)
* `pGripLimitTransforms` - Array of `vr::VRBoneTransform_t` containing the parent-space transforms for the grip limit pose.  The size should match the number of bones in the skeleton that was specified in pchSkeletonPath.  If this is null, then the system will will the default fist pose as the grip limit.  More info on grip limits below.  
* `unGripLimitTransformCount` - The number of elements in pGripLimitTransforms
* `pHandle` - Pointer to the where the handle for the newly created component should be written


### UpdateSkeletonComponent
```
EVRInputError vr::IVRDriverInput::UpdateSkeletonComponent( VRInputComponentHandle_t ulComponent, EVRSkeletalMotionRange eMotionRange, const VRBoneTransform_t *pTransforms, uint32_t unTransformCount )
```
Updates the pose of a skeletal component to be the values in the given list of transforms.  Returns VRInputError_None on success.  

* `ulComponent` - Handle for the skeletal component to update
* `eMotionRange` - Which skeletal data stream you are providing data for.  More info on this below.  Options are:
    * `VRSkeletalMotionRange_WithController` - The range of motion of the skeleton takes into account any physical limits imposed by the controller itself.  This will tend to be the most accurate pose compared to the user's actual hand pose, but might not allow a closed fist for example
    * `VRSkeletalMotionRange_WithoutController` - Retarget the range of motion provided by the input device to make the hand appear to move as if it was not holding a controller.  eg: map "hand grasping controller" to "closed fist"
* `pTransforms` - Array of bone transforms in parent space for the currently detected pose of the user's hand
* `unTransformCount` - The number of transforms in pTransforms.  Must match the number of bones in the skeleton that is used by this skeletal component, otherwise it will return an error

### Skeletal Input Paths
These are the available choices names for skeletal components
```
/input/skeleton/right
/input/skeleton/left
```

### Skeleton Paths
These are the paths for the currently supported skeletons
```
/skeleton/hand/right
/skeleton/hand/left
```


# Getting Started

Once your driver has been set up and you have the pointer to the `vr::IVRDriverInput` object, the first thing you will need to do is create a skeletal component using `vr::IVRDriverInput::CreateSkeletonComponent`.  This function will tell the system that you wish to provide animation data, specify which skeleton you'll be providing data for, and it will give you a handle you can use later when providing the system with animation data.  

```
// Get a pointer to the driver input object.  
vr::IVRDriverInput* pDriverInput = ...;

// The object you made to contain the properties of your device
vr::PropertyContainerHandle_t ulPropertyContainer = ...;

// Get the number of bones from the spec of the skeleton you plan to use
const uint32_t nBoneCount = 31;

// Create the grip limit pose as appropriate for your device
vr::VRBoneTransform_t gripLimitTransforms[nBoneCount];
YourCreateGripLimitFunction(gripLimitTransforms, nBoneCount);

// Choose the component name and skeleton path
const char* pComponentName;
const char* pSkeletonPath;
if ( IsLeftHand() )
{
    pComponentName = "/input/skeleton/left";
    pSkeletonPath = "/skeleton/hand/left";
}
else
{
    pComponentName = "/input/skeleton/right";
    pSkeletonPath = "/skeleton/hand/right";
}

// Pick the locator on your controller that will act as the skeleton's origin.  
// If your implementation involves artist-created poses, be sure that they are made
// relative to this position
const char* pBasePosePath = "/pose/raw";

// Create the skeletal component and save the handle for later use
vr::VRInputComponentHandle_t ulSkeletalComponentHandle;
vr::EVRInputError err = m_pDriverInput->CreateSkeletonComponent( ulPropertyContainer, pComponentName, pSkeletonPath, pBasePosePath, gripLimitTransforms, nBoneCount , &ulSkeletalComponentHandle);
if ( err != vr::VRInputError_None )
{
    // Handle failure case
    LogError( "CreateSkeletonComponent failed.  Error: %i\n", err );
}
```

Once you've created the component and gotten a handle to it, its up to your driver to provide it with an updated skeletal pose on a regular basis, ideally 90 times a second like the display rate so that the animation is smooth and responsive.  

To update the pose, call `vr::IVRDriverInput::UpdateSkeletonComponent()`.  You'll need to do this twice for each update: once for the WithController animation, and once for the WithoutController animation.  

```

vr::VRBoneTransform_t boneTransforms[nBoneCount];

// Perform whatever logic is necessary to convert your device's input into a skeletal pose,
// first to create a pose "With Controller", that is as close to the pose of the user's real
// hand as possible
GetMyAmazingDeviceAnimation(boneTransforms, nBoneCount);

// Then update the WithController pose on the component with those transforms
vr::EVRInputError err = pDriverInput->UpdateSkeletonComponent( m_ulSkeletalComponentHandle, vr::VRSkeletalMotionRange_WithController, boneTransforms, nBoneCount );
if ( err != vr::VRInputError_None )
{
    // Handle failure case
    LogError( "UpdateSkeletonComponentfailed.  Error: %i\n", err );
}

// Then create a pose "Without a Controller", that maps the detected range of motion your controller
// can detect to a full range of motion as if the user's hand was empty.  Note that for some devices,
// this is the same pose as the last one
GetMyAmazingEmptyHandAnimation(boneTransforms, nBoneCount);

// Then update the WithoutController pose on the component 
err = pDriverInput->UpdateSkeletonComponent( m_ulSkeletalComponentHandle, vr::VRSkeletalMotionRange_WithoutController, boneTransforms, nBoneCount );
if ( err != vr::VRInputError_None )
{
    // Handle failure case
    LogError( "UpdateSkeletonComponentfailed.  Error: %i\n", err );
}
```
