During level transitions, it's often not possible to continue rendering at full framerate.  When a game stops submitting frames, the SteamVR compositor will automatically trigger a fade to the construct so users have a proper frame of reference.  Applications are encouraged to skin this environment to match their art direction.  In The Lab, we set a skybox and put up several overlays to cover these transitions.  If you are using Unity, there is a "LoadLevel" script to help get you started.
https://github.com/ValveSoftware/steamvr_unity_plugin/blob/master/Assets/SteamVR/Scripts/SteamVR_LoadLevel.cs
Even if you aren't using Unity, this provides a good overview of the steps you should take for customizing these transitions.

Starting with SteamVR version 1.9, you can now also specify a single mesh to render as the "stage".  This mesh is limited to 65k vertices and a single texture which will be rendered unlit.  This mesh can be set by calling SetStageOverride_Async and passing in the full path to an OBJ file.  See the render models that ship with SteamVR for examples of how to specify the material.  E.g. C:\Program Files (x86)\Steam\steamapps\common\SteamVR\resources\rendermodels\generic_hmd
We recommend using PNGs for the texture.

If your model isn't showing up, inspect SteamVR's Web Console's vrcompositor_vrclient log for errors.

Reading the model's geometry and texture from disk and uploading to the gpu may take a second or so depending on size.  When this is completed and the model is ready to render the SteamVR event StageOverrideReady will be sent.  To ensure a seamless transition, you may want to delay your load until receiving this event and continue rendering in the meantime.  Once ready, use FadeToGrid to explicitly control the crossfade duration.  You should be prepared to handle the case of that event never being sent due to some failure to load the model.

Once the transition is completed and you've successfully crossfaded back into your active game, you should call ClearStageOverride to free the assets consumed by the model and its texture.  Failure to do so will also elicit an error when attempting to call SetStageOverride a second time.

Finally, there are several options you may choose from when setting the stage to control its rendering.

* PrimaryColor - This value is multiplied by the model's texture to apply a tint.
* VignetteInnerRadius & VignetteOuterRadius - These values specify a spherical fade to the specified SecondaryColor (in meters).  This is most commonly used with black to give the illusion of being in a pool of light centered on the playspace area.
* FresnelStrength - This can be used to apply a fresnel effect to the stage geometry.  The mesh is treated as faceted and lerps between the primary and secondary color based on triangle orientation to the viewer.
* BackfaceCulling - Controls the rendering of triangles that face away from the camera.
* Greyscale - Converts the render model's texture to luma.
* Wireframe - Renders mesh as a wireframe.

Note: Textures larger than 2048x2048 are automatically DXT compressed on load.  This compression can introduce undesired red/green color artifacts, particularly in greyscale source images.  Setting the Greyscale option to true is a useful way of combating this.