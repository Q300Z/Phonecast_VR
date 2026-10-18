`void GetProjectionRaw( Hmd_Eye eEye, float *pfLeft, float *pfRight, float *pfTop, float *pfBottom )`

Returns the raw project values to use for the specified eye. Most games should use GetProjectionMatrix instead of this method, but sometimes a game needs to do something fancy with its projection and can use these values to compute its own matrix.

The values represent the tangents of the half-angles from the center view axis.  These values can be turned into a projection matrix with the function below.

    void ComposeProjection(float fLeft, float fRight, float fTop, float fBottom, float zNear, float zFar,  HmdMatrix44_t *pmProj )
    {
        float idx = 1.0f / (fRight - fLeft);
        float idy = 1.0f / (fBottom - fTop);
        float idz = 1.0f / (zFar - zNear);
        float sx = fRight + fLeft;
        float sy = fBottom + fTop;

        float (*p)[4] = pmProj->m;
        p[0][0] = 2*idx; p[0][1] = 0;     p[0][2] = sx*idx;    p[0][3] = 0;
        p[1][0] = 0;     p[1][1] = 2*idy; p[1][2] = sy*idy;    p[1][3] = 0;
        p[2][0] = 0;     p[2][1] = 0;     p[2][2] = -zFar*idz; p[2][3] = -zFar*zNear*idz;
        p[3][0] = 0;     p[3][1] = 0;     p[3][2] = -1.0f;     p[3][3] = 0;
    }

If you are using OpenGL, you can accomplish the same thing with a call to glFrustum. Note that glFrustum (and similar APIs) often expect the left, right, top and bottom values to be coordinates on the near clipping plane. To fulfill that requirement, you will have to multiply the values returned by GetProjectionRaw by the near plane distance (zNear in the above example).


* `eEye` - Eye_Left or Eye_Right. Determines which eye the function should return the projection for.
* `pfLeft` - tangent of the half-angle from center axis to the left clipping plane
* `pfRight` - tangent of the half-angle from center axis to the right clipping plane
* `pfTop` - tangent of the half-angle from center axis to the top clipping plane
* `pfBottom` - tangent of the half-angle from center axis to the bottom clipping plane

