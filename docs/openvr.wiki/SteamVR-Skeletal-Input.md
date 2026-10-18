![](https://steamcdn-a.akamaihd.net/steamcommunity/public/images/clans/5519564/c4de76fcc6edd2380e753fbb7f9333ed76424f9f.png)

# Overview
The Skeletal Input system allows drivers for each type of controller to provide applications with an animated skeleton of the user's hand to the best level of fidelity that its sensors are able to detect.  Developers can then reduce their need to create custom hand animations for each controller they plan to support, and can instead use the animation provided by the current driver in conjunction with their own game-specific animations.  Skeletal data can be bound to input actions, just like other input.  The animation can be requested to be as accurate as possible (which will often mean that the range of motion of the hand will conform to the shape of the controller), or to represent the full range of motion of an empty human hand; in some cases these two options will effectively be the same.  The animation can also be provided as an additive offset from the tightest grip the user is able to make when grasping the controller, so that it can be layered on top of animations that the developer has made.  Finally, functionality is provided to compress and decompress the skeletal pose for the current frame to allow for more efficient transfer to remote clients in a networked multi-user application.  

# Getting Started
### Add an Action and Controller Binding
To use the Skeletal Input API, first add a "skeletal action" entry for each hand to the actions section of your application's [action manifest file](https://github.com/ValveSoftware/openvr/wiki/Action-manifest).  Make sure that the 'type' is skeleton, and that it has an additional 'skeleton' field to define which skeleton the action should use.  The choices for skeleton are either "/skeleton/hand/left" or "/skeleton/hand/right".  For example:

```JSON
{
"name": "/actions/demo/in/lefthand_anim",
"type": "skeleton",
"skeleton": "/skeleton/hand/left"
}
```

Next, you need to add a binding for this action to each controller.  You can do this through the SteamVR binding UI (preferred).  

### Setup the Action
Call `vr::VRInput()->GetActionSetHandle()` as described in the [SteamVR Input](https://github.com/ValveSoftware/openvr/wiki/SteamVR-Input) documentation to get a handle to the skeletal action that you defined in the manifest.  This will later be used to retrieve the skeletal pose associated with the action.  

### Retrieving the Pose
Each frame your application should already be calling `vr::VRInput()->UpdateActionState()` to update the status of all the actions that have been defined in your action manifest.  This will update the skeletal input too.  To retrieve the pose, first call [vr::VRInput()->GetSkeletalActionData()](#GetSkeletalActionData) with the 'VRActionHandle_t' for your skeletal action and a pointer to a [InputSkeletalActionData_t](#InputSkeletalActionData_t) object.  If `InputSkeletalActionData_t.bActive` is true, that means there is bone transform data that is ready.  `InputSkeletalActionData_t.boneCount` will contain the number of bones in the skeleton; you will need an array of at least this many `vr::VRBoneTransform_t` to hold the bone transforms.  

To retrieve the bone transforms, call [vr::VRInput()->GetSkeletalBoneData()](#GetSkeletalBoneData) with the options for the format you want to receive the data in and buffer to place the transform in.  

### Retrieving a Compressed Pose
Instead of using [vr::VRInput()->GetSkeletalBoneData()](#GetSkeletalBoneData) to retrieve the full bone transforms, you can instead call [vr::VRInput()->GetSkeletalBoneDataCompressed()](#GetSkeletalBoneDataCompressed) to retrieve a compressed binary blob suitable for networking.  Note that variable compression is used, so the size of the buffer will fluctuate as the hand pose changes.  Once you have transmitted the compressed buffer to a remote client, the bone transforms can be retrieved by passing the buffer to [vr::VRInput()->DecompressSkeletalBoneData()](#DecompressSkeletalBoneData).  Note that the remote client may not have the same type of controller as the client that sent the buffer, so the compression and decompression is designed to be used independent of any particular driver.  

### Placing the Skeleton in the World
The root bone transform provided by the API will always be in relation to an offset on the input device, and not a position in the VR space.  To get the location in VR space to place the skeleton so that it matches the location of the user's hand, retrieve it from the API the same way that you retrieve the locations of the controllers themselves using `vr::VRInput()->GetPoseActionData()`.  Use the same `vr::VRActionHandle_t` as you used with calls to the other skeletal input functions, and be sure to pass `vr::k_ulInvalidInputValueHandle` as the ulRestrictToDevice parameter, since skeletal actions don't support restricting to a particular device.  

# Skeleton Definition
Detailed information on the skeleton used by the Skeletal Input system can be found here: [Hand Skeleton](https://github.com/ValveSoftware/openvr/wiki/Hand-Skeleton)

# API Documentation

## Enums
### EVRSkeletalTransformSpace
Options for the coordinate space that bone transforms should be provided in
* `VRSkeletalTransformSpace_Model` - The bone transforms have been fully concatenated with their parent bones, and are all in same the coordinate space as the model which they would animate
* `VRSkeletalTransformSpace_Parent` - The bone transforms are in the local coordinate space of their parent bone


### EVRSkeletalMotionRange
Drivers for controllers that support skeletal input are required to provide two streams of animation data: the pose of the user's hand as accurately as it can track it, and the pose of the user's hand as if they were not holding the controller.  
* `VRSkeletalMotionRange_WithController` - The range of motion of the skeleton takes into account any physical limits imposed by the controller itself.  This will tend to be the most accurate pose compared to the user's actual hand pose, but might not allow a closed fist for example
* `VRSkeletalMotionRange_WithoutController` - Retarget the range of motion provided by the input device to make the hand appear to move as if it was not holding a controller.  eg: map "hand grasping controller" to "closed fist"


### EVRSkeletalReferencePose
Enumerates several static poses that the system can provide.  Applications can use these poses as reference for skinning, range of motion, and for blending with other animations.  Used with [GetSkeletalReferenceTransforms()](#GetSkeletalReferenceTransforms)
* `VRSkeletalReferencePose_BindPose` - The default pose for the skeleton, used for skinning the hand mesh
* `VRSkeletalReferencePose_OpenHand` - All fingers straight and spread apart
* `VRSkeletalReferencePose_Fist` - All fingers curled together tightly, thumb wrapped around the fingers
* `VRSkeletalReferencePose_GripLimit` - The pose of the hand when gripping the current controller


### EVRFinger
IDs for the fingers of the hand.  See [GetSkeletalSummaryData()](#GetSkeletalSummaryData)
* `VRFinger_Thumb`
* `VRFinger_Index`
* `VRFinger_Middle`
* `VRFinger_Ring`
* `VRFinger_Pinky`


### EVRFingerSplay
IDs for the pairs of adjacent fingers, used to indicate which pairs of fingers are spread apart or close together.  See [GetSkeletalSummaryData()](#GetSkeletalSummaryData)
* `VRFingerSplay_Thumb_Index`
* `VRFingerSplay_Index_Middle`
* `VRFingerSplay_Middle_Ring`
* `VRFingerSplay_Ring_Pinky`


### EVRSkeletalTrackingLevel
Definition of a standard set of tracking levels that can be used to categorize the capabilities of current and future input devices. Developers of drivers for input hardware can specify the tracking level that their hardware supports so that app developers have a better idea what level of fidelity to expect from the skeletal animation data. Developers can then optionally enable and disable certain features based on the capabilities of the hardware in use.  
See also: [GetSkeletalTrackingLevel()](#GetSkeletalTrackingLevel)
* `VRSkeletalTracking_Estimated` - body part location can’t be directly determined by the device. Any skeletal pose provided by the device is estimated by assuming the position required to active buttons, triggers, joysticks, or other input sensors. E.g. Vive Controller, Gamepad
* `VRSkeletalTracking_Partial` - body part location can be measured directly but with fewer degrees of freedom than the actual body part. Certain body part positions may be unmeasured by the device and estimated from other input data. E.g. Knuckles, gloves that only measure finger curl
* `VRSkeletalTracking_Full` - body part location can be measured directly throughout the entire range of motion of the body part. E.g. Mocap suit for the full body, gloves that measure rotation of each finger segment

## Structs

### InputSkeletalActionData_t
* `bool bActive` - Whether or not this action is bound to an input source that is present in the system and is in an action set that is active from the last UpdateActionState call.
* `VRInputValueHandle_t activeOrigin` - The input source that this action state was generated by.  If this action is bound to multiple inputs, this will be the input that changed most recently.


### VRSkeletalSummaryData_t
Contains summary information about the current skeletal pose
* `float flFingerCurl[ VRFinger_Count ]` - The amount that each finger is 'curled' inwards towards the palm.  In the case of the thumb, this represents how much the thumb is wrapped around the fist.  0 means straight, 1 means fully curled
* `float flFingerSplay[ VRFingerSplay_Count ]` - The amount that each pair of adjacent fingers are separated.  0 means the digits are touching, 1 means they are fully separated.


## Functions


### GetBoneCount
```
EVRInputError GetBoneCount( VRActionHandle_t action, uint32_t* pBoneCount )
```
Reads the number of bones in skeleton associated with the given action
* `action` - The handle of the skeletal action.  
* `pBoneCount` - Pointer to the unsigned integer to write the number of bones into


### GetBoneHierarchy
```
EVRInputError GetBoneHierarchy( VRActionHandle_t action, BoneIndex_t* pParentIndices, uint32_t unIndexArayCount )
```
Fills the given array with the index of each bone's parent in the skeleton associated with the given action
* `action` - The handle of the skeletal action
* `pParentIndices` - Pointer to an array of integers that the function should write the parent indices of each bone into.  This array should be at least as big as the number of bones in the skeleton, which can be retrieved by calling GetBoneCount()
* `unIndexArayCount` - The number of elements in `pParentIndices`


### GetBoneName
```
EVRInputError GetBoneName( VRActionHandle_t action, BoneIndex_t nBoneIndex, char* pchBoneName, uint32_t unBufferSize )
```
Fills the given buffer with the name of the bone at the given index in the skeleton associated with the given action.  Bone name lengths can be no longer than `k_unMaxBoneNameLength`
* `action` - The handle of the skeletal action
* `nBoneIndex` - The index of the bone to retrieve the name of
* `pchBoneName` - Pointer to a buffer to write the name string into.  The function will fail if the buffer is not big enough to fit the length of the name.  Bone names cannot be longer than `k_unMaxBoneNameLength`
* `unBufferSize` - The number of elements in the `pchBoneName` buffer

  
### GetSkeletalReferenceTransforms
```
EVRInputError GetSkeletalReferenceTransforms( VRActionHandle_t action, EVRSkeletalTransformSpace eTransformSpace, EVRSkeletalReferencePose eReferencePose, VRBoneTransform_t *pTransformArray, uint32_t unTransformArrayCount )
```
Fills the given buffer with the transforms for a specific static skeletal reference pose
* `action` - The handle of the skeletal action
* `eTransformSpace` - The coordinate space that the bone transforms should be returned in.  See [EVRSkeletalTransformSpace](#EVRSkeletalTransformSpace)
* `eReferencePose` - The pose to retrieve.  See [EVRSkeletalReferencePose](#EVRSkeletalReferencePose)
* `pTransformArray` - Buffer that the transforms of the pose should be written to.  Should have the same number of elements as bones the skeleton
* `unTransformArrayCount` - The number of elements in the `pTransformArray` array

  
### GetSkeletalTrackingLevel
```
EVRInputError GetSkeletalTrackingLevel( VRActionHandle_t action, EVRSkeletalTrackingLevel* pSkeletalTrackingLevel )
```
Reads the level of accuracy to which the controller is able to track the user to recreate a skeletal pose
* `action` - The handle of the skeletal action
* `pSkeletalTrackingLevel ` - Pointer to the variable that the tracking level should be written to.  See [EVRSkeletalTrackingLevel](#EVRSkeletalTrackingLevel)

  
### GetSkeletalActionData
```
EVRInputError GetSkeletalActionData( VRActionHandle_t action, InputSkeletalActionData_t *pActionData, uint32_t unActionDataSize )
```
Reads the state of the skeletal action and puts the results in the given action data struct.  
* `action` - The handle of the skeletal action to check the status of
* `pActionData` - Pointer to the `InputSkeletalActionData_t` struct that the function should fill in
* `unActionDataSize` - The size in bytes of the `InputSkeletalActionData_t` struct


### GetSkeletalBoneData
```
EVRInputError GetSkeletalBoneData( VRActionHandle_t action, EVRSkeletalTransformSpace eTransformSpace, EVRSkeletalMotionRange eMotionRange, VRBoneTransform_t *pTransformArray, uint32_t unTransformArrayCount )
```
Retrieve the bone transform data.  The bones will be in SteamVR's units and coordinate system; see [Hand Skeleton](https://github.com/ValveSoftware/openvr/wiki/Hand-Skeleton) for more info. 
* `action` - The handle of the skeletal action to retrieve the bone transforms for
* `eTransformSpace` - The coordinate space that each bone transform should be returned in.  See [EVRSkeletalTransformSpace](#EVRSkeletalTransformSpace)
* `eMotionRange` - The desired range of motion of the animation.  See [EVRSkeletalMotionRange](#EVRSkeletalMotionRange)
* `pTransformArray` - Pointer to the array of `vr::VRBoneTransform_t` that the function should put the bone transforms in
* `unTransformArrayCount` - The number of elements in `pTransformArray`


### GetSkeletalSummaryData
```
EVRInputError GetSkeletalSummaryData( VRActionHandle_t action, VRSkeletalSummaryData_t * pSkeletalSummaryData )
```
Reads summary information about the current pose of the skeleton associated with the given action
* `action` - The handle of the skeletal action to retrieve the summary data for
* `pSkeletalSummaryData ` - Pointer to the [VRSkeletalSummaryData_t](#VRSkeletalSummaryData_t) struct to write the summary data to


### GetSkeletalBoneDataCompressed
```
EVRInputError GetSkeletalBoneDataCompressed( VRActionHandle_t action, EVRSkeletalMotionRange eMotionRange, void *pvCompressedData, uint32_t unCompressedSize, uint32_t *punRequiredCompressedSize )
```
Reads the state of the skeletal bone data in a compressed form that is suitable for sending over the network. The required buffer size will never exceed ( sizeof(VR_BoneTransform_t)*boneCount + 2).  Usually the size will be much smaller. 
* `action` - The handle of the skeletal action to retrieve the bone data for
* `eMotionRange` - The desired range of motion of the animation.  See [EVRSkeletalMotionRange](#EVRSkeletalMotionRange)
* `pvCompressedData` - Pointer to a buffer where the compressed data should be placed
* `unCompressedSize` - The size of the buffer pointed to by `pvCompressedData`
* `punRequiredCompressedSize` - The size of the compressed data.  If this is less than unCompressedSize the function will return an error and no transforms will be written


### DecompressSkeletalBoneData
```
EVRInputError DecompressSkeletalBoneData( const void *pvCompressedBuffer, uint32_t unCompressedBufferSize, EVRSkeletalTransformSpace eTransformSpace, VRBoneTransform_t *pTransformArray, uint32_t unTransformArrayCount )
```
Turn a compressed blob back into bone transforms
* `pvCompressedBuffer` - Pointer to a buffer holding the compressed data
* `unCompressedBufferSize` - The size of the buffer pointed to by pvCompressedBuffer
* `eTransformSpace` - Specifies the coordinate space that the transforms should be returned in.  See [EVRSkeletalTransformSpace](#EVRSkeletalTransformSpace)
* `pTransformArray` - Pointer to the array of `vr::VRBoneTransform_t` that the function should put the bone transforms in
* `unTransformArrayCount` - The number of elements in `pTransformArray`.  If there are not enough elements, the function will return an error

