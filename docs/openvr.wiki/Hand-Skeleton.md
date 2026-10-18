# Overview 

The [Skeletal Input](https://github.com/ValveSoftware/openvr/wiki/SteamVR-Skeletal-Input) system allows applications to pick which from a set of pre-defined hand skeletons to receive animation data for from SteamVR.  The definitions of these skeletons are fixed, so that developers can rely on the properties of the skeleton not changing once their app is released.  With that in mind, the choices made about how the skeleton should be configured have been made in an attempt to provide maximum compatibility with existing game engines while also providing extra features that attempt to make it future proof.  

Both left and right hand skeletons have the following properties:
* They have the same number of bones (31)
* There is only one root bone.  It is intended to not translate or rotate, so that it can be constrained to the reference pose of the controller that is animating the skeleton
* The system is designed to avoid any kind of scale; animating scaling can be complicated in real-time engines and is not supported in some cases.  The API only return translations and rotations of bones
* No twist bones
* There are several helper bones, but that are not intended to be directly involved in skinning your hand mesh
* Bones are ordered such that the parent is always before the child


# Files
Reference files for the skeleton are provided with the installation of SteamVR as both GLTF and FBX files.  The skeletons with an example mesh are located here:
```
Steam\steamapps\common\SteamVR\resources\rendermodels\vr_glove\vr_glove_left_model.fbx
Steam\steamapps\common\SteamVR\resources\rendermodels\vr_glove\vr_glove_left_model.glb
Steam\steamapps\common\SteamVR\resources\rendermodels\vr_glove\vr_glove_right_model.fbx
Steam\steamapps\common\SteamVR\resources\rendermodels\vr_glove\vr_glove_right_model.glb
```

Files with just the skeleton (no mesh) are here:
```
Steam\steamapps\common\SteamVR\resources\skeletons\vr_glove_left_skeleton.fbx
Steam\steamapps\common\SteamVR\resources\skeletons\vr_glove_left_skeleton.glb
Steam\steamapps\common\SteamVR\resources\skeletons\vr_glove_right_skeleton.fbx
Steam\steamapps\common\SteamVR\resources\skeletons\vr_glove_right_skeleton.glb
```

# Units and Coordinate System
The skeletal data from the Skeletal Input API is all in SteamVR's units and coordinate system, which is:
* Meters
* Right Handed
* +Y is up
* +X is to the right
* -Z is forward

The bind pose of the hand has the hand pointing forward (-Z) with the palms facing inward (-X for the right hand, +X for the left).

![](https://steamcdn-a.akamaihd.net/steamcommunity/public/images/clans/5519564/05e70a1061cd053b2dbb5be5ce19bfac067f5d84.png) ![](https://steamcdn-a.akamaihd.net/steamcommunity/public/images/clans/5519564/e1ac5db914d9fb3ae884e3d6f72162bdb231a049.png)


# Bone Structure
Both left and right hand skeletons have 31 bones, which are arranged as seen here

![](https://steamcdn-a.akamaihd.net/steamcommunity/public/images/clans/5519564/332a809f2c6defffb2a8a9197d5a648aa17a472b.png)

For convenience, here's an enum defining the bone indices:

```
typedef int32_t BoneIndex_t;
const BoneIndex_t INVALID_BONEINDEX = -1;
enum HandSkeletonBone : BoneIndex_t
{
	eBone_Root = 0,
	eBone_Wrist,
	eBone_Thumb0,
	eBone_Thumb1,
	eBone_Thumb2,
	eBone_Thumb3,
	eBone_IndexFinger0,
	eBone_IndexFinger1,
	eBone_IndexFinger2,
	eBone_IndexFinger3,
	eBone_IndexFinger4,
	eBone_MiddleFinger0,
	eBone_MiddleFinger1,
	eBone_MiddleFinger2,
	eBone_MiddleFinger3,
	eBone_MiddleFinger4,
	eBone_RingFinger0,
	eBone_RingFinger1,
	eBone_RingFinger2,
	eBone_RingFinger3,
	eBone_RingFinger4,
	eBone_PinkyFinger0,
	eBone_PinkyFinger1,
	eBone_PinkyFinger2,
	eBone_PinkyFinger3,
	eBone_PinkyFinger4,
	eBone_Aux_Thumb,
	eBone_Aux_IndexFinger,
	eBone_Aux_MiddleFinger,
	eBone_Aux_RingFinger,
	eBone_Aux_PinkyFinger,
	eBone_Count
};
```



The currently supported skeletons are `/skeleton/hand/left` and `/skeleton/hand/right`.  

# Bone Categories

The bones in the skeleton fall in several categories based on their intended usage

## Skinning Bones
![Skinning Bones](https://steamcdn-a.akamaihd.net/steamcommunity/public/images/clans/5519564/4f43dd2e086c01eeb10fa3ab66d0179a8d1374cf.png)

These are the primary bones used for animating the hand.  Any hand mesh should be skinned to these bones, so that their animation is what drives the deformation of the mesh

## Finger Tip Bones
![Finger Tip Bones](https://steamcdn-a.akamaihd.net/steamcommunity/public/images/clans/5519564/3aa4db234eaf611623b436524267818df894e41b.png)

These bones are on the end of each digit.  They are not skinned to the hand mesh and so do not contribute to the motion that the user will see in your app, and should not have any animation on them.  Their purpose is purely convenience.  Its common in VR apps to want to know where the tip of the user's fingers are, for interaction with objects in the world, gestures, pointing, etc.  But without these bones the last animated bone in each hand is the knuckle, which does not line up with where the visual mesh of the finger ends.  Having finger tip bones is an easy way for gameplay code to know where the fingers end.  

## Auxiliary Bones
![Helper Bones](https://steamcdn-a.akamaihd.net/steamcommunity/public/images/clans/5519564/f10ef747fef0b0139abca23533507d95918cb7c6.png)

The skeleton has 5 auxiliary bones ('aux bones' for short) for helping in the construction of hand poses.  These bones have the same position and rotation and rotation as the last knuckle bone in each finger, but are direct children of the root bone.  This gives them several benefits: predictability in blending, and as a convenient IK target.  

### Aux Bone Blending

Normally, blending between two or more animations is done in the parent space of each bone.  A side effect the skeleton's construction as a forward kinematic chain is that the path that the end of a chain takes as it is blended from one pose to another can be an ark that is hard to predict.  Even when blending between two poses where the finger tips are in the same location in world space, there is no guarantee that the finger tips will still be in that location when the poses are blended 50/50.  

![Bad Blend](https://steamcdn-a.akamaihd.net/steamcommunity/public/images/clans/5519564/8bb9d7b3709b1e90334ee23caddf0a265d3d91a9.gif)

But since the aux bones are children of the root, and the root does not move from the origin of the blend space, blending them results in predictable, linear motion between the being and end positions.  A 50/50 blend between two poses that have the aux bones in the same location will result in a pose with them at that same location.  This make them ideal when you want to keep track of where the ends of the fingers should be through multiple complicated blends, because then you can use them as IK targets to fix up the location of the finger tips after the blends.  

![Blend with aux bones and IK](https://steamcdn-a.akamaihd.net/steamcommunity/public/images/clans/5519564/61dbb63bcf87b07a8f29eddf06f333bc62a85e1c.gif)

### Aux Bones as IK Targets
So if the aux bones are ideal as IK target, why not put them at the finger tips?  Simple: because then we can use two-bone IK to solve each finger instead of 3-bone IK.  

Two-bone IK is a feature that already exists in many game engines, and is relatively easy to implement in those that don't.  It is straightforward to define the limits on the degrees of freedom, has a single solution in the case of hinge joints like fingers.  The predictability in the results from a two-bone IK solve make it easier to tune to get the visual results you want.

Three-bone IK however is not as widely supported, nor trivial to implement.  Three-bone IK solves can have many valid results, and what you get can depend on which algorithm you choose and what your starting pose is.  This complexity and unpredictability make them difficult to work with.  

So, the aux bones track the position and rotation of the last knuckle of each hand rather than the finger tips, so that two-bone IK can be used to solve for the rotations of the first and second knuckle, and the world-space rotation of the third knuckle can simply be copied from the aux bone itself.  