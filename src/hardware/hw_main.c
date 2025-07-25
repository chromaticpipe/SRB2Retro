// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// Copyright (C) 1998-2000 by DooM Legacy Team.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//-----------------------------------------------------------------------------
/// \file
/// \brief hardware renderer, using the standard HardWareRender driver DLL for SRB2

#include <math.h>

#include "../doomstat.h"

#ifdef HWRENDER
#include "hw_glob.h"
#include "hw_light.h"
#include "hw_drv.h"

#include "../i_video.h" // for rendermode == render_glide
#include "../v_video.h"
#include "../p_local.h"
#include "../p_setup.h"
#include "../r_local.h"
#include "../r_bsp.h"
#include "../d_clisrv.h"
#include "../w_wad.h"
#include "../z_zone.h"
#include "../r_splats.h"
#include "../g_game.h"
#include "../st_stuff.h"
#ifdef SHUFFLE
#include "../i_system.h"
#endif

#include "hw_md2.h"
#include "../m_random.h"

#define R_FAKEFLOORS
//#define HWPRECIP
#define SORTING
//#define POLYSKY

// ==========================================================================
// the hardware driver object
// ==========================================================================
struct hwdriver_s hwdriver;

// ==========================================================================
//                                                                     PROTOS
// ==========================================================================


static void HWR_AddSprites(sector_t *sec);
static void HWR_ProjectSprite(mobj_t *thing);
#ifdef HWPRECIP
static void HWR_ProjectPrecipitationSprite(precipmobj_t *thing);
#endif

#ifdef SORTING
void HWR_AddTransparentFloor(lumpnum_t lumpnum, extrasubsector_t *xsub, fixed_t fixedheight,
                             INT32 lightlevel, INT32 alpha, sector_t *FOFSector, FBITFIELD blend, boolean fogplane, extracolormap_t *planecolormap);
#else
static void HWR_Add3DWater(lumpnum_t lumpnum, extrasubsector_t *xsub, fixed_t fixedheight,
                           INT32 lightlevel, INT32 alpha, sector_t *FOFSector);
static void HWR_Render3DWater(void);
static void HWR_RenderTransparentWalls(void);
#endif
static void HWR_FoggingOn(void);
static UINT32 atohex(const char *s);

static void CV_filtermode_ONChange(void);
static void CV_anisotropic_ONChange(void);
static void CV_FogDensity_ONChange(void);
static void CV_grFov_OnChange(void);
// ==========================================================================
//                                          3D ENGINE COMMANDS & CONSOLE VARS
// ==========================================================================

static CV_PossibleValue_t grfov_cons_t[] = {{0, "MIN"}, {179*FRACUNIT, "MAX"}, {0, NULL}};
static CV_PossibleValue_t grfiltermode_cons_t[]= {{HWD_SET_TEXTUREFILTER_POINTSAMPLED, "Nearest"},
	{HWD_SET_TEXTUREFILTER_BILINEAR, "Bilinear"}, {HWD_SET_TEXTUREFILTER_TRILINEAR, "Trilinear"},
	{HWD_SET_TEXTUREFILTER_MIXED1, "Linear_Nearest"},
	{HWD_SET_TEXTUREFILTER_MIXED2, "Nearest_Linear"},
#ifdef SHUFFLE
	{HWD_SET_TEXTUREFILTER_MIXED3, "Nearest_Mipmap"},
#endif
	{0, NULL}};
CV_PossibleValue_t granisotropicmode_cons_t[] = {{1, "MIN"}, {16, "MAX"}, {0, NULL}};

boolean drawsky = true;

// needs fix: walls are incorrectly clipped one column less
static consvar_t cv_grclipwalls = {"gr_clipwalls", "Off", 0, CV_OnOff, NULL, 0, NULL, NULL, 0, 0, NULL};

//development variables for diverse uses
static consvar_t cv_grbeta = {"gr_beta", "0", 0, CV_Unsigned, NULL, 0, NULL, NULL, 0, 0, NULL};

#ifdef SHUFFLE
static float HWRWipeCounter = 1.0f;
#endif
consvar_t cv_grfov = {"gr_fov", "90", CV_FLOAT|CV_CALL, grfov_cons_t, CV_grFov_OnChange, 0, NULL, NULL, 0, 0, NULL};
#ifndef HARDWAREFIX
consvar_t cv_grfogdensity = {"gr_fogdensity", "25", CV_CALL|CV_NOINIT, CV_Unsigned,
                             CV_FogDensity_ONChange, 0, NULL, NULL, 0, 0, NULL};
#else
consvar_t cv_grfogdensity = {"gr_fogdensity", "150", 0, CV_Unsigned,
                             CV_FogDensity_ONChange, 0, NULL, NULL, 0, 0, NULL};
#endif

// Unfortunately, this can no longer be saved...
#ifdef SHUFFLE
consvar_t cv_grfiltermode = {"gr_filtermode", "Nearest", CV_CALL, grfiltermode_cons_t,
                             CV_filtermode_ONChange, 0, NULL, NULL, 0, 0, NULL};
#else
consvar_t cv_grfiltermode = {"gr_filtermode", "Bilinear", CV_CALL, grfiltermode_cons_t,
                             CV_filtermode_ONChange, 0, NULL, NULL, 0, 0, NULL};
#endif
consvar_t cv_granisotropicmode = {"gr_anisotropicmode", "1", CV_CALL, granisotropicmode_cons_t,
                             CV_anisotropic_ONChange, 0, NULL, NULL, 0, 0, NULL};
consvar_t cv_grcorrecttricks = {"gr_correcttricks", "On", 0, CV_OnOff, NULL, 0, NULL, NULL, 0, 0, NULL};
consvar_t cv_grsolvetjoin = {"gr_solvetjoin", "On", 0, CV_OnOff, NULL, 0, NULL, NULL, 0, 0, NULL};

static CV_PossibleValue_t CV_MD2[] = {{0, "Off"}, {1, "On"}, {2, "Old"}, {0, NULL}};
// console variables in development
consvar_t cv_grmd2 = {"gr_md2", "Off", 0, CV_MD2, NULL, 0, NULL, NULL, 0, 0, NULL};

static void CV_FogDensity_ONChange(void)
{
	HWD.pfnSetSpecialState(HWD_SET_FOG_DENSITY, cv_grfogdensity.value);
}

static void CV_filtermode_ONChange(void)
{
	HWD.pfnSetSpecialState(HWD_SET_TEXTUREFILTERMODE, cv_grfiltermode.value);
}

static void CV_anisotropic_ONChange(void)
{
	HWD.pfnSetSpecialState(HWD_SET_TEXTUREANISOTROPICMODE, cv_granisotropicmode.value);
}

/*
 * lookuptable for lightvalues
 * calculated as follow:
 * floatlight = (1.0-exp((light^3)*gamma)) / (1.0-exp(1.0*gamma));
 * gamma=-0,2;-2,0;-4,0;-6,0;-8,0
 * light = 0,0 .. 1,0
 */
static const float lighttable[5][256] =
{
  {
    0.00000f,0.00000f,0.00000f,0.00000f,0.00000f,0.00001f,0.00001f,0.00002f,0.00003f,0.00004f,
    0.00006f,0.00008f,0.00010f,0.00013f,0.00017f,0.00020f,0.00025f,0.00030f,0.00035f,0.00041f,
    0.00048f,0.00056f,0.00064f,0.00073f,0.00083f,0.00094f,0.00106f,0.00119f,0.00132f,0.00147f,
    0.00163f,0.00180f,0.00198f,0.00217f,0.00237f,0.00259f,0.00281f,0.00305f,0.00331f,0.00358f,
    0.00386f,0.00416f,0.00447f,0.00479f,0.00514f,0.00550f,0.00587f,0.00626f,0.00667f,0.00710f,
    0.00754f,0.00800f,0.00848f,0.00898f,0.00950f,0.01003f,0.01059f,0.01117f,0.01177f,0.01239f,
    0.01303f,0.01369f,0.01437f,0.01508f,0.01581f,0.01656f,0.01734f,0.01814f,0.01896f,0.01981f,
    0.02069f,0.02159f,0.02251f,0.02346f,0.02444f,0.02544f,0.02647f,0.02753f,0.02862f,0.02973f,
    0.03088f,0.03205f,0.03325f,0.03448f,0.03575f,0.03704f,0.03836f,0.03971f,0.04110f,0.04252f,
    0.04396f,0.04545f,0.04696f,0.04851f,0.05009f,0.05171f,0.05336f,0.05504f,0.05676f,0.05852f,
    0.06031f,0.06214f,0.06400f,0.06590f,0.06784f,0.06981f,0.07183f,0.07388f,0.07597f,0.07810f,
    0.08027f,0.08248f,0.08473f,0.08702f,0.08935f,0.09172f,0.09414f,0.09659f,0.09909f,0.10163f,
    0.10421f,0.10684f,0.10951f,0.11223f,0.11499f,0.11779f,0.12064f,0.12354f,0.12648f,0.12946f,
    0.13250f,0.13558f,0.13871f,0.14188f,0.14511f,0.14838f,0.15170f,0.15507f,0.15850f,0.16197f,
    0.16549f,0.16906f,0.17268f,0.17635f,0.18008f,0.18386f,0.18769f,0.19157f,0.19551f,0.19950f,
    0.20354f,0.20764f,0.21179f,0.21600f,0.22026f,0.22458f,0.22896f,0.23339f,0.23788f,0.24242f,
    0.24702f,0.25168f,0.25640f,0.26118f,0.26602f,0.27091f,0.27587f,0.28089f,0.28596f,0.29110f,
    0.29630f,0.30156f,0.30688f,0.31226f,0.31771f,0.32322f,0.32879f,0.33443f,0.34013f,0.34589f,
    0.35172f,0.35761f,0.36357f,0.36960f,0.37569f,0.38185f,0.38808f,0.39437f,0.40073f,0.40716f,
    0.41366f,0.42022f,0.42686f,0.43356f,0.44034f,0.44718f,0.45410f,0.46108f,0.46814f,0.47527f,
    0.48247f,0.48974f,0.49709f,0.50451f,0.51200f,0.51957f,0.52721f,0.53492f,0.54271f,0.55058f,
    0.55852f,0.56654f,0.57463f,0.58280f,0.59105f,0.59937f,0.60777f,0.61625f,0.62481f,0.63345f,
    0.64217f,0.65096f,0.65984f,0.66880f,0.67783f,0.68695f,0.69615f,0.70544f,0.71480f,0.72425f,
    0.73378f,0.74339f,0.75308f,0.76286f,0.77273f,0.78268f,0.79271f,0.80283f,0.81304f,0.82333f,
    0.83371f,0.84417f,0.85472f,0.86536f,0.87609f,0.88691f,0.89781f,0.90880f,0.91989f,0.93106f,
    0.94232f,0.95368f,0.96512f,0.97665f,0.98828f,1.00000f
  },
  {
    0.00000f,0.00000f,0.00000f,0.00000f,0.00001f,0.00002f,0.00003f,0.00005f,0.00007f,0.00010f,
    0.00014f,0.00019f,0.00024f,0.00031f,0.00038f,0.00047f,0.00057f,0.00069f,0.00081f,0.00096f,
    0.00112f,0.00129f,0.00148f,0.00170f,0.00193f,0.00218f,0.00245f,0.00274f,0.00306f,0.00340f,
    0.00376f,0.00415f,0.00456f,0.00500f,0.00547f,0.00597f,0.00649f,0.00704f,0.00763f,0.00825f,
    0.00889f,0.00957f,0.01029f,0.01104f,0.01182f,0.01264f,0.01350f,0.01439f,0.01532f,0.01630f,
    0.01731f,0.01836f,0.01945f,0.02058f,0.02176f,0.02298f,0.02424f,0.02555f,0.02690f,0.02830f,
    0.02974f,0.03123f,0.03277f,0.03436f,0.03600f,0.03768f,0.03942f,0.04120f,0.04304f,0.04493f,
    0.04687f,0.04886f,0.05091f,0.05301f,0.05517f,0.05738f,0.05964f,0.06196f,0.06434f,0.06677f,
    0.06926f,0.07181f,0.07441f,0.07707f,0.07979f,0.08257f,0.08541f,0.08831f,0.09126f,0.09428f,
    0.09735f,0.10048f,0.10368f,0.10693f,0.11025f,0.11362f,0.11706f,0.12056f,0.12411f,0.12773f,
    0.13141f,0.13515f,0.13895f,0.14281f,0.14673f,0.15072f,0.15476f,0.15886f,0.16303f,0.16725f,
    0.17153f,0.17587f,0.18028f,0.18474f,0.18926f,0.19383f,0.19847f,0.20316f,0.20791f,0.21272f,
    0.21759f,0.22251f,0.22748f,0.23251f,0.23760f,0.24274f,0.24793f,0.25318f,0.25848f,0.26383f,
    0.26923f,0.27468f,0.28018f,0.28573f,0.29133f,0.29697f,0.30266f,0.30840f,0.31418f,0.32001f,
    0.32588f,0.33179f,0.33774f,0.34374f,0.34977f,0.35585f,0.36196f,0.36810f,0.37428f,0.38050f,
    0.38675f,0.39304f,0.39935f,0.40570f,0.41207f,0.41847f,0.42490f,0.43136f,0.43784f,0.44434f,
    0.45087f,0.45741f,0.46398f,0.47057f,0.47717f,0.48379f,0.49042f,0.49707f,0.50373f,0.51041f,
    0.51709f,0.52378f,0.53048f,0.53718f,0.54389f,0.55061f,0.55732f,0.56404f,0.57075f,0.57747f,
    0.58418f,0.59089f,0.59759f,0.60429f,0.61097f,0.61765f,0.62432f,0.63098f,0.63762f,0.64425f,
    0.65086f,0.65746f,0.66404f,0.67060f,0.67714f,0.68365f,0.69015f,0.69662f,0.70307f,0.70948f,
    0.71588f,0.72224f,0.72857f,0.73488f,0.74115f,0.74739f,0.75359f,0.75976f,0.76589f,0.77199f,
    0.77805f,0.78407f,0.79005f,0.79599f,0.80189f,0.80774f,0.81355f,0.81932f,0.82504f,0.83072f,
    0.83635f,0.84194f,0.84747f,0.85296f,0.85840f,0.86378f,0.86912f,0.87441f,0.87964f,0.88482f,
    0.88995f,0.89503f,0.90005f,0.90502f,0.90993f,0.91479f,0.91959f,0.92434f,0.92903f,0.93366f,
    0.93824f,0.94276f,0.94723f,0.95163f,0.95598f,0.96027f,0.96451f,0.96868f,0.97280f,0.97686f,
    0.98086f,0.98481f,0.98869f,0.99252f,0.99629f,1.00000f
  },
  {
    0.00000f,0.00000f,0.00000f,0.00001f,0.00002f,0.00003f,0.00005f,0.00008f,0.00013f,0.00018f,
    0.00025f,0.00033f,0.00042f,0.00054f,0.00067f,0.00083f,0.00101f,0.00121f,0.00143f,0.00168f,
    0.00196f,0.00227f,0.00261f,0.00299f,0.00339f,0.00383f,0.00431f,0.00483f,0.00538f,0.00598f,
    0.00661f,0.00729f,0.00802f,0.00879f,0.00961f,0.01048f,0.01140f,0.01237f,0.01340f,0.01447f,
    0.01561f,0.01680f,0.01804f,0.01935f,0.02072f,0.02215f,0.02364f,0.02520f,0.02682f,0.02850f,
    0.03026f,0.03208f,0.03397f,0.03594f,0.03797f,0.04007f,0.04225f,0.04451f,0.04684f,0.04924f,
    0.05172f,0.05428f,0.05691f,0.05963f,0.06242f,0.06530f,0.06825f,0.07129f,0.07441f,0.07761f,
    0.08089f,0.08426f,0.08771f,0.09125f,0.09487f,0.09857f,0.10236f,0.10623f,0.11019f,0.11423f,
    0.11836f,0.12257f,0.12687f,0.13125f,0.13571f,0.14027f,0.14490f,0.14962f,0.15442f,0.15931f,
    0.16427f,0.16932f,0.17445f,0.17966f,0.18496f,0.19033f,0.19578f,0.20130f,0.20691f,0.21259f,
    0.21834f,0.22417f,0.23007f,0.23605f,0.24209f,0.24820f,0.25438f,0.26063f,0.26694f,0.27332f,
    0.27976f,0.28626f,0.29282f,0.29944f,0.30611f,0.31284f,0.31962f,0.32646f,0.33334f,0.34027f,
    0.34724f,0.35426f,0.36132f,0.36842f,0.37556f,0.38273f,0.38994f,0.39718f,0.40445f,0.41174f,
    0.41907f,0.42641f,0.43378f,0.44116f,0.44856f,0.45598f,0.46340f,0.47084f,0.47828f,0.48573f,
    0.49319f,0.50064f,0.50809f,0.51554f,0.52298f,0.53042f,0.53784f,0.54525f,0.55265f,0.56002f,
    0.56738f,0.57472f,0.58203f,0.58932f,0.59658f,0.60381f,0.61101f,0.61817f,0.62529f,0.63238f,
    0.63943f,0.64643f,0.65339f,0.66031f,0.66717f,0.67399f,0.68075f,0.68746f,0.69412f,0.70072f,
    0.70726f,0.71375f,0.72017f,0.72653f,0.73282f,0.73905f,0.74522f,0.75131f,0.75734f,0.76330f,
    0.76918f,0.77500f,0.78074f,0.78640f,0.79199f,0.79751f,0.80295f,0.80831f,0.81359f,0.81880f,
    0.82393f,0.82898f,0.83394f,0.83883f,0.84364f,0.84836f,0.85301f,0.85758f,0.86206f,0.86646f,
    0.87078f,0.87502f,0.87918f,0.88326f,0.88726f,0.89118f,0.89501f,0.89877f,0.90245f,0.90605f,
    0.90957f,0.91301f,0.91638f,0.91966f,0.92288f,0.92601f,0.92908f,0.93206f,0.93498f,0.93782f,
    0.94059f,0.94329f,0.94592f,0.94848f,0.95097f,0.95339f,0.95575f,0.95804f,0.96027f,0.96244f,
    0.96454f,0.96658f,0.96856f,0.97049f,0.97235f,0.97416f,0.97591f,0.97760f,0.97924f,0.98083f,
    0.98237f,0.98386f,0.98530f,0.98669f,0.98803f,0.98933f,0.99058f,0.99179f,0.99295f,0.99408f,
    0.99516f,0.99620f,0.99721f,0.99817f,0.99910f,1.00000f
  },
  {
    0.00000f,0.00000f,0.00000f,0.00001f,0.00002f,0.00005f,0.00008f,0.00012f,0.00019f,0.00026f,
    0.00036f,0.00048f,0.00063f,0.00080f,0.00099f,0.00122f,0.00148f,0.00178f,0.00211f,0.00249f,
    0.00290f,0.00335f,0.00386f,0.00440f,0.00500f,0.00565f,0.00636f,0.00711f,0.00793f,0.00881f,
    0.00975f,0.01075f,0.01182f,0.01295f,0.01416f,0.01543f,0.01678f,0.01821f,0.01971f,0.02129f,
    0.02295f,0.02469f,0.02652f,0.02843f,0.03043f,0.03252f,0.03469f,0.03696f,0.03933f,0.04178f,
    0.04433f,0.04698f,0.04973f,0.05258f,0.05552f,0.05857f,0.06172f,0.06498f,0.06834f,0.07180f,
    0.07537f,0.07905f,0.08283f,0.08672f,0.09072f,0.09483f,0.09905f,0.10337f,0.10781f,0.11236f,
    0.11701f,0.12178f,0.12665f,0.13163f,0.13673f,0.14193f,0.14724f,0.15265f,0.15817f,0.16380f,
    0.16954f,0.17538f,0.18132f,0.18737f,0.19351f,0.19976f,0.20610f,0.21255f,0.21908f,0.22572f,
    0.23244f,0.23926f,0.24616f,0.25316f,0.26023f,0.26739f,0.27464f,0.28196f,0.28935f,0.29683f,
    0.30437f,0.31198f,0.31966f,0.32740f,0.33521f,0.34307f,0.35099f,0.35896f,0.36699f,0.37506f,
    0.38317f,0.39133f,0.39952f,0.40775f,0.41601f,0.42429f,0.43261f,0.44094f,0.44929f,0.45766f,
    0.46604f,0.47443f,0.48283f,0.49122f,0.49962f,0.50801f,0.51639f,0.52476f,0.53312f,0.54146f,
    0.54978f,0.55807f,0.56633f,0.57457f,0.58277f,0.59093f,0.59905f,0.60713f,0.61516f,0.62314f,
    0.63107f,0.63895f,0.64676f,0.65452f,0.66221f,0.66984f,0.67739f,0.68488f,0.69229f,0.69963f,
    0.70689f,0.71407f,0.72117f,0.72818f,0.73511f,0.74195f,0.74870f,0.75536f,0.76192f,0.76839f,
    0.77477f,0.78105f,0.78723f,0.79331f,0.79930f,0.80518f,0.81096f,0.81664f,0.82221f,0.82768f,
    0.83305f,0.83832f,0.84347f,0.84853f,0.85348f,0.85832f,0.86306f,0.86770f,0.87223f,0.87666f,
    0.88098f,0.88521f,0.88933f,0.89334f,0.89726f,0.90108f,0.90480f,0.90842f,0.91194f,0.91537f,
    0.91870f,0.92193f,0.92508f,0.92813f,0.93109f,0.93396f,0.93675f,0.93945f,0.94206f,0.94459f,
    0.94704f,0.94941f,0.95169f,0.95391f,0.95604f,0.95810f,0.96009f,0.96201f,0.96386f,0.96564f,
    0.96735f,0.96900f,0.97059f,0.97212f,0.97358f,0.97499f,0.97634f,0.97764f,0.97888f,0.98007f,
    0.98122f,0.98231f,0.98336f,0.98436f,0.98531f,0.98623f,0.98710f,0.98793f,0.98873f,0.98949f,
    0.99021f,0.99090f,0.99155f,0.99218f,0.99277f,0.99333f,0.99387f,0.99437f,0.99486f,0.99531f,
    0.99575f,0.99616f,0.99654f,0.99691f,0.99726f,0.99759f,0.99790f,0.99819f,0.99847f,0.99873f,
    0.99897f,0.99920f,0.99942f,0.99963f,0.99982f,1.00000f
  },
  {
    0.00000f,0.00000f,0.00000f,0.00001f,0.00003f,0.00006f,0.00010f,0.00017f,0.00025f,0.00035f,
    0.00048f,0.00064f,0.00083f,0.00106f,0.00132f,0.00163f,0.00197f,0.00237f,0.00281f,0.00330f,
    0.00385f,0.00446f,0.00513f,0.00585f,0.00665f,0.00751f,0.00845f,0.00945f,0.01054f,0.01170f,
    0.01295f,0.01428f,0.01569f,0.01719f,0.01879f,0.02048f,0.02227f,0.02415f,0.02614f,0.02822f,
    0.03042f,0.03272f,0.03513f,0.03765f,0.04028f,0.04303f,0.04589f,0.04887f,0.05198f,0.05520f,
    0.05855f,0.06202f,0.06561f,0.06933f,0.07318f,0.07716f,0.08127f,0.08550f,0.08987f,0.09437f,
    0.09900f,0.10376f,0.10866f,0.11369f,0.11884f,0.12414f,0.12956f,0.13512f,0.14080f,0.14662f,
    0.15257f,0.15865f,0.16485f,0.17118f,0.17764f,0.18423f,0.19093f,0.19776f,0.20471f,0.21177f,
    0.21895f,0.22625f,0.23365f,0.24117f,0.24879f,0.25652f,0.26435f,0.27228f,0.28030f,0.28842f,
    0.29662f,0.30492f,0.31329f,0.32175f,0.33028f,0.33889f,0.34756f,0.35630f,0.36510f,0.37396f,
    0.38287f,0.39183f,0.40084f,0.40989f,0.41897f,0.42809f,0.43723f,0.44640f,0.45559f,0.46479f,
    0.47401f,0.48323f,0.49245f,0.50167f,0.51088f,0.52008f,0.52927f,0.53843f,0.54757f,0.55668f,
    0.56575f,0.57479f,0.58379f,0.59274f,0.60164f,0.61048f,0.61927f,0.62799f,0.63665f,0.64524f,
    0.65376f,0.66220f,0.67056f,0.67883f,0.68702f,0.69511f,0.70312f,0.71103f,0.71884f,0.72655f,
    0.73415f,0.74165f,0.74904f,0.75632f,0.76348f,0.77053f,0.77747f,0.78428f,0.79098f,0.79756f,
    0.80401f,0.81035f,0.81655f,0.82264f,0.82859f,0.83443f,0.84013f,0.84571f,0.85117f,0.85649f,
    0.86169f,0.86677f,0.87172f,0.87654f,0.88124f,0.88581f,0.89026f,0.89459f,0.89880f,0.90289f,
    0.90686f,0.91071f,0.91445f,0.91807f,0.92157f,0.92497f,0.92826f,0.93143f,0.93450f,0.93747f,
    0.94034f,0.94310f,0.94577f,0.94833f,0.95081f,0.95319f,0.95548f,0.95768f,0.95980f,0.96183f,
    0.96378f,0.96565f,0.96744f,0.96916f,0.97081f,0.97238f,0.97388f,0.97532f,0.97669f,0.97801f,
    0.97926f,0.98045f,0.98158f,0.98266f,0.98369f,0.98467f,0.98560f,0.98648f,0.98732f,0.98811f,
    0.98886f,0.98958f,0.99025f,0.99089f,0.99149f,0.99206f,0.99260f,0.99311f,0.99359f,0.99404f,
    0.99446f,0.99486f,0.99523f,0.99559f,0.99592f,0.99623f,0.99652f,0.99679f,0.99705f,0.99729f,
    0.99751f,0.99772f,0.99792f,0.99810f,0.99827f,0.99843f,0.99857f,0.99871f,0.99884f,0.99896f,
    0.99907f,0.99917f,0.99926f,0.99935f,0.99943f,0.99951f,0.99958f,0.99964f,0.99970f,0.99975f,
    0.99980f,0.99985f,0.99989f,0.99993f,0.99997f,1.00000f
  }
};

#define gld_CalcLightLevel(lightlevel) (lighttable[1][max(min((lightlevel),255),0)])

// ==========================================================================
//                                                               VIEW GLOBALS
// ==========================================================================
// Fineangles in the SCREENWIDTH wide window.
#define FIELDOFVIEW ANGLE_90
#define ABS(x) ((x) < 0 ? -(x) : (x))

static angle_t gr_clipangle;

// The viewangletox[viewangle + FINEANGLES/4] lookup
// maps the visible view angles to screen X coordinates,
// flattening the arc to a flat projection plane.
// There will be many angles mapped to the same X.
static INT32 gr_viewangletox[FINEANGLES/2];

// The xtoviewangleangle[] table maps a screen pixel
// to the lowest viewangle that maps back to x ranges
// from clipangle to -clipangle.
static angle_t gr_xtoviewangle[MAXVIDWIDTH+1];

// ==========================================================================
//                                                                    GLOBALS
// ==========================================================================

// uncomment to remove the plane rendering
#define DOPLANES
//#define DOWALLS

// test of drawing sky by polygons like in software with visplane, unfortunately
// this doesn't work since we must have z for pixel and z for texture (not like now with z = oow)
//#define POLYSKY

/// \note crappy
#define drawtextured true

#define FLOATSCALE(x,y) ((y)*(x)/100.0f)


// base values set at SetViewSize
static float gr_basecentery;

float gr_baseviewwindowy, gr_basewindowcentery;
float gr_viewwidth, gr_viewheight; // viewport clipping boundaries (screen coords)
float gr_viewwindowx;

static float gr_centerx, gr_centery;
static float gr_viewwindowy; // top left corner of view window
static float gr_windowcenterx; // center of view window, for projection
static float gr_windowcentery;

static float gr_pspritexscale, gr_pspriteyscale;

static seg_t *gr_curline;
static side_t *gr_sidedef;
static line_t *gr_linedef;
static sector_t *gr_frontsector;
static sector_t *gr_backsector;

// --------------------------------------------------------------------------
//                                              STUFF FOR THE PROJECTION CODE
// --------------------------------------------------------------------------

FTransform atransform;
// duplicates of the main code, set after R_SetupFrame() passed them into sharedstruct,
// copied here for local use
static fixed_t dup_viewx, dup_viewy, dup_viewz;
static angle_t dup_viewangle;

static float gr_viewx, gr_viewy, gr_viewz;
static float gr_viewsin, gr_viewcos;

// Maybe not necessary with the new T&L code (needs to be checked!)
static float gr_viewludsin, gr_viewludcos; // look up down kik test
static float gr_fovlud;

// ==========================================================================
//                                    LIGHT stuffs
// ==========================================================================

static UINT8 lightleveltonumlut[256];

// added to SRB2's sector lightlevel to make things a bit brighter (sprites/walls/planes)
FUNCMATH UINT8 LightLevelToLum(INT32 l)
{
/*#ifdef HARDWAREFIX
	if (!cv_grfog.value)
	{
		if(l > 255)
			l = 255;
		else if(l < 0)
			l = 0;
		return lightleveltonumlut[l];
	}
#endif*/
	return (UINT8)(255*gld_CalcLightLevel(l));
/*
	l = lightleveltonumlut[l];

	if (l > 255)
		l = 255;
	return (UINT8)l;*/
}

static inline void InitLumLut(void)
{
	INT32 i, k = 0;
	for (i = 0; i < 256; i++)
	{
		if (i > 128)
			k += 2;
		else
			k = 1;
		lightleveltonumlut[i] = (UINT8)(k);
	}
}

#ifdef HARDWAREFIX
//#define FOGFACTOR 300 //was 600 >> Covered by cv_grfogdensity
#define NORMALFOG 0x00000000
#define FADEFOG 0x19000000
#define CALCFOGDENSITY(x) ((float)((5220.0f*(1.0f/((x)/41.0f+1.0f)))-(5220.0f*(1.0f/(255.0f/41.0f+1.0f))))) // Approximate fog calculation based off of software walls
#define CALCFOGDENSITYFLOOR(x) ((float)((40227.0f*(1.0f/((x)/11.0f+1.0f)))-(40227.0f*(1.0f/(255.0f/11.0f+1.0f))))) // Approximate fog calculation based off of software floors
#define CALCLIGHT(x,y) ((float)(x)*((y)/255.0f))
UINT32 HWR_Lighting(INT32 light, UINT32 color, UINT32 fadecolor, boolean fogblockpoly, boolean plane)
{
	RGBA_t realcolor, fogcolor, surfcolor;
	INT32 alpha, fogalpha;

	realcolor.rgba = color;
	fogcolor.rgba = fadecolor;

	alpha = (realcolor.s.alpha*255)/25;
	fogalpha = (fogcolor.s.alpha*255)/25;

	if (cv_grfog.value && cv_grsoftwarefog.value) // Only do this when fog is on, software fog mode is on, and the poly is not from a fog block
	{
		// Modulate the colors by alpha.
		realcolor.s.red = (UINT8)(CALCLIGHT(alpha,realcolor.s.red));
		realcolor.s.green = (UINT8)(CALCLIGHT(alpha,realcolor.s.green));
		realcolor.s.blue = (UINT8)(CALCLIGHT(alpha,realcolor.s.blue));

		// Set the surface colors and further modulate the colors by light.
		surfcolor.s.red = (UINT8)(CALCLIGHT((0xFF-alpha),255)+CALCLIGHT(realcolor.s.red,255));
		surfcolor.s.green = (UINT8)(CALCLIGHT((0xFF-alpha),255)+CALCLIGHT(realcolor.s.green,255));
		surfcolor.s.blue = (UINT8)(CALCLIGHT((0xFF-alpha),255)+CALCLIGHT(realcolor.s.blue,255));
		surfcolor.s.alpha = 0xFF;

		// Modulate the colors by alpha.
		fogcolor.s.red = (UINT8)(CALCLIGHT(fogalpha,fogcolor.s.red));
		fogcolor.s.green = (UINT8)(CALCLIGHT(fogalpha,fogcolor.s.green));
		fogcolor.s.blue = (UINT8)(CALCLIGHT(fogalpha,fogcolor.s.blue));
	}
	else
	{
		// Modulate the colors by alpha.
		realcolor.s.red = (UINT8)(CALCLIGHT(alpha,realcolor.s.red));
		realcolor.s.green = (UINT8)(CALCLIGHT(alpha,realcolor.s.green));
		realcolor.s.blue = (UINT8)(CALCLIGHT(alpha,realcolor.s.blue));

		// Set the surface colors and further modulate the colors by light.
		surfcolor.s.red = (UINT8)(CALCLIGHT((0xFF-alpha),light)+CALCLIGHT(realcolor.s.red,light));
		surfcolor.s.green = (UINT8)(CALCLIGHT((0xFF-alpha),light)+CALCLIGHT(realcolor.s.green,light));
		surfcolor.s.blue = (UINT8)(CALCLIGHT((0xFF-alpha),light)+CALCLIGHT(realcolor.s.blue,light));

		// Modulate the colors by alpha.
		fogcolor.s.red = (UINT8)(CALCLIGHT(fogalpha,fogcolor.s.red));
		fogcolor.s.green = (UINT8)(CALCLIGHT(fogalpha,fogcolor.s.green));
		fogcolor.s.blue = (UINT8)(CALCLIGHT(fogalpha,fogcolor.s.blue));

		// Set the surface colors and further modulate the colors by light.
		surfcolor.s.red = surfcolor.s.red+((UINT8)(CALCLIGHT((0xFF-fogalpha),(255-light))+CALCLIGHT(fogcolor.s.red,(255-light))));
		surfcolor.s.green = surfcolor.s.green+((UINT8)(CALCLIGHT((0xFF-fogalpha),(255-light))+CALCLIGHT(fogcolor.s.green,(255-light))));
		surfcolor.s.blue = surfcolor.s.blue+((UINT8)(CALCLIGHT((0xFF-fogalpha),(255-light))+CALCLIGHT(fogcolor.s.blue,(255-light))));
		surfcolor.s.alpha = 0xFF;
	}

	if(cv_grfog.value)
	{
		if (cv_grsoftwarefog.value)
		{
			fogcolor.s.red = (UINT8)((CALCLIGHT(fogcolor.s.red,(255-light)))+(CALCLIGHT(realcolor.s.red,light)));
			fogcolor.s.green = (UINT8)((CALCLIGHT(fogcolor.s.green,(255-light)))+(CALCLIGHT(realcolor.s.green,light)));
			fogcolor.s.blue = (UINT8)((CALCLIGHT(fogcolor.s.blue,(255-light)))+(CALCLIGHT(realcolor.s.blue,light)));

			// Set the fog options.
			if (cv_grsoftwarefog.value == 1 && plane) // With floors, software draws them way darker for their distance
				HWD.pfnSetSpecialState(HWD_SET_FOG_DENSITY, (INT32)(CALCFOGDENSITYFLOOR(light)));
			else // everything else is drawn like walls
				HWD.pfnSetSpecialState(HWD_SET_FOG_DENSITY, (INT32)(CALCFOGDENSITY(light)));
		}
		else
		{
			fogcolor.s.red = (UINT8)((CALCLIGHT(fogcolor.s.red,(255-light)))+(CALCLIGHT(realcolor.s.red,light)));
			fogcolor.s.green = (UINT8)((CALCLIGHT(fogcolor.s.green,(255-light)))+(CALCLIGHT(realcolor.s.green,light)));
			fogcolor.s.blue = (UINT8)((CALCLIGHT(fogcolor.s.blue,(255-light)))+(CALCLIGHT(realcolor.s.blue,light)));

			fogalpha = (UINT8)((CALCLIGHT(fogalpha,(255-light)))+(CALCLIGHT(alpha,light)));

			// Set the fog options.
			light = (UINT8)(CALCLIGHT(light,(255-fogalpha)));
			HWD.pfnSetSpecialState(HWD_SET_FOG_DENSITY, (INT32)(cv_grfogdensity.value-(cv_grfogdensity.value*(float)light/255.0f)));
		}

		HWD.pfnSetSpecialState(HWD_SET_FOG_COLOR, (fogcolor.s.red*0x10000)+(fogcolor.s.green*0x100)+fogcolor.s.blue);
		HWD.pfnSetSpecialState(HWD_SET_FOG_MODE, 1);
	}
	return surfcolor.rgba;
}

static UINT8 HWR_FogBlockAlpha(INT32 light, UINT32 color, UINT32 fadecolor) // Let's see if this can work
{
	RGBA_t realcolor, fogcolor, surfcolor;
	INT32 alpha, fogalpha;

	realcolor.rgba = color;
	fogcolor.rgba = fadecolor;

	alpha = (realcolor.s.alpha*255)/25;
	fogalpha = (fogcolor.s.alpha*255)/25;

	// Fog blocks seem to get slightly more opaque with more opaque colourmap opacity, and much more opaque with darker brightness
	surfcolor.s.alpha = (UINT8)(CALCLIGHT(light, ((0xFF-light)+alpha)/2)+CALCLIGHT(0xFF-light, ((light)+fogalpha)/2));

	return surfcolor.s.alpha;
}
#endif

// ==========================================================================
//                                   FLOOR/CEILING GENERATION FROM SUBSECTORS
// ==========================================================================

#ifdef DOPLANES

// maximum number of verts around a convex floor/ceiling polygon
#define MAXPLANEVERTICES 2048
static FOutVector  planeVerts[MAXPLANEVERTICES];

// -----------------+
// HWR_RenderPlane  : Render a floor or ceiling convex polygon
// -----------------+
static void HWR_RenderPlane(sector_t *sector, extrasubsector_t *xsub, fixed_t fixedheight,
                           FBITFIELD PolyFlags, INT32 lightlevel, lumpnum_t lumpnum, sector_t *FOFsector, UINT8 alpha, boolean fogplane, extracolormap_t *planecolormap)
{
	polyvertex_t *  pv;
	float           height; //constant y for all points on the convex flat polygon
	FOutVector      *v3d;
	INT32             nrPlaneVerts;   //verts original define of convex flat polygon
	INT32             i;
	float           flatxref,flatyref;
	float fflatsize;
	INT32 flatflag;
	size_t len;
	float scrollx = 0.0f, scrolly = 0.0f;
	angle_t angle = 0;
	FSurfaceInfo    Surf;

	// no convex poly were generated for this subsector
	if (!xsub->planepoly)
		return;

	height = FIXED_TO_FLOAT(fixedheight);

	pv  = xsub->planepoly->pts;
	nrPlaneVerts = xsub->planepoly->numpts;

	if (nrPlaneVerts < 3)   //not even a triangle ?
		return;

	if (nrPlaneVerts > MAXPLANEVERTICES) // FIXME: exceeds plVerts size
	{
		CONS_Printf("polygon size of %d exceeds max value of %d vertices\n", nrPlaneVerts, MAXPLANEVERTICES);
		return;
	}

	len = W_LumpLength(lumpnum);

	switch (len)
	{
		case 4194304: // 2048x2048 lump
			fflatsize = 2048.0f;
			flatflag = 2047;
			break;
		case 1048576: // 1024x1024 lump
			fflatsize = 1024.0f;
			flatflag = 1023;
			break;
		case 262144:// 512x512 lump
			fflatsize = 512.0f;
			flatflag = 511;
			break;
		case 65536: // 256x256 lump
			fflatsize = 256.0f;
			flatflag = 255;
			break;
		case 16384: // 128x128 lump
			fflatsize = 128.0f;
			flatflag = 127;
			break;
		case 1024: // 32x32 lump
			fflatsize = 32.0f;
			flatflag = 31;
			break;
		default: // 64x64 lump
			fflatsize = 64.0f;
			flatflag = 63;
			break;
	}

	// reference point for flat texture coord for each vertex around the polygon
	flatxref = (float)(((fixed_t)pv->x & (~flatflag)) / fflatsize);
	flatyref = (float)(((fixed_t)pv->y & (~flatflag)) / fflatsize);

	// transform
	v3d = planeVerts;

	if (FOFsector != NULL)
	{
		if (fixedheight == FOFsector->floorheight) // it's a floor
		{
			scrollx = FIXED_TO_FLOAT(FOFsector->floor_xoffs)/fflatsize;
			scrolly = FIXED_TO_FLOAT(FOFsector->floor_yoffs)/fflatsize;
			angle = FOFsector->floorpic_angle>>ANGLETOFINESHIFT;
		}
		else // it's a ceiling
		{
			scrollx = FIXED_TO_FLOAT(FOFsector->ceiling_xoffs)/fflatsize;
			scrolly = FIXED_TO_FLOAT(FOFsector->ceiling_yoffs)/fflatsize;
			angle = FOFsector->ceilingpic_angle>>ANGLETOFINESHIFT;
		}
	}
	else if (gr_frontsector)
	{
		if (fixedheight < dup_viewz) // it's a floor
		{
			scrollx = FIXED_TO_FLOAT(gr_frontsector->floor_xoffs)/fflatsize;
			scrolly = FIXED_TO_FLOAT(gr_frontsector->floor_yoffs)/fflatsize;
			angle = gr_frontsector->floorpic_angle>>ANGLETOFINESHIFT;
		}
		else // it's a ceiling
		{
			scrollx = FIXED_TO_FLOAT(gr_frontsector->ceiling_xoffs)/fflatsize;
			scrolly = FIXED_TO_FLOAT(gr_frontsector->ceiling_yoffs)/fflatsize;
			angle = gr_frontsector->ceilingpic_angle>>ANGLETOFINESHIFT;
		}
	}

	fixed_t tempxsow, tempytow;

	if (angle) // Only needs to be done if there's an altered angle
	{
		// This needs to be done so that it scrolls in a different direction after rotation like software
		tempxsow = FLOAT_TO_FIXED(scrollx);
		tempytow = FLOAT_TO_FIXED(scrolly);
		scrollx = (FIXED_TO_FLOAT(FixedMul(tempxsow, FINECOSINE(angle)) - FixedMul(tempytow, FINESINE(angle))));
		scrolly = (FIXED_TO_FLOAT(FixedMul(tempxsow, FINESINE(angle)) + FixedMul(tempytow, FINECOSINE(angle))));

		// This needs to be done so everything aligns after rotation
		// It would be done so that rotation is done, THEN the translation, but I couldn't get it to rotate AND scroll like software does
		tempxsow = FLOAT_TO_FIXED(flatxref);
		tempytow = FLOAT_TO_FIXED(flatyref);
		flatxref = (FIXED_TO_FLOAT(FixedMul(tempxsow, FINECOSINE(angle)) - FixedMul(tempytow, FINESINE(angle))));
		flatyref = (FIXED_TO_FLOAT(FixedMul(tempxsow, FINESINE(angle)) + FixedMul(tempytow, FINECOSINE(angle))));
	}

	for (i = 0; i < nrPlaneVerts; i++,v3d++,pv++)
	{
		// Hurdler: add scrolling texture on floor/ceiling
		v3d->sow = (float)((pv->x / fflatsize) - flatxref + scrollx);
		v3d->tow = (float)(flatyref - (pv->y / fflatsize) + scrolly);

		//v3d->sow = (float)(pv->x / fflatsize);
		//v3d->tow = (float)(pv->y / fflatsize);

		// Need to rotate before translate
		if (angle) // Only needs to be done if there's an altered angle
		{
			tempxsow = FLOAT_TO_FIXED(v3d->sow);
			tempytow = FLOAT_TO_FIXED(v3d->tow);
			v3d->sow = (FIXED_TO_FLOAT(FixedMul(tempxsow, FINECOSINE(angle)) - FixedMul(tempytow, FINESINE(angle))));
			v3d->tow = (FIXED_TO_FLOAT(-FixedMul(tempxsow, FINESINE(angle)) - FixedMul(tempytow, FINECOSINE(angle))));
		}

		//v3d->sow = (float)(v3d->sow - flatxref + scrollx);
		//v3d->tow = (float)(flatyref - v3d->tow + scrolly);

		v3d->x = pv->x;
		v3d->y = height;
		v3d->z = pv->y;
#ifdef SLOPENESS
		if (sector && sector->special == 65535)
		{
			size_t q;
			for (q = 0; q < sector->linecount; q++)
			{
				if (v3d->x == sector->lines[q]->v1->x>>FRACBITS)
				{
					if (v3d->z == sector->lines[q]->v1->y>>FRACBITS)
					{
						v3d->y += sector->lines[q]->v1->z>>FRACBITS;
						break;
					}
				}
			}
		}
#else
		sector = NULL;
#endif
	}

	// only useful for flat coloured triangles
	//Surf.FlatColor = 0xff804020;

	// use different light tables
	// for horizontal / vertical / diagonal
	// note: try to get the same visual feel as the original
#ifndef HARDWAREFIX
	Surf.FlatColor.s.red = Surf.FlatColor.s.green =
	Surf.FlatColor.s.blue = LightLevelToLum(lightlevel); //  Don't take from the frontsector, or the game will crash
#endif

/*	// colormap test
	if (gr_frontsector)
	{
		sector_t *psector = gr_frontsector;

		if (psector->ffloors)
		{
			ffloor_t *caster = psector->lightlist[R_GetPlaneLight(psector, fixedheight, false)].caster;
			psector = caster ? &sectors[caster->secnum] : psector;

			if (caster)
			{
				lightlevel = psector->lightlevel;
#ifndef HARDWAREFIX
				Surf.FlatColor.s.red = Surf.FlatColor.s.green = Surf.FlatColor.s.blue = LightLevelToLum(lightlevel);
#endif
			}
		}
		if (psector->extra_colormap)
#ifdef HARDWAREFIX
			Surf.FlatColor.rgba = HWR_Lighting(LightLevelToLum(lightlevel),psector->extra_colormap->rgba,psector->extra_colormap->fadergba, false, true);
		else
			Surf.FlatColor.rgba = HWR_Lighting(LightLevelToLum(lightlevel),NORMALFOG,FADEFOG, false, true);
#else
		{
			RGBA_t temp;
			INT32 alpha;

			temp.rgba = psector->extra_colormap->rgba;
			alpha = (26-temp.s.alpha)*LightLevelToLum(lightlevel);
			Surf.FlatColor.s.red = (UINT8)((alpha + temp.s.alpha*temp.s.red)/26);
			Surf.FlatColor.s.blue = (UINT8)((alpha + temp.s.alpha*temp.s.blue)/26);
			Surf.FlatColor.s.green = (UINT8)((alpha + temp.s.alpha*temp.s.green)/26);
			Surf.FlatColor.s.alpha = 0xff;
		}
#endif
	}
#ifdef HARDWAREFIX
	else
		Surf.FlatColor.rgba = HWR_Lighting(LightLevelToLum(lightlevel),NORMALFOG,FADEFOG, false, true);
#endif*/

	if (planecolormap)
		if (fogplane)
			Surf.FlatColor.rgba = HWR_Lighting(LightLevelToLum(lightlevel), planecolormap->rgba, planecolormap->fadergba, true, false);
		else
			Surf.FlatColor.rgba = HWR_Lighting(LightLevelToLum(lightlevel), planecolormap->rgba, planecolormap->fadergba, false, true);
	else
		if (fogplane)
			Surf.FlatColor.rgba = HWR_Lighting(LightLevelToLum(lightlevel), NORMALFOG, FADEFOG, true, false);
		else
			Surf.FlatColor.rgba = HWR_Lighting(LightLevelToLum(lightlevel), NORMALFOG, FADEFOG, false, true);

	if (PolyFlags & PF_Translucent)
	{
		Surf.FlatColor.s.alpha = alpha;
		HWD.pfnDrawPolygon(&Surf, planeVerts, nrPlaneVerts,
		                   PolyFlags|PF_Modulated|PF_Occlude|PF_Clip);
	}
	else
	{
		Surf.FlatColor.s.alpha = 0xff;
		HWD.pfnDrawPolygon(&Surf, planeVerts, nrPlaneVerts,
		                   PolyFlags|PF_Masked|PF_Modulated|PF_Clip);
	}
}

#ifdef POLYSKY
// this don't draw anything it only update the z-buffer so there isn't problem with
// wall/things upper that sky (map12)
static void HWR_RenderSkyPlane(extrasubsector_t *xsub, fixed_t fixedheight)
{
	polyvertex_t *pv;
	float height; //constant y for all points on the convex flat polygon
	FOutVector *v3d;
	INT32 nrPlaneVerts;   //verts original define of convex flat polygon
	INT32 i;

	// no convex poly were generated for this subsector
	if (!xsub->planepoly)
		return;

	height = FIXED_TO_FLOAT(fixedheight);

	pv  = xsub->planepoly->pts;
	nrPlaneVerts = xsub->planepoly->numpts;

	if (nrPlaneVerts < 3) // not even a triangle?
		return;

	if (nrPlaneVerts > MAXPLANEVERTICES) // FIXME: exceeds plVerts size
	{
		CONS_Printf("polygon size of %d exceeds max value of %d vertices\n", nrPlaneVerts, MAXPLANEVERTICES);
		return;
	}

	// transform
	v3d = planeVerts;
	for (i = 0; i < nrPlaneVerts; i++,v3d++,pv++)
	{
		v3d->sow = 0.0f;
		v3d->tow = 0.0f;
		v3d->x = pv->x;
		v3d->y = height;
		v3d->z = pv->y;
	}

	HWD.pfnDrawPolygon(NULL, planeVerts, nrPlaneVerts,
	 PF_Clip|PF_Invisible|PF_NoTexture|PF_Occlude);
}
#endif //polysky

#endif //doplanes

/*
   wallVerts order is :
		3--2
		| /|
		|/ |
		0--1
*/
#ifdef WALLSPLATS
static void HWR_DrawSegsSplats(FSurfaceInfo * pSurf)
{
	FOutVector trVerts[4], *wv;
	wallVert3D wallVerts[4];
	wallVert3D *pwallVerts;
	wallsplat_t *splat;
	GLPatch_t *gpatch;
	fixed_t i;
	FSurfaceInfo pSurf2;
	// seg bbox
	fixed_t segbbox[4];

	M_ClearBox(segbbox);
	M_AddToBox(segbbox,
		FLOAT_TO_FIXED(((polyvertex_t *)gr_curline->v1)->x),
		FLOAT_TO_FIXED(((polyvertex_t *)gr_curline->v1)->y));
	M_AddToBox(segbbox,
		FLOAT_TO_FIXED(((polyvertex_t *)gr_curline->v2)->x),
		FLOAT_TO_FIXED(((polyvertex_t *)gr_curline->v2)->y));

	splat = (wallsplat_t *)gr_curline->linedef->splats;
	for (; splat; splat = splat->next)
	{
		//BP: don't draw splat extern to this seg
		//    this is quick fix best is explain in logboris.txt at 12-4-2000
		if (!M_PointInBox(segbbox,splat->v1.x,splat->v1.y) && !M_PointInBox(segbbox,splat->v2.x,splat->v2.y))
			continue;

		gpatch = W_CachePatchNum(splat->patch, PU_CACHE);
		HWR_GetPatch(gpatch);

		wallVerts[0].x = wallVerts[3].x = FIXED_TO_FLOAT(splat->v1.x);
		wallVerts[0].z = wallVerts[3].z = FIXED_TO_FLOAT(splat->v1.y);
		wallVerts[2].x = wallVerts[1].x = FIXED_TO_FLOAT(splat->v2.x);
		wallVerts[2].z = wallVerts[1].z = FIXED_TO_FLOAT(splat->v2.y);

		i = splat->top;
		if (splat->yoffset)
			i += *splat->yoffset;

		wallVerts[2].y = wallVerts[3].y = FIXED_TO_FLOAT(i)+(gpatch->height>>1);
		wallVerts[0].y = wallVerts[1].y = FIXED_TO_FLOAT(i)-(gpatch->height>>1);

		wallVerts[3].s = wallVerts[3].t = wallVerts[2].s = wallVerts[0].t = 0.0f;
		wallVerts[1].s = wallVerts[1].t = wallVerts[2].t = wallVerts[0].s = 1.0f;

		// transform
		wv = trVerts;
		pwallVerts = wallVerts;
		for (i = 0; i < 4; i++,wv++,pwallVerts++)
		{
			wv->x   = pwallVerts->x;
			wv->z = pwallVerts->z;
			wv->y   = pwallVerts->y;

			// Kalaron: TOW and SOW needed to be switched
			wv->sow = pwallVerts->t;
			wv->tow = pwallVerts->s;
		}
		M_Memcpy(&pSurf2,pSurf,sizeof (FSurfaceInfo));
		switch (splat->flags & SPLATDRAWMODE_MASK)
		{
			case SPLATDRAWMODE_OPAQUE :
				pSurf2.FlatColor.s.alpha = 0xff;
				i = PF_Translucent;
				break;
			case SPLATDRAWMODE_TRANS :
				pSurf2.FlatColor.s.alpha = 128;
				i = PF_Translucent;
				break;
			case SPLATDRAWMODE_SHADE :
				pSurf2.FlatColor.s.alpha = 0xff;
				i = PF_Substractive;
				break;
		}

		HWD.pfnDrawPolygon(&pSurf2, trVerts, 4, i|PF_Modulated|PF_Clip|PF_Decal);
	}
}
#endif

// ==========================================================================
//                                        WALL GENERATION FROM SUBSECTOR SEGS
// ==========================================================================


FBITFIELD HWR_TranstableToAlpha(INT32 transtablenum, FSurfaceInfo *pSurf)
{
	switch (transtablenum)
	{
		case tr_trans10 : pSurf->FlatColor.s.alpha = 0xe6;return  PF_Translucent;
		case tr_trans20 : pSurf->FlatColor.s.alpha = 0xcc;return  PF_Translucent;
		case tr_trans30 : pSurf->FlatColor.s.alpha = 0xb3;return  PF_Translucent;
		case tr_trans40 : pSurf->FlatColor.s.alpha = 0x99;return  PF_Translucent;
		case tr_trans50 : pSurf->FlatColor.s.alpha = 0x80;return  PF_Translucent;
		case tr_trans60 : pSurf->FlatColor.s.alpha = 0x66;return  PF_Translucent;
		case tr_trans70 : pSurf->FlatColor.s.alpha = 0x4c;return  PF_Translucent;
		case tr_trans80 : pSurf->FlatColor.s.alpha = 0x33;return  PF_Translucent;
		case tr_trans90 : pSurf->FlatColor.s.alpha = 0x19;return  PF_Translucent;
	}
	return PF_Translucent;
}

// v1,v2 : the start & end vertices along the original wall segment, that may have been
//         clipped so that only a visible portion of the wall seg is drawn.
// floorheight, ceilingheight : depend on wall upper/lower/middle, comes from the sectors.

static void HWR_AddTransparentWall(wallVert3D *wallVerts, FSurfaceInfo * pSurf, INT32 texnum, FBITFIELD blend, boolean fogwall, INT32 lightlevel, extracolormap_t *wallcolormap);

// -----------------+
// HWR_ProjectWall  :
// -----------------+
/*
   wallVerts order is :
		3--2
		| /|
		|/ |
		0--1
*/
static void HWR_ProjectWall(wallVert3D   * wallVerts,
                                    FSurfaceInfo * pSurf,
                                    FBITFIELD blendmode, INT32 lightlevel, extracolormap_t *wallcolormap)
{
	FOutVector  trVerts[4];
	FOutVector  *wv;

	// transform
	wv = trVerts;
	// it sounds really stupid to do this conversion with the new T&L code
	// we should directly put the right information in the right structure
	// wallVerts3D seems ok, doesn't need FOutVector
	// also remove the light copy

	// More messy to unwrap, but it's also quicker, uses less memory.
	wv->sow = wallVerts->s;
	wv->tow = wallVerts->t;
	wv->x   = wallVerts->x;
	wv->y   = wallVerts->y;
	wv->z   = wallVerts->z;
	wv++; wallVerts++;
	wv->sow = wallVerts->s;
	wv->tow = wallVerts->t;
	wv->x   = wallVerts->x;
	wv->y   = wallVerts->y;
	wv->z   = wallVerts->z;
	wv++; wallVerts++;
	wv->sow = wallVerts->s;
	wv->tow = wallVerts->t;
	wv->x   = wallVerts->x;
	wv->y   = wallVerts->y;
	wv->z   = wallVerts->z;
	wv++; wallVerts++;
	wv->sow = wallVerts->s;
	wv->tow = wallVerts->t;
	wv->x   = wallVerts->x;
	wv->y   = wallVerts->y;
	wv->z   = wallVerts->z;

	if (wallcolormap)
	{
		pSurf->FlatColor.rgba = HWR_Lighting(LightLevelToLum(lightlevel), wallcolormap->rgba, wallcolormap->fadergba, false, false);
	}
	else
	{
		pSurf->FlatColor.rgba = HWR_Lighting(LightLevelToLum(lightlevel), NORMALFOG, FADEFOG, false, false);
	}

	HWD.pfnDrawPolygon(pSurf, trVerts, 4, blendmode|PF_Modulated|PF_Occlude|PF_Clip);

#ifdef WALLSPLATS
	if (gr_curline->linedef->splats && cv_splats.value)
		HWR_DrawSegsSplats(pSurf);
#endif
}

// ==========================================================================
//                                                          BSP, CULL, ETC..
// ==========================================================================

// return the frac from the interception of the clipping line
// (in fact a clipping plane that has a constant, so can clip with simple 2d)
// with the wall segment
//
static float HWR_ClipViewSegment(INT32 x, polyvertex_t *v1, polyvertex_t *v2)
{
	float num, den;
	float v1x, v1y, v1dx, v1dy, v2dx, v2dy;
	angle_t pclipangle = gr_xtoviewangle[x];

	// a segment of a polygon
	v1x  = v1->x;
	v1y  = v1->y;
	v1dx = (v2->x - v1->x);
	v1dy = (v2->y - v1->y);

	// the clipping line
	pclipangle = pclipangle + dup_viewangle; //back to normal angle (non-relative)
	v2dx = FIXED_TO_FLOAT(FINECOSINE(pclipangle>>ANGLETOFINESHIFT));
	v2dy = FIXED_TO_FLOAT(FINESINE(pclipangle>>ANGLETOFINESHIFT));

	den = v2dy*v1dx - v2dx*v1dy;
	if (den == 0)
		return -1; // parallel

	// calc the frac along the polygon segment,
	//num = (v2x - v1x)*v2dy + (v1y - v2y)*v2dx;
	//num = -v1x * v2dy + v1y * v2dx;
	num = (gr_viewx - v1x)*v2dy + (v1y - gr_viewy)*v2dx;

	return num / den;
}

//
// HWR_SplitWall
//
static void HWR_SplitWall(sector_t *sector, wallVert3D *wallVerts, INT32 texnum, FSurfaceInfo* Surf, INT32 cutflag)
{
	/* SoM: split up and light walls according to the
	 lightlist. This may also include leaving out parts
	 of the wall that can't be seen */
	GLTexture_t * glTex;
	float realtop, realbot, top, bot;
	float pegt, pegb, pegmul;
	float height = 0.0f, bheight = 0.0f;
	INT32   solid, i;
	lightlist_t *  list = sector->lightlist;
#ifdef HARDWAREFIX
	const UINT8 alpha = Surf->FlatColor.s.alpha;
#endif

	realtop = top = wallVerts[2].y;
	realbot = bot = wallVerts[0].y;
	pegt = wallVerts[2].t;
	pegb = wallVerts[0].t;
	pegmul = (pegb - pegt) / (top - bot);

	for (i = 1; i < sector->numlights; i++)
	{
		if (top < realbot)
			return;

	//Hurdler: fix a crashing bug, but is it correct?
//		if (!list[i].caster)
//			continue;

		solid = false;

		if (list[i].caster)
		{
			if (sector->lightlist[i].caster->flags & FF_SOLID && !(cutflag & FF_EXTRA))
				solid = true;
			else if (sector->lightlist[i].caster->flags & FF_CUTEXTRA && cutflag & FF_EXTRA)
			{
				if (sector->lightlist[i].caster->flags & FF_EXTRA)
				{
					if (sector->lightlist[i].caster->flags == cutflag) // Only merge with your own types
						solid = true;
				}
				else
					solid = true;
			}
			else
				solid = false;
		}
		else
			solid = false;

		if (cutflag == FF_CUTSOLIDS) // These are regular walls sent in from StoreWallRange, they shouldn't be cut from this
			solid = false;

		if (cutflag & FF_SOLID) // these weren't being cut before anyway, although they probably should be in the right conditions
			solid = false;


		height = FIXED_TO_FLOAT(list[i].height);
		if (solid)
			bheight = FIXED_TO_FLOAT(*list[i].caster->bottomheight);

		if (height >= top)
		{
			if (solid && top > bheight)
				top = bheight;
			continue;
		}

		FUINT lightnum;
		extracolormap_t *colormap;

		//Found a break;
		bot = height;

		if (bot < realbot)
			bot = realbot;

		{


#ifndef HARDWAREFIX
			lightnum = LightLevelToLum(*list[i-1].lightlevel);
			// store Surface->FlatColor to modulate wall texture
			Surf->FlatColor.s.red = Surf->FlatColor.s.green = Surf->FlatColor.s.blue =
				(UINT8)lightnum;
#endif

			// colormap test
#ifdef HARDWAREFIX
			if (list[i-1].caster)
			{
				lightnum = *list[i-1].lightlevel;
				colormap = list[i-1].extra_colormap;
			}
			else
			{
				lightnum = sector->lightlevel;
				colormap = sector->extra_colormap;
			}
#endif
#ifdef HARDWAREFIX
			Surf->FlatColor.s.alpha = alpha;
#else
			{
				RGBA_t temp;
				INT32 alpha;

				temp.rgba = psector->extra_colormap->rgba;
				alpha = (INT32)((26 - temp.s.alpha)*lightnum);

				Surf->FlatColor.s.red =
					(UINT8)((alpha + temp.s.alpha*temp.s.red)/26);
				Surf->FlatColor.s.blue =
					(UINT8)((alpha + temp.s.alpha*temp.s.blue)/26);
				Surf->FlatColor.s.green =
					(UINT8)((alpha + temp.s.alpha*temp.s.green)/26);
				Surf->FlatColor.s.alpha = 0xff;
			}
#endif
		}

		wallVerts[3].t = wallVerts[2].t = pegt + ((realtop - top) * pegmul);
		wallVerts[0].t = wallVerts[1].t = pegt + ((realtop - bot) * pegmul);

		// set top/bottom coords
		wallVerts[2].y = wallVerts[3].y = top;
		wallVerts[0].y = wallVerts[1].y = bot;

		glTex = HWR_GetTexture(texnum);
		if (cutflag & FF_TRANSLUCENT)
			HWR_AddTransparentWall(wallVerts, Surf, texnum, PF_Translucent, false, lightnum, colormap);
		else if (glTex->mipmap.flags & TF_TRANSPARENT)
			HWR_AddTransparentWall(wallVerts, Surf, texnum, PF_Environment, false, lightnum, colormap);
		else
			HWR_ProjectWall(wallVerts, Surf, PF_Masked, lightnum, colormap);

		if (solid)
			top = bheight;
		else
			top = height;
	}

	FUINT lightnum;
	extracolormap_t *colormap;

	bot = realbot;
	if (top <= realbot)
		return;

	{
#ifndef HARDWAREFIX
		lightnum = LightLevelToLum(*list[i-1].lightlevel);
		// store Surface->FlatColor to modulate wall texture
		Surf->FlatColor.s.red = Surf->FlatColor.s.green = Surf->FlatColor.s.blue
			= (UINT8)lightnum;
#endif

#ifdef HARDWAREFIX
		if (list[i-1].caster)
		{
			lightnum = *list[i-1].lightlevel;
			colormap = list[i-1].extra_colormap;
		}
		else
		{
			lightnum = sector->lightlevel;
			colormap = sector->extra_colormap;
		}
#endif
#ifdef HARDWAREFIX
		Surf->FlatColor.s.alpha = alpha;
#else
		{
			RGBA_t  temp;
			INT32     alpha;

			temp.rgba = psector->extra_colormap->rgba;
			alpha = (INT32)((26 - temp.s.alpha)*lightnum);
			Surf->FlatColor.s.red = (UINT8)((alpha + temp.s.alpha*temp.s.red)/26);
			Surf->FlatColor.s.blue = (UINT8)((alpha + temp.s.alpha*temp.s.blue)/26);
			Surf->FlatColor.s.green = (UINT8)((alpha + temp.s.alpha*temp.s.green)/26);
			Surf->FlatColor.s.alpha = 0xFF;
		}
#endif
	}

	wallVerts[3].t = wallVerts[2].t = pegt + ((realtop - top) * pegmul);
	wallVerts[0].t = wallVerts[1].t = pegt + ((realtop - bot) * pegmul);

	// set top/bottom coords
	wallVerts[2].y = wallVerts[3].y = top;
	wallVerts[0].y = wallVerts[1].y = bot;

	glTex = HWR_GetTexture(texnum);
	if (cutflag & FF_TRANSLUCENT)
		HWR_AddTransparentWall(wallVerts, Surf, texnum, PF_Translucent, false, lightnum, colormap);
	else if (glTex->mipmap.flags & TF_TRANSPARENT)
		HWR_AddTransparentWall(wallVerts, Surf, texnum, PF_Environment, false, lightnum, colormap);
	else
		HWR_ProjectWall(wallVerts, Surf, PF_Masked, lightnum, colormap);
}


//
// HWR_SplitFog
// Exclusively for fog
//
static void HWR_SplitFog(sector_t *sector, wallVert3D *wallVerts, FSurfaceInfo* Surf, INT32 cutflag, FUINT lightnum, extracolormap_t *colormap)
{
	/* SoM: split up and light walls according to the
	 lightlist. This may also include leaving out parts
	 of the wall that can't be seen */
	float realtop, realbot, top, bot;
	float pegt, pegb, pegmul;
	float height = 0.0f, bheight = 0.0f;
	INT32   solid, i;
	lightlist_t *  list = sector->lightlist;
#ifdef HARDWAREFIX
	const UINT8 alpha = Surf->FlatColor.s.alpha;
#endif

	realtop = top = wallVerts[2].y;
	realbot = bot = wallVerts[0].y;
	pegt = wallVerts[2].t;
	pegb = wallVerts[0].t;
	pegmul = (pegb - pegt) / (top - bot);

	for (i = 1; i < sector->numlights; i++)
	{
		if (top < realbot)
			return;

	//Hurdler: fix a crashing bug, but is it correct?
//		if (!list[i].caster)
//			continue;

		solid = false;

		if (list[i].caster)
		{
			if (sector->lightlist[i].caster->flags & FF_SOLID && !(cutflag & FF_EXTRA))
				solid = true;
			else if (sector->lightlist[i].caster->flags & FF_CUTEXTRA && cutflag & FF_EXTRA)
			{
				if (sector->lightlist[i].caster->flags & FF_EXTRA)
				{
					if (sector->lightlist[i].caster->flags == cutflag)
						solid = true;
				}
				else
					solid = true;
			}
			else
				solid = false;
		}
		else
			solid = false;

		height = FIXED_TO_FLOAT(list[i].height);

		if (solid)
			bheight = FIXED_TO_FLOAT(*list[i].caster->bottomheight);

		if (height >= top)
		{
			if (solid && top > bheight)
				top = bheight;
			continue;
		}

		//Found a break;
		bot = height;

		if (bot < realbot)
			bot = realbot;

		{


#ifndef HARDWAREFIX
			lightnum = LightLevelToLum(*list[i-1].lightlevel);
			// store Surface->FlatColor to modulate wall texture
			Surf->FlatColor.s.red = Surf->FlatColor.s.green = Surf->FlatColor.s.blue =
				(UINT8)lightnum;
#endif

#ifdef HARDWAREFIX
			Surf->FlatColor.s.alpha = alpha;
#else
			{
				RGBA_t temp;
				INT32 alpha;

				temp.rgba = psector->extra_colormap->rgba;
				alpha = (INT32)((26 - temp.s.alpha)*lightnum);

				Surf->FlatColor.s.red =
					(UINT8)((alpha + temp.s.alpha*temp.s.red)/26);
				Surf->FlatColor.s.blue =
					(UINT8)((alpha + temp.s.alpha*temp.s.blue)/26);
				Surf->FlatColor.s.green =
					(UINT8)((alpha + temp.s.alpha*temp.s.green)/26);
				Surf->FlatColor.s.alpha = 0xff;
			}
#endif
		}

		wallVerts[3].t = wallVerts[2].t = pegt + ((realtop - top) * pegmul);
		wallVerts[0].t = wallVerts[1].t = pegt + ((realtop - bot) * pegmul);

		// set top/bottom coords
		wallVerts[2].y = wallVerts[3].y = top;
		wallVerts[0].y = wallVerts[1].y = bot;

		if (!solid) // Don't draw it if there's more fog behind it
			HWR_AddTransparentWall(wallVerts, Surf, 0, PF_Translucent|PF_NoTexture, true, lightnum, colormap);

		top = height;
	}

	bot = realbot;
	if (top <= realbot)
		return;

	{
#ifndef HARDWAREFIX
		lightnum = LightLevelToLum(*list[i-1].lightlevel);
		// store Surface->FlatColor to modulate wall texture
		Surf->FlatColor.s.red = Surf->FlatColor.s.green = Surf->FlatColor.s.blue
			= (UINT8)lightnum;
#endif

#ifdef HARDWAREFIX
		Surf->FlatColor.s.alpha = alpha;
#else
		{
			RGBA_t  temp;
			INT32     alpha;

			temp.rgba = psector->extra_colormap->rgba;
			alpha = (INT32)((26 - temp.s.alpha)*lightnum);
			Surf->FlatColor.s.red = (UINT8)((alpha + temp.s.alpha*temp.s.red)/26);
			Surf->FlatColor.s.blue = (UINT8)((alpha + temp.s.alpha*temp.s.blue)/26);
			Surf->FlatColor.s.green = (UINT8)((alpha + temp.s.alpha*temp.s.green)/26);
			Surf->FlatColor.s.alpha = 0xFF;
		}
#endif
	}

	wallVerts[3].t = wallVerts[2].t = pegt + ((realtop - top) * pegmul);
	wallVerts[0].t = wallVerts[1].t = pegt + ((realtop - bot) * pegmul);

	// set top/bottom coords
	wallVerts[2].y = wallVerts[3].y = top;
	wallVerts[0].y = wallVerts[1].y = bot;

	HWR_AddTransparentWall(wallVerts, Surf, 0, PF_Translucent|PF_NoTexture, true, lightnum, colormap);
}


//
// HWR_StoreWallRange
// A portion or all of a wall segment will be drawn, from startfrac to endfrac,
//  where 0 is the start of the segment, 1 the end of the segment
// Anything between means the wall segment has been clipped with solidsegs,
//  reducing wall overdraw to a minimum
//
static void HWR_StoreWallRange(double startfrac, double endfrac)
{
	wallVert3D wallVerts[4];
	v2d_t vs, ve; // start, end vertices of 2d line (view from above)

	fixed_t worldtop, worldbottom;
	fixed_t worldhigh = 0, worldlow = 0;

	GLTexture_t *grTex = NULL;
	float cliplow = 0.0f, cliphigh = 0.0f;
	INT32 gr_midtexture;
	fixed_t h, l; // 3D sides and 2s middle textures

	FUINT lightnum = 0; // shut up compiler
	extracolormap_t *colormap;
	FSurfaceInfo Surf;

	if (startfrac > endfrac)
		return;

	gr_sidedef = gr_curline->sidedef;
	gr_linedef = gr_curline->linedef;

	if (gr_frontsector->heightsec != -1)
	{
		worldtop = sectors[gr_frontsector->heightsec].ceilingheight;
		worldbottom = sectors[gr_frontsector->heightsec].floorheight;
	}
	else
	{
		worldtop    = gr_frontsector->ceilingheight;
		worldbottom = gr_frontsector->floorheight;
	}

	vs.x = ((polyvertex_t *)gr_curline->v1)->x;
	vs.y = ((polyvertex_t *)gr_curline->v1)->y;
	ve.x = ((polyvertex_t *)gr_curline->v2)->x;
	ve.y = ((polyvertex_t *)gr_curline->v2)->y;

	// remember vertices ordering
	//  3--2
	//  | /|
	//  |/ |
	//  0--1
	// make a wall polygon (with 2 triangles), using the floor/ceiling heights,
	// and the 2d map coords of start/end vertices
	wallVerts[0].x = wallVerts[3].x = vs.x;
	wallVerts[0].z = wallVerts[3].z = vs.y;
	wallVerts[2].x = wallVerts[1].x = ve.x;
	wallVerts[2].z = wallVerts[1].z = ve.y;
	wallVerts[0].w = wallVerts[1].w = wallVerts[2].w = wallVerts[3].w = 1.0f;

	if (drawtextured)
	{
		// x offset the texture
		fixed_t texturehpeg = gr_sidedef->textureoffset + gr_curline->offset;

		// clip texture s start/end coords with solidsegs
		if (startfrac > 0.0f && startfrac < 1.0f)
			cliplow = (float)(texturehpeg + (gr_curline->flength*FRACUNIT) * startfrac);
		else
			cliplow = (float)texturehpeg;

		if (endfrac > 0.0f && endfrac < 1.0f)
			cliphigh = (float)(texturehpeg + (gr_curline->flength*FRACUNIT) * endfrac);
		else
			cliphigh = (float)(texturehpeg + (gr_curline->flength*FRACUNIT));
	}

#ifdef HARDWAREFIX
	lightnum = gr_frontsector->lightlevel;
	colormap = gr_frontsector->extra_colormap;
#else
	//  use different light tables
	//  for horizontal / vertical / diagonal
	//  note: try to get the same visual feel as the original
	Surf.FlatColor.s.alpha = 0xff;

	lightnum = LightLevelToLum(gr_frontsector->lightlevel);

	if (((polyvertex_t *)gr_curline->v1)->y == ((polyvertex_t *)gr_curline->v2)->y
		&& lightnum >= (255/LIGHTLEVELS))
	{
		lightnum -= 255/LIGHTLEVELS;
	}
	else if (((polyvertex_t *)gr_curline->v1)->x == ((polyvertex_t *)gr_curline->v2)->x
		&& lightnum < 255 - (255/LIGHTLEVELS))
	{
		lightnum += 255/LIGHTLEVELS;
	}

	// store Surface->FlatColor to modulate wall texture
	Surf.FlatColor.s.red = Surf.FlatColor.s.green = Surf.FlatColor.s.blue =
		(UINT8)lightnum;
#endif

	if (gr_frontsector)
	{
		Surf.FlatColor.s.alpha = 255;
#ifndef HARDWAREFIX
		{
			RGBA_t temp;
			INT32 alpha;

			temp.rgba = sector->extra_colormap->rgba;
			alpha = (INT32)((26 - temp.s.alpha)*lightnum);
			Surf.FlatColor.s.red =
				(UINT8)((alpha + temp.s.alpha*temp.s.red)/26);
			Surf.FlatColor.s.blue =
				(UINT8)((alpha + temp.s.alpha*temp.s.blue)/26);
			Surf.FlatColor.s.green =
				(UINT8)((alpha + temp.s.alpha*temp.s.green)/26);
			Surf.FlatColor.s.alpha = 0xff;
		}
#endif
	}

	if (gr_backsector)
	{
		// two sided line
		if (gr_backsector->heightsec != -1)
		{
			worldhigh = sectors[gr_backsector->heightsec].ceilingheight;
			worldlow = sectors[gr_backsector->heightsec].floorheight;
		}
		else
		{
			worldhigh = gr_backsector->ceilingheight;
			worldlow  = gr_backsector->floorheight;
		}

		// hack to allow height changes in outdoor areas
		// This is what gets rid of the upper textures if there should be sky
		if (gr_frontsector->ceilingpic == skyflatnum &&
			gr_backsector->ceilingpic  == skyflatnum)
		{
			worldtop = worldhigh;
		}

		// check TOP TEXTURE
		if (worldhigh < worldtop && texturetranslation[gr_sidedef->toptexture]
#ifdef POLYOBJECTS // polyobjects don't have top textures, silly.
		&& !gr_curline->polyseg
#endif
			)
		{
			if (drawtextured)
			{
				fixed_t texturevpegtop; // top

				grTex = HWR_GetTexture(texturetranslation[gr_sidedef->toptexture]);

				// PEGGING
				if (gr_linedef->flags & ML_DONTPEGTOP)
					texturevpegtop = 0;
				else
					texturevpegtop = worldhigh + textureheight[gr_sidedef->toptexture] - worldtop;

				texturevpegtop += gr_sidedef->rowoffset;

				wallVerts[3].t = wallVerts[2].t = texturevpegtop * grTex->scaleY;
				wallVerts[0].t = wallVerts[1].t = (texturevpegtop + worldtop - worldhigh) * grTex->scaleY;
				wallVerts[0].s = wallVerts[3].s = cliplow * grTex->scaleX;
				wallVerts[2].s = wallVerts[1].s = cliphigh * grTex->scaleX;
			}

			// set top/bottom coords
			wallVerts[2].y = wallVerts[3].y = FIXED_TO_FLOAT(worldtop);
			wallVerts[0].y = wallVerts[1].y = FIXED_TO_FLOAT(worldhigh);

			if (gr_frontsector->numlights)
				HWR_SplitWall(gr_frontsector, wallVerts, texturetranslation[gr_sidedef->toptexture], &Surf, FF_CUTSOLIDS);
			else if (grTex->mipmap.flags & TF_TRANSPARENT)
				HWR_AddTransparentWall(wallVerts, &Surf, texturetranslation[gr_sidedef->toptexture], PF_Environment, false, lightnum, colormap);
			else
				HWR_ProjectWall(wallVerts, &Surf, PF_Masked, lightnum, colormap);
		}

		// check BOTTOM TEXTURE
		if (worldlow > worldbottom && texturetranslation[gr_sidedef->bottomtexture]
#ifdef POLYOBJECTS // polyobjects don't have bottom textures, silly.
		&& !gr_curline->polyseg
#endif
			) //only if VISIBLE!!!
		{
			if (drawtextured)
			{
				fixed_t texturevpegbottom = 0; // bottom

				grTex = HWR_GetTexture(texturetranslation[gr_sidedef->bottomtexture]);

				// PEGGING
				if (gr_linedef->flags & ML_DONTPEGBOTTOM)
					texturevpegbottom = worldtop - worldlow;
				else
					texturevpegbottom = 0;

				texturevpegbottom += gr_sidedef->rowoffset;

				wallVerts[3].t = wallVerts[2].t = texturevpegbottom * grTex->scaleY;
				wallVerts[0].t = wallVerts[1].t = (texturevpegbottom + worldlow - worldbottom) * grTex->scaleY;
				wallVerts[0].s = wallVerts[3].s = cliplow * grTex->scaleX;
				wallVerts[2].s = wallVerts[1].s = cliphigh * grTex->scaleX;
			}

			// set top/bottom coords
			wallVerts[2].y = wallVerts[3].y = FIXED_TO_FLOAT(worldlow);
			wallVerts[0].y = wallVerts[1].y = FIXED_TO_FLOAT(worldbottom);

			if (gr_frontsector->numlights)
				HWR_SplitWall(gr_frontsector, wallVerts, texturetranslation[gr_sidedef->bottomtexture], &Surf, FF_CUTSOLIDS);
			else if (grTex->mipmap.flags & TF_TRANSPARENT)
				HWR_AddTransparentWall(wallVerts, &Surf, texturetranslation[gr_sidedef->bottomtexture], PF_Environment, false, lightnum, colormap);
			else
				HWR_ProjectWall(wallVerts, &Surf, PF_Masked, lightnum, colormap);
		}
		gr_midtexture = texturetranslation[gr_sidedef->midtexture];
		if (gr_midtexture)
		{
			FBITFIELD blendmode;
			sector_t *front, *back;
			fixed_t  popentop, popenbottom, polytop, polybottom, lowcut, highcut;
			fixed_t     texturevpeg = 0;
			INT32 repeats;

			if (gr_linedef->frontsector->heightsec != -1)
				front = &sectors[gr_linedef->frontsector->heightsec];
			else
				front = gr_linedef->frontsector;

			if (gr_linedef->backsector->heightsec != -1)
				back = &sectors[gr_linedef->backsector->heightsec];
			else
				back = gr_linedef->backsector;

			if (gr_sidedef->repeatcnt)
				repeats = 1 + gr_sidedef->repeatcnt;
			else if (gr_linedef->flags & ML_EFFECT5)
			{
				fixed_t high, low;

				if (front->ceilingheight > back->ceilingheight)
					high = back->ceilingheight;
				else
					high = front->ceilingheight;

				if (front->floorheight > back->floorheight)
					low = front->floorheight;
				else
					low = back->floorheight;

				repeats = (high - low)/textureheight[gr_sidedef->midtexture];
			}
			else
				repeats = 1;

			// SoM: a little note: This code re-arranging will
			// fix the bug in Nimrod map02. popentop and popenbottom
			// record the limits the texture can be displayed in.
			// polytop and polybottom, are the ideal (i.e. unclipped)
			// heights of the polygon, and h & l, are the final (clipped)
			// poly coords.

#ifdef POLYOBJECTS
			// NOTE: With polyobjects, whenever you need to check the properties of the polyobject sector it belongs to,
			// you must use the linedef's backsector to be correct
			// From CB
			if (gr_curline->polyseg)
			{
				popentop = back->ceilingheight;
				popenbottom = back->floorheight;
			}
#endif
			else
			{
				popentop = front->ceilingheight < back->ceilingheight ? front->ceilingheight : back->ceilingheight;
				popenbottom = front->floorheight > back->floorheight ? front->floorheight : back->floorheight;
			}

			if (gr_linedef->flags & ML_DONTPEGBOTTOM)
			{
				polybottom = popenbottom + gr_sidedef->rowoffset;
				polytop = polybottom + textureheight[gr_midtexture]*repeats;
			}
			else
			{
				polytop = popentop + gr_sidedef->rowoffset;
				polybottom = polytop - textureheight[gr_midtexture]*repeats;
			}

			// CB
#ifdef POLYOBJECTS
			// NOTE: With polyobjects, whenever you need to check the properties of the polyobject sector it belongs to,
			// you must use the linedef's backsector to be correct
			if (gr_curline->polyseg)
			{
				lowcut = polybottom;
				highcut = polytop;
			}
#endif
			else
			{
				// The cut-off values of a linedef can always be constant, since every line has an absoulute front and or back sector
				lowcut = front->floorheight > back->floorheight ? front->floorheight : back->floorheight;
				highcut = front->ceilingheight < back->ceilingheight ? front->ceilingheight : back->ceilingheight;
			}

			h = polytop > highcut ? highcut : polytop;
			l = polybottom < lowcut ? lowcut : polybottom;

			if (drawtextured)
			{
				// PEGGING
				if (gr_linedef->flags & ML_DONTPEGBOTTOM)
					texturevpeg = textureheight[gr_sidedef->midtexture]*repeats - h + polybottom;
				else
					texturevpeg = polytop - h;

				grTex = HWR_GetTexture(gr_midtexture);

				wallVerts[3].t = wallVerts[2].t = texturevpeg * grTex->scaleY;
				wallVerts[0].t = wallVerts[1].t = (h - l + texturevpeg) * grTex->scaleY;
				wallVerts[0].s = wallVerts[3].s = cliplow * grTex->scaleX;
				wallVerts[2].s = wallVerts[1].s = cliphigh * grTex->scaleX;
			}

			// set top/bottom coords
			// Take the texture peg into account, rather than changing the offsets past
			// where the polygon might not be.
			wallVerts[2].y = wallVerts[3].y = FIXED_TO_FLOAT(h);
			wallVerts[0].y = wallVerts[1].y = FIXED_TO_FLOAT(l);

			// set alpha for transparent walls (new boom and legacy linedef types)
			// ooops ! this do not work at all because render order we should render it in backtofront order
			switch (gr_linedef->special)
			{
				case 900:
					blendmode = HWR_TranstableToAlpha(tr_trans10, &Surf);
					break;
				case 901:
					blendmode = HWR_TranstableToAlpha(tr_trans20, &Surf);
					break;
				case 902:
					blendmode = HWR_TranstableToAlpha(tr_trans30, &Surf);
					break;
				case 903:
					blendmode = HWR_TranstableToAlpha(tr_trans40, &Surf);
					break;
				case 904:
					blendmode = HWR_TranstableToAlpha(tr_trans50, &Surf);
					break;
				case 905:
					blendmode = HWR_TranstableToAlpha(tr_trans60, &Surf);
					break;
				case 906:
					blendmode = HWR_TranstableToAlpha(tr_trans70, &Surf);
					break;
				case 907:
					blendmode = HWR_TranstableToAlpha(tr_trans80, &Surf);
					break;
				case 908:
					blendmode = HWR_TranstableToAlpha(tr_trans90, &Surf);
					break;
				//  Translucent
				case 102:
				case 121:
				case 123:
				case 141:
				case 142:
				case 144:
				case 145:
				case 174:
				case 175:
				case 192:
				case 195:
				case 221:
				case 256:
				case 253:
					blendmode = PF_Translucent;
					break;
				default:
					blendmode = PF_Masked;
					break;
			}
			if (grTex->mipmap.flags & TF_TRANSPARENT)
				blendmode = PF_Environment;

			if (gr_frontsector->numlights)
				HWR_SplitWall(gr_frontsector, wallVerts, gr_midtexture, &Surf, FF_CUTSOLIDS);
			else if (!(blendmode & PF_Masked))
				HWR_AddTransparentWall(wallVerts, &Surf, gr_midtexture, blendmode, false, lightnum, colormap);
			else
				HWR_ProjectWall(wallVerts, &Surf, blendmode, lightnum, colormap);

			// If there is a colormap change, remove it.
/*			if (!(Surf.FlatColor.s.red + Surf.FlatColor.s.green + Surf.FlatColor.s.blue == Surf.FlatColor.s.red/3)
			{
				Surf.FlatColor.s.red = Surf.FlatColor.s.green = Surf.FlatColor.s.blue = 0xff;
				Surf.FlatColor.rgba = 0xffffffff;
			}*/
		}
	}
	else
	{
		// Single sided line... Deal only with the middletexture (if one exists)
		gr_midtexture = texturetranslation[gr_sidedef->midtexture];
		if (gr_midtexture)
		{
			if (drawtextured)
			{
				fixed_t     texturevpeg;
				// PEGGING
				if (gr_linedef->flags & ML_DONTPEGBOTTOM)
					texturevpeg = worldbottom + textureheight[gr_sidedef->midtexture] - worldtop + gr_sidedef->rowoffset;
				else
					// top of texture at top
					texturevpeg = gr_sidedef->rowoffset;

				grTex = HWR_GetTexture(gr_midtexture);

				wallVerts[3].t = wallVerts[2].t = texturevpeg * grTex->scaleY;
				wallVerts[0].t = wallVerts[1].t = (texturevpeg + worldtop - worldbottom) * grTex->scaleY;
				wallVerts[0].s = wallVerts[3].s = cliplow * grTex->scaleX;
				wallVerts[2].s = wallVerts[1].s = cliphigh * grTex->scaleX;
			}
			// set top/bottom coords
			wallVerts[2].y = wallVerts[3].y = FIXED_TO_FLOAT(worldtop);
			wallVerts[0].y = wallVerts[1].y = FIXED_TO_FLOAT(worldbottom);

			// I don't think that solid walls can use translucent linedef types...
			if (gr_frontsector->numlights)
				HWR_SplitWall(gr_frontsector, wallVerts, gr_midtexture, &Surf, FF_CUTSOLIDS);
			else
			{
				if (grTex->mipmap.flags & TF_TRANSPARENT)
					HWR_AddTransparentWall(wallVerts, &Surf, gr_midtexture, PF_Environment, false, lightnum, colormap);
				else
					HWR_ProjectWall(wallVerts, &Surf, PF_Masked, lightnum, colormap);
			}
		}
	}


	//Hurdler: 3d-floors test
#ifdef R_FAKEFLOORS
	if (gr_frontsector && gr_backsector && gr_frontsector->tag != gr_backsector->tag && (gr_backsector->ffloors || gr_frontsector->ffloors))
	{
		ffloor_t * rover;
		ffloor_t * r2;
		fixed_t    highcut = 0, lowcut = 0;

		highcut = gr_frontsector->ceilingheight < gr_backsector->ceilingheight ? gr_frontsector->ceilingheight : gr_backsector->ceilingheight;
		lowcut = gr_frontsector->floorheight > gr_backsector->floorheight ? gr_frontsector->floorheight : gr_backsector->floorheight;

		if (gr_backsector->ffloors)
		{
			for (rover = gr_backsector->ffloors; rover; rover = rover->next)
			{
				if (!(rover->flags & FF_EXISTS) || !(rover->flags & FF_RENDERSIDES) || (rover->flags & FF_INVERTSIDES))
					continue;
				if (*rover->topheight < lowcut || *rover->bottomheight > highcut)
					continue;

				h = *rover->topheight;
				l = *rover->bottomheight;
				if (h > highcut)
					h = highcut;
				if (l < lowcut)
					l = lowcut;
				//Hurdler: HW code starts here
				//FIXME: check if peging is correct
				// set top/bottom coords
				wallVerts[2].y = wallVerts[3].y = FIXED_TO_FLOAT(h);
				wallVerts[0].y = wallVerts[1].y = FIXED_TO_FLOAT(l);

				if (rover->flags & FF_FOG)
				{
					wallVerts[3].t = wallVerts[2].t = 0;
					wallVerts[0].t = wallVerts[1].t = 0;
					wallVerts[0].s = wallVerts[3].s = 0;
					wallVerts[2].s = wallVerts[1].s = 0;
				}
				else if (drawtextured)
				{
					grTex = HWR_GetTexture(texturetranslation[sides[rover->master->sidenum[0]].midtexture]);

					wallVerts[3].t = wallVerts[2].t = (*rover->topheight - h + sides[rover->master->sidenum[0]].rowoffset) * grTex->scaleY;
					wallVerts[0].t = wallVerts[1].t = (h - l + (*rover->topheight - h + sides[rover->master->sidenum[0]].rowoffset)) * grTex->scaleY;
					wallVerts[0].s = wallVerts[3].s = cliplow * grTex->scaleX;
					wallVerts[2].s = wallVerts[1].s = cliphigh * grTex->scaleX;
				}

				if (rover->flags & FF_FOG)
				{
					FBITFIELD blendmode;

					blendmode = PF_Translucent|PF_NoTexture;

					lightnum = rover->master->frontsector->lightlevel;
					colormap = rover->master->frontsector->extra_colormap;

					if (rover->master->frontsector->extra_colormap)
					{

						Surf.FlatColor.s.alpha = HWR_FogBlockAlpha(rover->master->frontsector->lightlevel,rover->master->frontsector->extra_colormap->rgba,rover->master->frontsector->extra_colormap->fadergba);
					}
					else
					{
						Surf.FlatColor.s.alpha = HWR_FogBlockAlpha(rover->master->frontsector->lightlevel,NORMALFOG,FADEFOG);
					}

					if (gr_frontsector->numlights)
						HWR_SplitFog(gr_frontsector, wallVerts, &Surf, rover->flags, lightnum, colormap);
					else
						HWR_AddTransparentWall(wallVerts, &Surf, 0, blendmode, true, lightnum, colormap);
				}
				else
				{
					FBITFIELD blendmode = PF_Masked;

					if (rover->flags & FF_TRANSLUCENT)
					{
						blendmode = PF_Translucent;
						Surf.FlatColor.s.alpha = (UINT8)rover->alpha-1 > 255 ? 255 : rover->alpha-1;
					}
					else if (grTex->mipmap.flags & TF_TRANSPARENT)
					{
						blendmode = PF_Environment;
					}

					if (gr_frontsector->numlights)
						HWR_SplitWall(gr_frontsector, wallVerts, texturetranslation[sides[rover->master->sidenum[0]].midtexture], &Surf, rover->flags);
					else
					{
						if (blendmode != PF_Masked)
							HWR_AddTransparentWall(wallVerts, &Surf, texturetranslation[sides[rover->master->sidenum[0]].midtexture], blendmode, false, lightnum, colormap);
						else
							HWR_ProjectWall(wallVerts, &Surf, PF_Masked, lightnum, colormap);
					}
				}
			}
		}

		if (gr_frontsector->ffloors) // Putting this seperate should allow 2 FOF sectors to be connected without too many errors? I think?
		{
			for (rover = gr_frontsector->ffloors; rover; rover = rover->next)
			{
				if (!(rover->flags & FF_EXISTS) || !(rover->flags & FF_RENDERSIDES) || !(rover->flags & FF_ALLSIDES))
					continue;
				if (*rover->topheight < lowcut || *rover->bottomheight > highcut)
					continue;

				h = *rover->topheight;
				l = *rover->bottomheight;
				if (h > highcut)
					h = highcut;
				if (l < lowcut)
					l = lowcut;
				//Hurdler: HW code starts here
				//FIXME: check if peging is correct
				// set top/bottom coords
				wallVerts[2].y = wallVerts[3].y = FIXED_TO_FLOAT(h);
				wallVerts[0].y = wallVerts[1].y = FIXED_TO_FLOAT(l);

				if (rover->flags & FF_FOG)
				{
					wallVerts[3].t = wallVerts[2].t = 0;
					wallVerts[0].t = wallVerts[1].t = 0;
					wallVerts[0].s = wallVerts[3].s = 0;
					wallVerts[2].s = wallVerts[1].s = 0;
				}
				else if (drawtextured)
				{
					grTex = HWR_GetTexture(texturetranslation[sides[rover->master->sidenum[0]].midtexture]);

					wallVerts[3].t = wallVerts[2].t = (*rover->topheight - h + sides[rover->master->sidenum[0]].rowoffset) * grTex->scaleY;
					wallVerts[0].t = wallVerts[1].t = (h - l + (*rover->topheight - h + sides[rover->master->sidenum[0]].rowoffset)) * grTex->scaleY;
					wallVerts[0].s = wallVerts[3].s = cliplow * grTex->scaleX;
					wallVerts[2].s = wallVerts[1].s = cliphigh * grTex->scaleX;
				}

				if (rover->flags & FF_FOG)
				{
					FBITFIELD blendmode;

					blendmode = PF_Translucent|PF_NoTexture;

					lightnum = rover->master->frontsector->lightlevel;
					colormap = rover->master->frontsector->extra_colormap;

					if (rover->master->frontsector->extra_colormap)
					{
						Surf.FlatColor.s.alpha = HWR_FogBlockAlpha(rover->master->frontsector->lightlevel,rover->master->frontsector->extra_colormap->rgba,rover->master->frontsector->extra_colormap->fadergba);
					}
					else
					{
						Surf.FlatColor.s.alpha = HWR_FogBlockAlpha(rover->master->frontsector->lightlevel,NORMALFOG,FADEFOG);
					}

					if (gr_backsector->numlights)
						HWR_SplitFog(gr_backsector, wallVerts, &Surf, rover->flags, lightnum, colormap);
					else
						HWR_AddTransparentWall(wallVerts, &Surf, 0, blendmode, true, lightnum, colormap);
				}
				else
				{
					FBITFIELD blendmode = PF_Masked;

					if (rover->flags & FF_TRANSLUCENT)
					{
						blendmode = PF_Translucent;
						Surf.FlatColor.s.alpha = (UINT8)rover->alpha-1 > 255 ? 255 : rover->alpha-1;
					}
					else if (grTex->mipmap.flags & TF_TRANSPARENT)
					{
						blendmode = PF_Environment;
					}

					if (gr_backsector->numlights)
						HWR_SplitWall(gr_backsector, wallVerts, texturetranslation[sides[rover->master->sidenum[0]].midtexture], &Surf, rover->flags);
					else
					{
						if (blendmode != PF_Masked)
							HWR_AddTransparentWall(wallVerts, &Surf, texturetranslation[sides[rover->master->sidenum[0]].midtexture], blendmode, false, lightnum, colormap);
						else
							HWR_ProjectWall(wallVerts, &Surf, PF_Masked, lightnum, colormap);
					}
				}
			}
		}
	}
#endif
//Hurdler: end of 3d-floors test
}

//Hurdler: just like in r_bsp.c
#if 1
#define MAXSEGS         MAXVIDWIDTH/2+1
#else
//Alam_GBC: Or not (may cause overflow)
#define MAXSEGS         128
#endif

// hw_newend is one past the last valid seg
static cliprange_t *   hw_newend;
static cliprange_t     gr_solidsegs[MAXSEGS];


static void printsolidsegs(void)
{
	cliprange_t *       start;
	if (!hw_newend || cv_grbeta.value != 2)
		return;
	for (start = gr_solidsegs;start != hw_newend;start++)
		CONS_Printf("%d-%d|",start->first,start->last);
	CONS_Printf("\n\n");
}

//
//
//
static void HWR_ClipSolidWallSegment(INT32 first, INT32 last)
{
	cliprange_t *next, *start;
	float lowfrac, highfrac;
	boolean poorhack = false;

	// Find the first range that touches the range
	//  (adjacent pixels are touching).
	start = gr_solidsegs;
	while (start->last < first-1)
		start++;

	if (first < start->first)
	{
		if (last < start->first-1)
		{
			// Post is entirely visible (above start),
			//  so insert a new clippost.
			HWR_StoreWallRange(first, last);

			next = hw_newend;
			hw_newend++;

			while (next != start)
			{
				*next = *(next-1);
				next--;
			}

			next->first = first;
			next->last = last;
			printsolidsegs();
			return;
		}

		// There is a fragment above *start.
		if (!cv_grclipwalls.value)
		{
			if (!poorhack) HWR_StoreWallRange(first, last);
			poorhack = true;
		}
		else
		{
			highfrac = HWR_ClipViewSegment(start->first+1, (polyvertex_t *)gr_curline->v1, (polyvertex_t *)gr_curline->v2);
			HWR_StoreWallRange(0, highfrac);
		}
		// Now adjust the clip size.
		start->first = first;
	}

	// Bottom contained in start?
	if (last <= start->last)
	{
		printsolidsegs();
		return;
	}
	next = start;
	while (last >= (next+1)->first-1)
	{
		// There is a fragment between two posts.
		if (!cv_grclipwalls.value)
		{
			if (!poorhack) HWR_StoreWallRange(first,last);
			poorhack = true;
		}
		else
		{
			lowfrac  = HWR_ClipViewSegment(next->last-1, (polyvertex_t *)gr_curline->v1, (polyvertex_t *)gr_curline->v2);
			highfrac = HWR_ClipViewSegment((next+1)->first+1, (polyvertex_t *)gr_curline->v1, (polyvertex_t *)gr_curline->v2);
			HWR_StoreWallRange(lowfrac, highfrac);
		}
		next++;

		if (last <= next->last)
		{
			// Bottom is contained in next.
			// Adjust the clip size.
			start->last = next->last;
			goto crunch;
		}
	}

	if (first == next->first+1) // 1 line texture
	{
		if (!cv_grclipwalls.value)
		{
			if (!poorhack) HWR_StoreWallRange(first,last);
			poorhack = true;
		}
		else
			HWR_StoreWallRange(0, 1);
	}
	else
	{
	// There is a fragment after *next.
		if (!cv_grclipwalls.value)
		{
			if (!poorhack) HWR_StoreWallRange(first,last);
			poorhack = true;
		}
		else
		{
			lowfrac  = HWR_ClipViewSegment(next->last-1, (polyvertex_t *)gr_curline->v1, (polyvertex_t *)gr_curline->v2);
			HWR_StoreWallRange(lowfrac, 1);
		}
	}

	// Adjust the clip size.
	start->last = last;

	// Remove start+1 to next from the clip list,
	// because start now covers their area.
crunch:
	if (next == start)
	{
		printsolidsegs();
		// Post just extended past the bottom of one post.
		return;
	}


	while (next++ != hw_newend)
	{
		// Remove a post.
		*++start = *next;
	}

	hw_newend = start;
	printsolidsegs();
}

//
//  handle LineDefs with upper and lower texture (windows)
//
static void HWR_ClipPassWallSegment(INT32 first, INT32 last)
{
	cliprange_t *start;
	float lowfrac, highfrac;
	//to allow noclipwalls but still solidseg reject of non-visible walls
	boolean poorhack = false;

	// Find the first range that touches the range
	//  (adjacent pixels are touching).
	start = gr_solidsegs;
	while (start->last < first - 1)
		start++;

	if (first < start->first)
	{
		if (last < start->first-1)
		{
			// Post is entirely visible (above start).
			HWR_StoreWallRange(0, 1);
			return;
		}

		// There is a fragment above *start.
		if (!cv_grclipwalls.value)
		{	//20/08/99: Changed by Hurdler (taken from faB's code)
			if (!poorhack) HWR_StoreWallRange(0, 1);
			poorhack = true;
		}
		else
		{
			highfrac = HWR_ClipViewSegment(min(start->first + 1,
				start->last), (polyvertex_t *)gr_curline->v1,
				(polyvertex_t *)gr_curline->v2);
			HWR_StoreWallRange(0, highfrac);
		}
	}

	// Bottom contained in start?
	if (last <= start->last)
		return;

	while (last >= (start+1)->first-1)
	{
		// There is a fragment between two posts.
		if (!cv_grclipwalls.value)
		{
			if (!poorhack) HWR_StoreWallRange(0, 1);
			poorhack = true;
		}
		else
		{
			lowfrac  = HWR_ClipViewSegment(max(start->last-1,start->first), (polyvertex_t *)gr_curline->v1, (polyvertex_t *)gr_curline->v2);
			highfrac = HWR_ClipViewSegment(min((start+1)->first+1,(start+1)->last), (polyvertex_t *)gr_curline->v1, (polyvertex_t *)gr_curline->v2);
			HWR_StoreWallRange(lowfrac, highfrac);
		}
		start++;

		if (last <= start->last)
			return;
	}

	if (first == start->first+1) // 1 line texture
	{
		if (!cv_grclipwalls.value)
		{
			if (!poorhack) HWR_StoreWallRange(0, 1);
			poorhack = true;
		}
		else
			HWR_StoreWallRange(0, 1);
	}
	else
	{
		// There is a fragment after *next.
		if (!cv_grclipwalls.value)
		{
			if (!poorhack) HWR_StoreWallRange(0,1);
			poorhack = true;
		}
		else
		{
			lowfrac = HWR_ClipViewSegment(max(start->last - 1,
				start->first), (polyvertex_t *)gr_curline->v1,
				(polyvertex_t *)gr_curline->v2);
			HWR_StoreWallRange(lowfrac, 1);
		}
	}
}

// --------------------------------------------------------------------------
//  HWR_ClipToSolidSegs check if it is hide by wall (solidsegs)
// --------------------------------------------------------------------------
static boolean HWR_ClipToSolidSegs(INT32 first, INT32 last)
{
	cliprange_t * start;

	// Find the first range that touches the range
	//  (adjacent pixels are touching).
	start = gr_solidsegs;
	while (start->last < first-1)
		start++;

	if (first < start->first)
		return true;

	// Bottom contained in start?
	if (last <= start->last)
		return false;

	return true;
}

//
// HWR_ClearClipSegs
//
static void HWR_ClearClipSegs(void)
{
	gr_solidsegs[0].first = -0x7fffffff;
	gr_solidsegs[0].last = -1;
	gr_solidsegs[1].first = vid.width; //viewwidth;
	gr_solidsegs[1].last = 0x7fffffff;
	hw_newend = gr_solidsegs+2;
}

// -----------------+
// HWR_AddLine      : Clips the given segment and adds any visible pieces to the line list.
// Notes            : gr_cursectorlight is set to the current subsector -> sector -> light value
//                  : (it may be mixed with the wall's own flat colour in the future ...)
// -----------------+
static void HWR_AddLine(seg_t * line)
{
	INT32 x1, x2;
	angle_t angle1, angle2;
	angle_t span, tspan;

	// SoM: Backsector needs to be run through R_FakeFlat
	sector_t tempsec;

	if (line->polyseg && !(line->polyseg->flags & POF_RENDERSIDES))
		return;

	gr_curline = line;

	// OPTIMIZE: quickly reject orthogonal back sides.
	angle1 = R_PointToAngle(FLOAT_TO_FIXED(((polyvertex_t *)gr_curline->v1)->x),
	                        FLOAT_TO_FIXED(((polyvertex_t *)gr_curline->v1)->y));
	angle2 = R_PointToAngle(FLOAT_TO_FIXED(((polyvertex_t *)gr_curline->v2)->x),
	                        FLOAT_TO_FIXED(((polyvertex_t *)gr_curline->v2)->y));

	// Clip to view edges.
	span = angle1 - angle2;

	// backface culling : span is < ANGLE_180 if ang1 > ang2 : the seg is facing
	if (span >= ANGLE_180)
		return;

	// Global angle needed by segcalc.
	//rw_angle1 = angle1;
	angle1 -= dup_viewangle;
	angle2 -= dup_viewangle;

	tspan = angle1 + gr_clipangle;
	if (tspan > 2*gr_clipangle)
	{
		tspan -= 2*gr_clipangle;

		// Totally off the left edge?
		if (tspan >= span)
			return;

		angle1 = gr_clipangle;
	}
	tspan = gr_clipangle - angle2;
	if (tspan > 2*gr_clipangle)
	{
		tspan -= 2*gr_clipangle;

		// Totally off the left edge?
		if (tspan >= span)
			return;

		angle2 = (angle_t)-(signed)gr_clipangle;
	}

#if 0
	{
		float fx1,fx2,fy1,fy2;
		//BP: test with a better projection than viewangletox[R_PointToAngle(angle)]
		// do not enable this at release 4 mul and 2 div
		fx1 = ((polyvertex_t *)(line->v1))->x-gr_viewx;
		fy1 = ((polyvertex_t *)(line->v1))->y-gr_viewy;
		fy2 = (fx1 * gr_viewcos + fy1 * gr_viewsin);
		if (fy2 < 0)
			// the point is back
			fx1 = 0;
		else
			fx1 = gr_windowcenterx + (fx1 * gr_viewsin - fy1 * gr_viewcos) * gr_centerx / fy2;

		fx2 = ((polyvertex_t *)(line->v2))->x-gr_viewx;
		fy2 = ((polyvertex_t *)(line->v2))->y-gr_viewy;
		fy1 = (fx2 * gr_viewcos + fy2 * gr_viewsin);
		if (fy1 < 0)
			// the point is back
			fx2 = vid.width;
		else
			fx2 = gr_windowcenterx + (fx2 * gr_viewsin - fy2 * gr_viewcos) * gr_centerx / fy1;

		x1 = fx1+0.5f;
		x2 = fx2+0.5f;
	}
#else
	// The seg is in the view range,
	// but not necessarily visible.
	angle1 = (angle1+ANGLE_90)>>ANGLETOFINESHIFT;
	angle2 = (angle2+ANGLE_90)>>ANGLETOFINESHIFT;

	x1 = gr_viewangletox[angle1];
	x2 = gr_viewangletox[angle2];
#endif
	// Does not cross a pixel?
//    if (x1 == x2)
/*    {
		// BP: HERE IS THE MAIN PROBLEM !
		//CONS_Printf("tineline\n");
		return;
	}
*/
	gr_backsector = line->backsector;

	// Single sided line?
	if (!gr_backsector)
		goto clipsolid;

	gr_backsector = R_FakeFlat(gr_backsector, &tempsec, NULL, NULL, true);

	// Closed door.
	if (gr_backsector->ceilingheight <= gr_frontsector->floorheight ||
	    gr_backsector->floorheight >= gr_frontsector->ceilingheight)
		goto clipsolid;

	// Window.
	if (gr_backsector->ceilingheight != gr_frontsector->ceilingheight ||
	    gr_backsector->floorheight != gr_frontsector->floorheight)
		goto clippass;

	// Reject empty lines used for triggers and special events.
	// Identical floor and ceiling on both sides,
	//  identical light levels on both sides,
	//  and no middle texture.
	if (
#ifdef POLYOBJECTS
	    !line->polyseg
#endif
	    && gr_backsector->ceilingpic == gr_frontsector->ceilingpic
	    && gr_backsector->floorpic == gr_frontsector->floorpic
	    && gr_backsector->lightlevel == gr_frontsector->lightlevel
	    && gr_curline->sidedef->midtexture == 0
	    && !gr_backsector->ffloors && !gr_frontsector->ffloors)
		// SoM: For 3D sides... Boris, would you like to take a
		// crack at rendering 3D sides? You would need to add the
		// above check and add code to HWR_StoreWallRange...
	{
		return;
	}

clippass:
	if (x1 == x2)
		{  x2++;x1 -= 2; }
	HWR_ClipPassWallSegment(x1, x2-1);
	return;

clipsolid:
	if (x1 == x2)
		goto clippass;
	HWR_ClipSolidWallSegment(x1, x2-1);
}

// HWR_CheckBBox
// Checks BSP node/subtree bounding box.
// Returns true
//  if some part of the bbox might be visible.
//
// modified to use local variables

static boolean HWR_CheckBBox(fixed_t *bspcoord)
{
	INT32 boxpos, sx1, sx2;
	fixed_t px1, py1, px2, py2;
	angle_t angle1, angle2, span, tspan;

	// Find the corners of the box
	// that define the edges from current viewpoint.
	if (dup_viewx <= bspcoord[BOXLEFT])
		boxpos = 0;
	else if (dup_viewx < bspcoord[BOXRIGHT])
		boxpos = 1;
	else
		boxpos = 2;

	if (dup_viewy >= bspcoord[BOXTOP])
		boxpos |= 0;
	else if (dup_viewy > bspcoord[BOXBOTTOM])
		boxpos |= 1<<2;
	else
		boxpos |= 2<<2;

	if (boxpos == 5)
		return true;

	px1 = bspcoord[checkcoord[boxpos][0]];
	py1 = bspcoord[checkcoord[boxpos][1]];
	px2 = bspcoord[checkcoord[boxpos][2]];
	py2 = bspcoord[checkcoord[boxpos][3]];

	// check clip list for an open space
	angle1 = R_PointToAngle(px1, py1) - dup_viewangle;
	angle2 = R_PointToAngle(px2, py2) - dup_viewangle;

	span = angle1 - angle2;

	// Sitting on a line?
	if (span >= ANGLE_180)
		return true;

	tspan = angle1 + gr_clipangle;

	if (tspan > 2*gr_clipangle)
	{
		tspan -= 2*gr_clipangle;

		// Totally off the left edge?
		if (tspan >= span)
			return false;

		angle1 = gr_clipangle;
	}
	tspan = gr_clipangle - angle2;
	if (tspan > 2*gr_clipangle)
	{
		tspan -= 2*gr_clipangle;

		// Totally off the left edge?
		if (tspan >= span)
			return false;

		angle2 = (angle_t)-(signed)gr_clipangle;
	}

	// Find the first clippost
	//  that touches the source post
	//  (adjacent pixels are touching).
	angle1 = (angle1+ANGLE_90)>>ANGLETOFINESHIFT;
	angle2 = (angle2+ANGLE_90)>>ANGLETOFINESHIFT;
	sx1 = gr_viewangletox[angle1];
	sx2 = gr_viewangletox[angle2];

	// Does not cross a pixel.
	if (sx1 == sx2)
		return false;

	return HWR_ClipToSolidSegs(sx1, sx2 - 1);
}

#ifdef POLYOBJECTS

//
// HWR_AddPolyObjectSegs
//
// haleyjd 02/19/06
// Adds all segs in all polyobjects in the given subsector.
// Modified for SRB2 hardware rendering by Jazz 7/13/09
//
static inline void HWR_AddPolyObjectSegs(void)
{
	size_t i, j;
	seg_t *gr_fakeline = Z_Calloc(sizeof(seg_t), PU_STATIC, NULL);
	polyvertex_t *pv1 = Z_Calloc(sizeof(polyvertex_t), PU_STATIC, NULL);
	polyvertex_t *pv2 = Z_Calloc(sizeof(polyvertex_t), PU_STATIC, NULL);

	// Sort through all the polyobjects
	for (i = 0; i < numpolys; ++i)
	{
		// Render the polyobject's lines
		for (j = 0; j < po_ptrs[i]->segCount; ++j)
		{
			// Copy the info of a polyobject's seg, then convert it to OpenGL floating point
			M_Memcpy(gr_fakeline, po_ptrs[i]->segs[j], sizeof(seg_t));

			// Now convert the line to float and add it to be rendered
			pv1->x = FIXED_TO_FLOAT(gr_fakeline->v1->x);
			pv1->y = FIXED_TO_FLOAT(gr_fakeline->v1->y);
			pv2->x = FIXED_TO_FLOAT(gr_fakeline->v2->x);
			pv2->y = FIXED_TO_FLOAT(gr_fakeline->v2->y);

			gr_fakeline->v1 = (vertex_t *)pv1;
			gr_fakeline->v2 = (vertex_t *)pv2;

			HWR_AddLine(gr_fakeline);
		}
	}

	// Free temporary data no longer needed
	Z_Free(pv2);
	Z_Free(pv1);
	Z_Free(gr_fakeline);
}
#endif

// -----------------+
// HWR_Subsector    : Determine floor/ceiling planes.
//                  : Add sprites of things in sector.
//                  : Draw one or more line segments.
// Notes            : Sets gr_cursectorlight to the light of the parent sector, to modulate wall textures
// -----------------+
static lumpnum_t doomwaterflat;  //set by R_InitFlats hack
static void HWR_Subsector(size_t num)
{
	INT16 count;
	seg_t *line;
	subsector_t *sub;
	sector_t tempsec; //SoM: 4/7/2000
	INT32 floorlightlevel;
	INT32 ceilinglightlevel;
	INT32 locFloorHeight, locCeilingHeight;
	INT32 light = 0;
	fixed_t wh;
	extracolormap_t *floorcolormap;
	extracolormap_t *ceilingcolormap;

#ifdef PARANOIA //no risk while developing, enough debugging nights!
	if (num >= addsubsector)
		I_Error("HWR_Subsector: ss %"PRIdS" with numss = %"PRIdS", addss = %"PRIdS"\n",
			num, numsubsectors, addsubsector);

	/*if (num >= numsubsectors)
		I_Error("HWR_Subsector: ss %i with numss = %i",
		        num,
		        numsubsectors);*/
#endif

	if (num < numsubsectors)
	{
		sscount++;
		// subsector
		sub = &subsectors[num];
		// sector
		gr_frontsector = sub->sector;
		// how many linedefs
		count = sub->numlines;
		// first line seg
		line = &segs[sub->firstline];
	}
	else
	{
		// there are no segs but only planes
		sub = &subsectors[0];
		gr_frontsector = sub->sector;
		count = 0;
		line = NULL;
	}

	//SoM: 4/7/2000: Test to make Boom water work in Hardware mode.
	gr_frontsector = R_FakeFlat(gr_frontsector, &tempsec, &floorlightlevel,
								&ceilinglightlevel, false);
	//FIXME: Use floorlightlevel and ceilinglightlevel insted of lightlevel.

	floorcolormap = ceilingcolormap = gr_frontsector->extra_colormap;

	// ------------------------------------------------------------------------
	// sector lighting, DISABLED because it's done in HWR_StoreWallRange
	// ------------------------------------------------------------------------
	/// \todo store a RGBA instead of just intensity, allow coloured sector lighting
	//light = (FUBYTE)(sub->sector->lightlevel & 0xFF) / 255.0f;
	//gr_cursectorlight.red   = light;
	//gr_cursectorlight.green = light;
	//gr_cursectorlight.blue  = light;
	//gr_cursectorlight.alpha = light;

// ----- for special tricks with HW renderer -----
	if (gr_frontsector->pseudoSector)
	{
		locFloorHeight = gr_frontsector->virtualFloorheight;
		locCeilingHeight = gr_frontsector->virtualCeilingheight;
	}
	else if (gr_frontsector->virtualFloor)
	{
		locFloorHeight = gr_frontsector->virtualFloorheight;
		if (gr_frontsector->virtualCeiling)
			locCeilingHeight = gr_frontsector->virtualCeilingheight;
		else
			locCeilingHeight = gr_frontsector->ceilingheight;
	}
	else if (gr_frontsector->virtualCeiling)
	{
		locCeilingHeight = gr_frontsector->virtualCeilingheight;
		locFloorHeight   = gr_frontsector->floorheight;
	}
	else
	{
		locFloorHeight   = gr_frontsector->floorheight;
		locCeilingHeight = gr_frontsector->ceilingheight;
	}
// ----- end special tricks -----

	if (gr_frontsector->ffloors)
	{
		if (gr_frontsector->moved)
		{
			gr_frontsector->numlights = sub->sector->numlights = 0;
			R_Prep3DFloors(gr_frontsector);
			sub->sector->lightlist = gr_frontsector->lightlist;
			sub->sector->numlights = gr_frontsector->numlights;
			sub->sector->moved = gr_frontsector->moved = false;
		}

		light = R_GetPlaneLight(gr_frontsector, locFloorHeight, false);
		if (gr_frontsector->floorlightsec == -1)
			floorlightlevel = *gr_frontsector->lightlist[light].lightlevel;
		floorcolormap = gr_frontsector->lightlist[light].extra_colormap;

		light = R_GetPlaneLight(gr_frontsector, locCeilingHeight, false);
		if (gr_frontsector->ceilinglightsec == -1)
			ceilinglightlevel = *gr_frontsector->lightlist[light].lightlevel;
		ceilingcolormap = gr_frontsector->lightlist[light].extra_colormap;
	}

	sub->sector->extra_colormap = gr_frontsector->extra_colormap;

	// render floor ?
#ifdef DOPLANES
	// yeah, easy backface cull! :)
	if (locFloorHeight < dup_viewz)
	{
		if (gr_frontsector->floorpic != skyflatnum)
		{
			if (sub->validcount != validcount)
			{
				HWR_GetFlat(levelflats[gr_frontsector->floorpic].lumpnum);
				HWR_RenderPlane(gr_frontsector, &extrasubsectors[num], locFloorHeight, PF_Occlude, floorlightlevel, levelflats[gr_frontsector->floorpic].lumpnum, NULL, 255, false, floorcolormap);
			}
		}
		else
		{
#ifdef POLYSKY
			HWR_RenderSkyPlane(&extrasubsectors[num], locFloorHeight);
#endif
		}
	}

	if (locCeilingHeight > dup_viewz)
	{
		if (gr_frontsector->ceilingpic != skyflatnum)
		{
			if (sub->validcount != validcount)
			{
				HWR_GetFlat(levelflats[gr_frontsector->ceilingpic].lumpnum);
				HWR_RenderPlane(NULL, &extrasubsectors[num], locCeilingHeight, PF_Occlude, ceilinglightlevel, levelflats[gr_frontsector->ceilingpic].lumpnum,NULL, 255, false, ceilingcolormap);
			}
		}
		else
		{
#ifdef POLYSKY
			HWR_RenderSkyPlane(&extrasubsectors[num], locCeilingHeight);
#endif
		}
	}

#ifndef POLYSKY
	// Moved here because before, when above the ceiling and the floor does not have the sky flat, it doesn't draw the sky
	if (gr_frontsector->ceilingpic == skyflatnum || gr_frontsector->floorpic == skyflatnum)
	{
		drawsky = true;
	}
#endif

#ifdef R_FAKEFLOORS
	if (gr_frontsector->ffloors)
	{
		/// \todo fix light, xoffs, yoffs, extracolormap ?
		ffloor_t * rover;

		R_Prep3DFloors(gr_frontsector);
		for (rover = gr_frontsector->ffloors;
			rover; rover = rover->next)
		{

			if (!(rover->flags & FF_EXISTS) || !(rover->flags & FF_RENDERPLANES))
				continue;
			if (sub->validcount == validcount)
				continue;

			if (*rover->bottomheight <= gr_frontsector->ceilingheight &&
			    *rover->bottomheight >= gr_frontsector->floorheight &&
			    ((dup_viewz < *rover->bottomheight && !(rover->flags & FF_INVERTPLANES)) ||
			     (dup_viewz > *rover->bottomheight && (rover->flags & FF_BOTHPLANES || rover->flags & FF_INVERTPLANES))))
			{
				if (rover->flags & FF_FOG)
				{
					UINT8 alpha;

					light = R_GetPlaneLight(gr_frontsector, *rover->bottomheight, dup_viewz < *rover->bottomheight ? true : false);

					if (rover->master->frontsector->extra_colormap)
						alpha = HWR_FogBlockAlpha(*gr_frontsector->lightlist[light].lightlevel, rover->master->frontsector->extra_colormap->rgba, rover->master->frontsector->extra_colormap->fadergba);
					else
						alpha = HWR_FogBlockAlpha(*gr_frontsector->lightlist[light].lightlevel, NORMALFOG, FADEFOG);

					HWR_AddTransparentFloor(0,
					                       &extrasubsectors[num],
					                       *rover->bottomheight,
					                       *gr_frontsector->lightlist[light].lightlevel,
					                       alpha, rover->master->frontsector, PF_Translucent|PF_NoTexture,
										   true, rover->master->frontsector->extra_colormap);
				}
				else if (rover->flags & FF_TRANSLUCENT) // SoM: Flags are more efficient
				{
					light = R_GetPlaneLight(gr_frontsector, *rover->bottomheight, dup_viewz < *rover->bottomheight ? true : false);
#ifndef SORTING
					HWR_Add3DWater(levelflats[*rover->bottompic].lumpnum,
					               &extrasubsectors[num],
					               *rover->bottomheight,
					               *gr_frontsector->lightlist[light].lightlevel,
					               rover->alpha, rover->master->frontsector);
#else
					HWR_AddTransparentFloor(levelflats[*rover->bottompic].lumpnum,
					                       &extrasubsectors[num],
					                       *rover->bottomheight,
					                       *gr_frontsector->lightlist[light].lightlevel,
					                       rover->alpha-1 > 255 ? 255 : rover->alpha-1, rover->master->frontsector, PF_Translucent,
										   false, gr_frontsector->lightlist[light].extra_colormap);
#endif
				}
				else
				{
					HWR_GetFlat(levelflats[*rover->bottompic].lumpnum);
					light = R_GetPlaneLight(gr_frontsector, *rover->bottomheight, dup_viewz < *rover->bottomheight ? true : false);
					HWR_RenderPlane(NULL, &extrasubsectors[num], *rover->bottomheight, PF_Occlude, *gr_frontsector->lightlist[light].lightlevel, levelflats[*rover->bottompic].lumpnum,
					                rover->master->frontsector, 255, false, gr_frontsector->lightlist[light].extra_colormap);
				}
			}
			if (*rover->topheight >= gr_frontsector->floorheight &&
			    *rover->topheight <= gr_frontsector->ceilingheight &&
			    ((dup_viewz > *rover->topheight && !(rover->flags & FF_INVERTPLANES)) ||
			     (dup_viewz < *rover->topheight && (rover->flags & FF_BOTHPLANES || rover->flags & FF_INVERTPLANES))))
			{
				if (rover->flags & FF_FOG)
				{
					UINT8 alpha;

					light = R_GetPlaneLight(gr_frontsector, *rover->topheight, dup_viewz < *rover->topheight ? true : false);

					if (rover->master->frontsector->extra_colormap)
						alpha = HWR_FogBlockAlpha(*gr_frontsector->lightlist[light].lightlevel, rover->master->frontsector->extra_colormap->rgba, rover->master->frontsector->extra_colormap->fadergba);
					else
						alpha = HWR_FogBlockAlpha(*gr_frontsector->lightlist[light].lightlevel, NORMALFOG, FADEFOG);

					HWR_AddTransparentFloor(0,
					                       &extrasubsectors[num],
					                       *rover->topheight,
					                       *gr_frontsector->lightlist[light].lightlevel,
					                       alpha, rover->master->frontsector, PF_Translucent|PF_NoTexture,
										   true, rover->master->frontsector->extra_colormap);
				}
				else if (rover->flags & FF_TRANSLUCENT)
				{
					light = R_GetPlaneLight(gr_frontsector, *rover->topheight, dup_viewz < *rover->topheight ? true : false);
#ifndef SORTING
					HWR_Add3DWater(levelflats[*rover->toppic].lumpnum,
					                          &extrasubsectors[num],
					                          *rover->topheight,
					                          *gr_frontsector->lightlist[light].lightlevel,
					                          rover->alpha, rover->master->frontsector);
#else
					HWR_AddTransparentFloor(levelflats[*rover->toppic].lumpnum,
					                        &extrasubsectors[num],
					                        *rover->topheight,
					                        *gr_frontsector->lightlist[light].lightlevel,
					                        rover->alpha-1 > 255 ? 255 : rover->alpha-1, rover->master->frontsector, PF_Translucent,
											false, gr_frontsector->lightlist[light].extra_colormap);
#endif

				}
				else
				{
					HWR_GetFlat(levelflats[*rover->toppic].lumpnum);
					light = R_GetPlaneLight(gr_frontsector, *rover->topheight, dup_viewz < *rover->topheight ? true : false);
					HWR_RenderPlane(NULL, &extrasubsectors[num], *rover->topheight, PF_Occlude, *gr_frontsector->lightlist[light].lightlevel, levelflats[*rover->toppic].lumpnum,
					                  rover->master->frontsector, 255, false, gr_frontsector->lightlist[light].extra_colormap);
				}
			}
		}
	}
#endif
#endif //doplanes

#ifdef POLYOBJECTS
	// Draw all the polyobjects in this subsector
	if (sub->polyList)
	{
		polyobj_t *po = sub->polyList;

		numpolys = 0;

		// Count all the polyobjects, reset the list, and recount them
		while (po)
		{
			++numpolys;
			po = (polyobj_t *)(po->link.next);
		}

		// Sort polyobjects
		R_SortPolyObjects(sub);

		// Draw polyobject lines.
		HWR_AddPolyObjectSegs();

		// Draw polyobject planes
		//HWR_AddPolyObjectPlanes();
	}
#endif

// Hurder ici se passe les choses INT32�essantes!
// on vient de tracer le sol et le plafond
// on trace �pr�ent d'abord les sprites et ensuite les murs
// hurdler: faux: on ajoute seulement les sprites, le murs sont trac� d'abord
	if (line)
	{
		// draw sprites first, coz they are clipped to the solidsegs of
		// subsectors more 'in front'
		HWR_AddSprites(gr_frontsector);

		//Hurdler: at this point validcount must be the same, but is not because
		//         gr_frontsector doesn't point anymore to sub->sector due to
		//         the call gr_frontsector = R_FakeFlat(...)
		//         if it's not done, the sprite is drawn more than once,
		//         what looks really bad with translucency or dynamic light,
		//         without talking about the overdraw of course.
		sub->sector->validcount = validcount;/// \todo fix that in a better way

		while (count--)
		{
				HWR_AddLine(line);
				line++;
		}
	}

//20/08/99: Changed by Hurdler (taken from faB's code)
#ifdef DOPLANES
	// -------------------- WATER IN DEV. TEST ------------------------
	//dck hack : use abs(tag) for waterheight
	if (gr_frontsector->tag < 0)
	{
		wh = ((-gr_frontsector->tag) <<FRACBITS) + (FRACUNIT/2);
		if (wh > gr_frontsector->floorheight &&
			wh < gr_frontsector->ceilingheight)
		{
			HWR_GetFlat(doomwaterflat);
			HWR_RenderPlane(gr_frontsector,
				&extrasubsectors[num], wh, PF_Translucent,
				gr_frontsector->lightlevel, doomwaterflat,
				NULL, 255, false, gr_frontsector->lightlist[light].extra_colormap);
		}
	}
	// -------------------- WATER IN DEV. TEST ------------------------
#endif
	sub->validcount = validcount;
}

//
// Renders all subsectors below a given node,
//  traversing subtree recursively.
// Just call with BSP root.

#ifdef coolhack
//t;b;l;r
static fixed_t hackbbox[4];
//BOXTOP,
//BOXBOTTOM,
//BOXLEFT,
//BOXRIGHT
static boolean HWR_CheckHackBBox(fixed_t *bb)
{
	if (bb[BOXTOP] < hackbbox[BOXBOTTOM]) //y up
		return false;
	if (bb[BOXBOTTOM] > hackbbox[BOXTOP])
		return false;
	if (bb[BOXLEFT] > hackbbox[BOXRIGHT])
		return false;
	if (bb[BOXRIGHT] < hackbbox[BOXLEFT])
		return false;
	return true;
}
#endif

// BP: big hack for a test in lighning ref : 1249753487AB
fixed_t *hwbbox;

static void HWR_RenderBSPNode(INT32 bspnum)
{
	/*//GZDoom code
	if(bspnum == -1)
	{
		HWR_Subsector(subsectors);
		return;
	}
	while(!((size_t)bspnum&(~NF_SUBSECTOR))) // Keep going until found a subsector
	{
		node_t *bsp = &nodes[bspnum];

		// Decide which side the view point is on
		INT32 side = R_PointOnSide(dup_viewx, dup_viewy, bsp);

		// Recursively divide front space (toward the viewer)
		HWR_RenderBSPNode(bsp->children[side]);

		// Possibly divide back space (away from viewer)
		side ^= 1;

		if (!HWR_CheckBBox(bsp->bbox[side]))
			return;

		bspnum = bsp->children[side];
	}

	HWR_Subsector(bspnum-1);
*/
	node_t *bsp = &nodes[bspnum];

	// Decide which side the view point is on
	INT32 side;

	// Found a subsector?
	if (bspnum & NF_SUBSECTOR)
	{
		if (bspnum == -1)
		{
			//*(gr_drawsubsector_p++) = 0;
			HWR_Subsector(0);
		}
		else
		{
			//*(gr_drawsubsector_p++) = bspnum&(~NF_SUBSECTOR);
			HWR_Subsector(bspnum&(~NF_SUBSECTOR));
		}
		return;
	}

	// Decide which side the view point is on.
	side = R_PointOnSide(dup_viewx, dup_viewy, bsp);

	// BP: big hack for a test in lighning ref : 1249753487AB
	hwbbox = bsp->bbox[side];

	// Recursively divide front space.
	HWR_RenderBSPNode(bsp->children[side]);

	// Possibly divide back space.
	if (HWR_CheckBBox(bsp->bbox[side^1]))
	{
		// BP: big hack for a test in lighning ref : 1249753487AB
		hwbbox = bsp->bbox[side^1];
		HWR_RenderBSPNode(bsp->children[side^1]);
	}
}

/*
//
// Clear 'stack' of subsectors to draw
//
static void HWR_ClearDrawSubsectors(void)
{
	gr_drawsubsector_p = gr_drawsubsectors;
}

//
// Draw subsectors pushed on the drawsubsectors 'stack', back to front
//
static void HWR_RenderSubsectors(void)
{
	while (gr_drawsubsector_p > gr_drawsubsectors)
	{
		HWR_RenderBSPNode(
		lastsubsec->nextsubsec = bspnum & (~NF_SUBSECTOR);
	}
}
*/

// ==========================================================================
//                                                              FROM R_MAIN.C
// ==========================================================================

//BP : exactely the same as R_InitTextureMapping
void HWR_InitTextureMapping(void)
{
	angle_t i;
	INT32 x;
	INT32 t;
	fixed_t focallength;
	fixed_t grcenterx;
	fixed_t grcenterxfrac;
	INT32 grviewwidth;

#define clipanglefov (FIELDOFVIEW>>ANGLETOFINESHIFT)

	grviewwidth = vid.width;
	grcenterx = grviewwidth/2;
	grcenterxfrac = grcenterx<<FRACBITS;

	// Use tangent table to generate viewangletox:
	//  viewangletox will give the next greatest x
	//  after the view angle.
	//
	// Calc focallength
	//  so FIELDOFVIEW angles covers SCREENWIDTH.
	focallength = FixedDiv(grcenterxfrac,
		FINETANGENT(FINEANGLES/4+clipanglefov/2));

	for (i = 0; i < FINEANGLES/2; i++)
	{
		if (FINETANGENT(i) > FRACUNIT*2)
			t = -1;
		else if (FINETANGENT(i) < -FRACUNIT*2)
			t = grviewwidth+1;
		else
		{
			t = FixedMul(FINETANGENT(i), focallength);
			t = (grcenterxfrac - t+FRACUNIT-1)>>FRACBITS;

			if (t < -1)
				t = -1;
			else if (t > grviewwidth+1)
				t = grviewwidth+1;
		}
		gr_viewangletox[i] = t;
	}

	// Scan viewangletox[] to generate xtoviewangle[]:
	//  xtoviewangle will give the smallest view angle
	//  that maps to x.
	for (x = 0; x <= grviewwidth; x++)
	{
		i = 0;
		while (gr_viewangletox[i]>x)
			i++;
		gr_xtoviewangle[x] = (i<<ANGLETOFINESHIFT) - ANGLE_90;
	}

	// Take out the fencepost cases from viewangletox.
	for (i = 0; i < FINEANGLES/2; i++)
	{
		if (gr_viewangletox[i] == -1)
			gr_viewangletox[i] = 0;
		else if (gr_viewangletox[i] == grviewwidth+1)
			gr_viewangletox[i]  = grviewwidth;
	}

	gr_clipangle = gr_xtoviewangle[0];
}

// ==========================================================================
// gr_things.c
// ==========================================================================

// sprites are drawn after all wall and planes are rendered, so that
// sprite translucency effects apply on the rendered view (instead of the background sky!!)

static gr_vissprite_t gr_vissprites[MAXVISSPRITES];
static gr_vissprite_t *gr_vissprite_p;

// --------------------------------------------------------------------------
// HWR_ClearSprites
// Called at frame start.
// --------------------------------------------------------------------------
static void HWR_ClearSprites(void)
{
	gr_vissprite_p = gr_vissprites;
}


// --------------------------------------------------------------------------
// HWR_NewVisSprite
// --------------------------------------------------------------------------
static gr_vissprite_t gr_overflowsprite;

static gr_vissprite_t *HWR_NewVisSprite(void)
{
	if (gr_vissprite_p == &gr_vissprites[MAXVISSPRITES])
		return &gr_overflowsprite;

	gr_vissprite_p++;
	return gr_vissprite_p-1;
}

// Finds a floor through which light does not pass.
static fixed_t HWR_OpaqueFloorAtPos(fixed_t x, fixed_t y, fixed_t z, fixed_t height)
{
	const sector_t *sec = R_PointInSubsector(x, y)->sector;
	fixed_t floorz = sec->floorheight;

	if (sec->ffloors)
	{
		ffloor_t *rover;
		fixed_t delta1, delta2;
		const fixed_t thingtop = z + height;

		for (rover = sec->ffloors; rover; rover = rover->next)
		{
			if (!(rover->flags & FF_EXISTS)
			|| !(rover->flags & FF_RENDERPLANES)
			|| rover->flags & FF_TRANSLUCENT
			|| rover->flags & FF_FOG
			|| rover->flags & FF_INVERTPLANES)
				continue;

			delta1 = z - (*rover->bottomheight + ((*rover->topheight - *rover->bottomheight)/2));
			delta2 = thingtop - (*rover->bottomheight + ((*rover->topheight - *rover->bottomheight)/2));
			if (*rover->topheight > floorz && abs(delta1) < abs(delta2))
				floorz = *rover->topheight;
		}
	}

	return floorz;
}

// -----------------+
// HWR_DrawSprite   : Draw flat sprites
//                  : (monsters, bonuses, weapons, lights, ...)
// Returns          :
// -----------------+
static void HWR_DrawSprite(gr_vissprite_t *spr)
{
	UINT8 i;
	float tr_x, tr_y;
	FOutVector wallVerts[4];
	FOutVector *wv;
	GLPatch_t *gpatch; // sprite patch converted to hardware
	FSurfaceInfo Surf;
	const boolean hires = (spr->mobj && spr->mobj->flags & MF_HIRES);

	if (!spr->mobj)
		return;

	if (!spr->mobj->subsector)
		return;

	// cache sprite graphics
	//12/12/99: Hurdler:
	//          OK, I don't change anything for MD2 support because I want to be
	//          sure to do it the right way. So actually, we keep normal sprite
	//          in memory and we add the md2 model if it exists for that sprite

	gpatch = W_CachePatchNum(spr->patchlumpnum, PU_CACHE);

	// create the sprite billboard
	//
	//  3--2
	//  | /|
	//  |/ |
	//  0--1
	wallVerts[0].x = wallVerts[3].x = spr->x1;
	wallVerts[2].x = wallVerts[1].x = spr->x2;
	wallVerts[2].y = wallVerts[3].y = spr->ty;
	wallVerts[0].y = wallVerts[1].y = spr->ty - gpatch->height;

	if (hires)
	{
		wallVerts[0].x = wallVerts[3].x += (spr->x2-spr->x1)/4;
		wallVerts[2].x = wallVerts[1].x -= (spr->x2-spr->x1)/4;
	}

	if(spr->mobj && spr->mobj->scale != 100)
	{
		const float radius = (spr->x2-spr->x1)/2;
		const float scale = (float)(100-spr->mobj->scale);
		if(spr->mobj->flags & MF_HIRES)
		{
			wallVerts[0].x = wallVerts[3].x += FLOATSCALE(radius,scale)/2;
			wallVerts[2].x = wallVerts[1].x -= FLOATSCALE(radius,scale)/2;
			wallVerts[0].y = wallVerts[1].y += FLOATSCALE(abs(gpatch->topoffset-gpatch->height),scale/2);
			wallVerts[2].y = wallVerts[3].y += FLOATSCALE(abs(gpatch->topoffset-gpatch->height),scale/2);
			wallVerts[2].y = wallVerts[3].y -= FLOATSCALE(gpatch->height,scale);
		}
		else
		{
			wallVerts[0].x = wallVerts[3].x += FLOATSCALE(radius,scale);
			wallVerts[2].x = wallVerts[1].x -= FLOATSCALE(radius,scale);
			wallVerts[0].y = wallVerts[1].y += FLOATSCALE(abs(gpatch->topoffset-gpatch->height),scale);
			wallVerts[2].y = wallVerts[3].y += FLOATSCALE(abs(gpatch->topoffset-gpatch->height),scale);
			wallVerts[2].y = wallVerts[3].y -= FLOATSCALE(gpatch->height,scale);
		}
	}

	// make a wall polygon (with 2 triangles), using the floor/ceiling heights,
	// and the 2d map coords of start/end vertices
	wallVerts[0].z = wallVerts[1].z = wallVerts[2].z = wallVerts[3].z = spr->tz;

	// transform
	wv = wallVerts;

	for (i = 0; i < 4; i++,wv++)
	{
		//look up/down ----TOTAL SUCKS!!!--- do the 2 in one!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
		tr_x = wv->z;
		tr_y = wv->y;
		wv->y = (tr_x * gr_viewludcos) + (tr_y * gr_viewludsin);
		wv->z = (tr_x * gr_viewludsin) - (tr_y * gr_viewludcos);
		// ---------------------- mega lame test ----------------------------------

		//scale y before frustum so that frustum can be scaled to screen height
		wv->y *= ORIGINAL_ASPECT * gr_fovlud;
		wv->x *= gr_fovlud;
	}

	if (spr->flip)
	{
		wallVerts[0].sow = wallVerts[3].sow = gpatch->max_s;
		wallVerts[2].sow = wallVerts[1].sow = 0;
	}else{
		wallVerts[0].sow = wallVerts[3].sow = 0;
		wallVerts[2].sow = wallVerts[1].sow = gpatch->max_s;
	}

	wallVerts[3].tow = wallVerts[2].tow = 0;

	if (hires)
		wallVerts[0].tow = wallVerts[1].tow = gpatch->max_t*2;
	else
		wallVerts[0].tow = wallVerts[1].tow = gpatch->max_t;

	// flip the texture coords
	if (spr->vflip)
	{
		FLOAT temp = wallVerts[0].tow;
		wallVerts[0].tow = wallVerts[3].tow;
		wallVerts[3].tow = temp;
		temp = wallVerts[1].tow;
		wallVerts[1].tow = wallVerts[2].tow;
		wallVerts[2].tow = temp;
	}

	// cache the patch in the graphics card memory
	//12/12/99: Hurdler: same comment as above (for md2)
	//Hurdler: 25/04/2000: now support colormap in hardware mode
	HWR_GetMappedPatch(gpatch, spr->colormap);

	// Draw shadow BEFORE SPRITE.
	if (cv_shadow.value // Shadows enabled
		&& !(spr->mobj->flags & MF_SCENERY && spr->mobj->flags & MF_SPAWNCEILING && spr->mobj->flags & MF_NOGRAVITY) // Ceiling scenery have no shadow.
		&& !(spr->mobj->flags2 & MF2_DEBRIS) // Debris have no corona or shadow.
		&& (spr->mobj->z >= spr->mobj->floorz)) // Without this, your shadow shows on the floor, even after you die and fall through the ground.
	{
		////////////////////
		// SHADOW SPRITE! //
		////////////////////
		FOutVector swallVerts[4];
		FSurfaceInfo sSurf;
		fixed_t floorheight, mobjfloor;

		mobjfloor = HWR_OpaqueFloorAtPos(
			spr->mobj->x, spr->mobj->y,
			spr->mobj->z, spr->mobj->height);
		if (cv_shadowoffs.value)
		{
			angle_t shadowdir;

			// Set direction
			if (splitscreen && stplyr != &players[displayplayer])
				shadowdir = localangle2 + FixedAngle(cv_cam2_rotate.value);
			else
				shadowdir = localangle + FixedAngle(cv_cam_rotate.value);

			// Find floorheight
			floorheight = HWR_OpaqueFloorAtPos(
				spr->mobj->x + P_ReturnThrustX(spr->mobj, shadowdir, spr->mobj->z - mobjfloor),
				spr->mobj->y + P_ReturnThrustY(spr->mobj, shadowdir, spr->mobj->z - mobjfloor),
				spr->mobj->z, spr->mobj->height);

			// The shadow is falling ABOVE it's mobj?
			// Don't draw it, then!
			if (spr->mobj->z < floorheight)
				goto noshadow;
			else
			{
				fixed_t floorz;
				floorz = HWR_OpaqueFloorAtPos(
					spr->mobj->x + P_ReturnThrustX(spr->mobj, shadowdir, spr->mobj->z - floorheight),
					spr->mobj->y + P_ReturnThrustY(spr->mobj, shadowdir, spr->mobj->z - floorheight),
					spr->mobj->z, spr->mobj->height);
				// The shadow would be falling on a wall? Don't draw it, then.
				// Would draw midair otherwise.
				if (floorz < floorheight)
					goto noshadow;
			}

			floorheight = FixedInt(spr->mobj->z - floorheight);
		}
		else
			floorheight = FixedInt(spr->mobj->z - mobjfloor);

		// create the sprite billboard
		//
		//  3--2
		//  | /|
		//  |/ |
		//  0--1
		swallVerts[0].x = swallVerts[3].x = spr->x1;
		swallVerts[2].x = swallVerts[1].x = spr->x2;

		if (hires)
		{
			swallVerts[0].x = swallVerts[3].x += (spr->x2-spr->x1)/4;
			swallVerts[2].x = swallVerts[1].x -= (spr->x2-spr->x1)/4;

			// Always a pixel above the floor, perfectly flat.
			swallVerts[0].y = swallVerts[1].y = swallVerts[2].y = swallVerts[3].y =
				spr->ty - gpatch->height/2 - (gpatch->topoffset-gpatch->height)/2 - (floorheight-1);

			// Spread out top away from the camera. (Fixme: Make it always move out in the same direction!... somehow.)
			swallVerts[0].z = swallVerts[1].z = spr->tz - (gpatch->height-gpatch->topoffset)/2;
			swallVerts[2].z = swallVerts[3].z = spr->tz + gpatch->height/2 - (gpatch->height-gpatch->topoffset)/2;
		}
		else
		{
			// Always a pixel above the floor, perfectly flat.
			swallVerts[0].y = swallVerts[1].y = swallVerts[2].y = swallVerts[3].y =
				spr->ty - gpatch->height - (gpatch->topoffset-gpatch->height) - (floorheight+3);

			// Spread out top away from the camera. (Fixme: Make it always move out in the same direction!... somehow.)
			swallVerts[0].z = swallVerts[1].z = spr->tz - (gpatch->height-gpatch->topoffset);
			swallVerts[2].z = swallVerts[3].z = spr->tz + gpatch->height - (gpatch->height-gpatch->topoffset);
		}

		// Huh setting the Y higher? Also moving this before to stop the alterations affecting it after scaling.
		if (spr->mobj && spr->mobj->scale != 100)
		{
			const float radius = (spr->x2-spr->x1)/2;
			const float scale = (float)(100-spr->mobj->scale);
			if(spr->mobj->flags & MF_HIRES)
			{
				swallVerts[0].x = swallVerts[3].x += FLOATSCALE(radius,scale)/2;
				swallVerts[2].x = swallVerts[1].x -= FLOATSCALE(radius,scale)/2;
				swallVerts[2].z = swallVerts[3].z -= FLOATSCALE(gpatch->height,scale);
			}
			else
			{
				swallVerts[0].x = swallVerts[3].x += FLOATSCALE(radius,scale);
				swallVerts[2].x = swallVerts[1].x -= FLOATSCALE(radius,scale);
				swallVerts[2].z = swallVerts[3].z -= FLOATSCALE(gpatch->height,scale);
			}
		}

		// transform
		wv = swallVerts;

		for (i = 0; i < 4; i++,wv++)
		{
			// Offset away from the camera based on height from floor.
			if (cv_shadowoffs.value)
				wv->z += floorheight;
			wv->z += 3;

			//look up/down ----TOTAL SUCKS!!!--- do the 2 in one!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
			tr_x = wv->z;
			tr_y = wv->y;
			wv->y = (tr_x * gr_viewludcos) + (tr_y * gr_viewludsin);
			wv->z = (tr_x * gr_viewludsin) - (tr_y * gr_viewludcos);
			// ---------------------- mega lame test ----------------------------------

			//scale y before frustum so that frustum can be scaled to screen height
			wv->y *= ORIGINAL_ASPECT * gr_fovlud;
			wv->x *= gr_fovlud;
		}

		if (spr->flip)
		{
			swallVerts[0].sow = swallVerts[3].sow = gpatch->max_s;
			swallVerts[2].sow = swallVerts[1].sow = 0;
		}
		else
		{
			swallVerts[0].sow = swallVerts[3].sow = 0;
			swallVerts[2].sow = swallVerts[1].sow = gpatch->max_s;
		}
		swallVerts[3].tow = swallVerts[2].tow = 0;

		if (hires)
			swallVerts[0].tow = swallVerts[1].tow = gpatch->max_t*2;
		else
			swallVerts[0].tow = swallVerts[1].tow = gpatch->max_t;

		sSurf.FlatColor.s.red = 0x00;
		sSurf.FlatColor.s.blue = 0x00;
		sSurf.FlatColor.s.green = 0x00;

		if (spr->mobj->frame & FF_TRANSMASK || spr->mobj->flags2 & MF2_SHADOW)
		{
#ifdef HARDWAREFIX
			sector_t *sector = spr->mobj->subsector->sector;
			UINT8 lightlevel = LightLevelToLum(sector->lightlevel);
			extracolormap_t *colormap = sector->extra_colormap;

			if (sector->numlights)
			{
				INT32 light = R_GetPlaneLight(sector, spr->mobj->floorz, false);

				if (!(spr->mobj->frame & FF_FULLBRIGHT))
					lightlevel = LightLevelToLum(*sector->lightlist[light].lightlevel);
				else
					lightlevel = LightLevelToLum(255);

				if (sector->lightlist[light].extra_colormap)
					colormap = sector->lightlist[light].extra_colormap;
			}
			else
			{
				lightlevel = LightLevelToLum(sector->lightlevel);

				if (sector->extra_colormap)
					colormap = sector->extra_colormap;
			}

			if (colormap)
				sSurf.FlatColor.rgba = HWR_Lighting(lightlevel/2, colormap->rgba, colormap->fadergba, false, true);
			else
				sSurf.FlatColor.rgba = HWR_Lighting(lightlevel/2, NORMALFOG, FADEFOG, false, true);
#else
			// sprite lighting by modulating the RGB components
			sSurf.FlatColor.s.red = sSurf.FlatColor.s.green = sSurf.FlatColor.s.blue = (UINT8)(spr->sectorlight/2);
#endif
		}
		else
#ifdef HARDWAREFIX
			Surf.FlatColor.rgba = NORMALFOG;
#else
			sSurf.FlatColor.s.red = sSurf.FlatColor.s.green = sSurf.FlatColor.s.blue = 0x00;
#endif

		/// \todo do the test earlier
		if (!cv_grmd2.value || (md2_models[spr->mobj->sprite].scale < 0.0f) || (md2_models[spr->mobj->sprite].notfound = true) || (md2_playermodels[(skin_t*)spr->mobj->skin-skins].scale < 0.0f) || (md2_playermodels[(skin_t*)spr->mobj->skin-skins].notfound = true))
		{
			if (0x80 > floorheight/4)
			{
				sSurf.FlatColor.s.alpha = (UINT8)(0x80 - floorheight/4);
				HWD.pfnDrawPolygon(&sSurf, swallVerts, 4, PF_Translucent|PF_Modulated|PF_Clip);
			}
		}
	}

noshadow:

	// This needs to be AFTER the shadows so that the regular sprites aren't drawn completely black.
	// sprite lighting by modulating the RGB components
	/// \todo coloured
#ifndef HARDWAREFIX
	Surf.FlatColor.s.red = Surf.FlatColor.s.green = Surf.FlatColor.s.blue = spr->sectorlight;
#endif

	// colormap test
	{
#ifdef HARDWAREFIX
		sector_t *sector = spr->mobj->subsector->sector;
		UINT8 lightlevel = LightLevelToLum(sector->lightlevel);
		extracolormap_t *colormap = sector->extra_colormap;

		if (sector->numlights)
		{
			INT32 light = R_GetPlaneLight(sector, spr->mobj->z, false);

			if ((sector->lightlist[light].height > (spr->mobj->z + spr->mobj->height)) && !(sector->lightlist[light].flags & FF_NOSHADE))
			{
				if (!(spr->mobj->frame & FF_FULLBRIGHT))
					lightlevel = LightLevelToLum(*sector->lightlist[light].lightlevel);
				else
					lightlevel = LightLevelToLum(255);

				if (sector->lightlist[light].extra_colormap)
					colormap = sector->lightlist[light].extra_colormap;
			}
			else // If we can't use the light at its bottom, we'll use the light at its top
			{
				light = R_GetPlaneLight(sector, spr->mobj->z + spr->mobj->height, false);

				if (!(spr->mobj->frame & FF_FULLBRIGHT))
					lightlevel = LightLevelToLum(*sector->lightlist[light].lightlevel);
				else
					lightlevel = LightLevelToLum(255);

				if (sector->lightlist[light].extra_colormap)
					colormap = sector->lightlist[light].extra_colormap;
			}
		}
		else
		{
			if (!(spr->mobj->frame & FF_FULLBRIGHT))
				lightlevel = LightLevelToLum(sector->lightlevel);
			else
				lightlevel = LightLevelToLum(255);

			if (sector->extra_colormap)
				colormap = sector->extra_colormap;
		}

		if (spr->mobj->frame & FF_FULLBRIGHT)
			lightlevel = LightLevelToLum(255);

		if (colormap)
			Surf.FlatColor.rgba = HWR_Lighting(lightlevel, colormap->rgba, colormap->fadergba, false, false);
		else
			Surf.FlatColor.rgba = HWR_Lighting(lightlevel, NORMALFOG, FADEFOG, false, false);
#else

		sector_t *sector = spr->mobj->subsector->sector;

		if (sector->ffloors)
		{
			ffloor_t *caster = sector->lightlist[R_GetPlaneLight(sector, spr->mobj->z, false)].caster;
			sector = caster ? &sectors[caster->secnum] : sector;
		}
		{
			RGBA_t temp;
			INT32 alpha;

			temp.rgba = sector->extra_colormap->rgba;
			alpha = (26 - temp.s.alpha)*spr->sectorlight;
			Surf.FlatColor.s.red = (UINT8)((alpha + temp.s.alpha*temp.s.red)/26);
			Surf.FlatColor.s.blue = (UINT8)((alpha + temp.s.alpha*temp.s.blue)/26);
			Surf.FlatColor.s.green = (UINT8)((alpha + temp.s.alpha*temp.s.green)/26);
			Surf.FlatColor.s.alpha = 0xff;
		}
#endif
	}

	/// \todo do the test earlier
	if (!cv_grmd2.value || (md2_models[spr->mobj->sprite].scale < 0.0f))
	{
		FBITFIELD blend = 0;
		if (spr->mobj->flags2 & MF2_SHADOW)
		{
			Surf.FlatColor.s.alpha = 0x40;
			blend = PF_Translucent;
		}
		else if (spr->mobj->frame & FF_TRANSMASK)
			blend = HWR_TranstableToAlpha((spr->mobj->frame & FF_TRANSMASK)>>FF_TRANSSHIFT, &Surf);
		else
		{
			// BP: i agree that is little better in environement but it don't
			//     work properly under glide nor with fogcolor to ffffff :(
			// Hurdler: PF_Environement would be cool, but we need to fix
			//          the issue with the fog before
			Surf.FlatColor.s.alpha = 0xFF;
			blend = PF_Translucent|PF_Occlude;
		}

		HWD.pfnDrawPolygon(&Surf, wallVerts, 4, blend|PF_Modulated|PF_Clip);
	}
}

#ifdef HWPRECIP
// Sprite drawer for precipitation
static inline void HWR_DrawPrecipitationSprite(gr_vissprite_t *spr)
{
	UINT8 i;
	FBITFIELD blend = 0;
	float tr_x, tr_y;
	FOutVector wallVerts[4];
	FOutVector *wv;
	GLPatch_t *gpatch; // sprite patch converted to hardware
	FSurfaceInfo Surf;

	if (!spr->mobj)
		return;

	if (!spr->mobj->subsector)
		return;

	// cache sprite graphics
	gpatch = W_CachePatchNum(spr->patchlumpnum, PU_CACHE);

	// create the sprite billboard
	//
	//  3--2
	//  | /|
	//  |/ |
	//  0--1
	wallVerts[0].x = wallVerts[3].x = spr->x1;
	wallVerts[2].x = wallVerts[1].x = spr->x2;
	wallVerts[2].y = wallVerts[3].y = spr->ty;
	wallVerts[0].y = wallVerts[1].y = spr->ty - gpatch->height;

	// make a wall polygon (with 2 triangles), using the floor/ceiling heights,
	// and the 2d map coords of start/end vertices
	wallVerts[0].z = wallVerts[1].z = wallVerts[2].z = wallVerts[3].z = spr->tz;

	// transform
	wv = wallVerts;

	for (i = 0; i < 4; i++, wv++)
	{
		//look up/down ----TOTAL SUCKS!!!--- do the 2 in one!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
		tr_x = wv->z;
		tr_y = wv->y;
		wv->y = (tr_x * gr_viewludcos) + (tr_y * gr_viewludsin);
		wv->z = (tr_x * gr_viewludsin) - (tr_y * gr_viewludcos);
		// ---------------------- mega lame test ----------------------------------

		//scale y before frustum so that frustum can be scaled to screen height
		wv->y *= ORIGINAL_ASPECT * gr_fovlud;
		wv->x *= gr_fovlud;
	}

	wallVerts[0].sow = wallVerts[3].sow = 0;
	wallVerts[2].sow = wallVerts[1].sow = gpatch->max_s;

	wallVerts[3].tow = wallVerts[2].tow = 0;
	wallVerts[0].tow = wallVerts[1].tow = gpatch->max_t;

	// cache the patch in the graphics card memory
	//12/12/99: Hurdler: same comment as above (for md2)
	//Hurdler: 25/04/2000: now support colormap in hardware mode
	HWR_GetMappedPatch(gpatch, spr->colormap);

	sector_t *sector = spr->mobj->subsector->sector;

	if (sector->ffloors)
	{
		ffloor_t *caster = sector->lightlist[R_GetPlaneLight(sector, spr->mobj->z, false)].caster;
		sector = caster ? &sectors[caster->secnum] : sector;
	}


	// sprite lighting by modulating the RGB components
	if (sector->extra_colormap)
#ifdef HARDWAREFIX
			Surf.FlatColor.rgba = HWR_Lighting(spr->sectorlight,sector->extra_colormap->rgba,sector->extra_colormap->fadergba, false, false);
		else
			Surf.FlatColor.rgba = HWR_Lighting(spr->sectorlight,NORMALFOG,FADEFOG, false, false);
#endif

	if (spr->mobj->flags2 & MF2_SHADOW)
	{
		Surf.FlatColor.s.alpha = 0x40;
		blend = PF_Translucent;
	}
	else if (spr->mobj->frame & FF_TRANSMASK)
		blend = HWR_TranstableToAlpha((spr->mobj->frame & FF_TRANSMASK)>>FF_TRANSSHIFT, &Surf);
	else
	{
		// BP: i agree that is little better in environement but it don't
		//     work properly under glide nor with fogcolor to ffffff :(
		// Hurdler: PF_Environement would be cool, but we need to fix
		//          the issue with the fog before
		Surf.FlatColor.s.alpha = 0xFF;
		blend = PF_Translucent|PF_Occlude;
	}

	HWD.pfnDrawPolygon(&Surf, wallVerts, 4, blend|PF_Modulated|PF_Clip);
}
#endif

// --------------------------------------------------------------------------
// Sort vissprites by distance
// --------------------------------------------------------------------------
static gr_vissprite_t gr_vsprsortedhead;

static void HWR_SortVisSprites(void)
{
	size_t i, count;
	gr_vissprite_t *ds;
	gr_vissprite_t *best = NULL;
	gr_vissprite_t unsorted;
	float bestdist;

	count = gr_vissprite_p - gr_vissprites;

	unsorted.next = unsorted.prev = &unsorted;

	if (!count)
		return;

	for (ds = gr_vissprites; ds < gr_vissprite_p; ds++)
	{
		ds->next = ds+1;
		ds->prev = ds-1;
	}

	gr_vissprites[0].prev = &unsorted;
	unsorted.next = &gr_vissprites[0];
	(gr_vissprite_p-1)->next = &unsorted;
	unsorted.prev = gr_vissprite_p-1;

	// pull the vissprites out by scale
	gr_vsprsortedhead.next = gr_vsprsortedhead.prev = &gr_vsprsortedhead;
	for (i = 0; i < count; i++)
	{
		bestdist = ZCLIP_PLANE-1;
		for (ds = unsorted.next; ds != &unsorted; ds = ds->next)
		{
			if (ds->tz > bestdist)
			{
				bestdist = ds->tz;
				best = ds;
			}
		}
		best->next->prev = best->prev;
		best->prev->next = best->next;
		best->next = &gr_vsprsortedhead;
		best->prev = gr_vsprsortedhead.prev;
		gr_vsprsortedhead.prev->next = best;
		gr_vsprsortedhead.prev = best;
	}
}

// A drawnode is something that points to a 3D floor, 3D side, or masked
// middle texture. This is used for sorting with sprites.
typedef struct
{
	wallVert3D    wallVerts[4];
	FSurfaceInfo  Surf;
	INT32         texnum;
	FBITFIELD     blend;
	INT32         drawcount;
#ifndef SORTING
	fixed_t       fixedheight;
#endif
	boolean fogwall;
	INT32 lightlevel;
	extracolormap_t *wallcolormap; // Doing the lighting in HWR_RenderWall now for correct fog after sorting
} wallinfo_t;

static wallinfo_t *wallinfo = NULL;
static size_t numwalls = 0; // a list of transparent walls to be drawn

static void HWR_RenderWall(wallVert3D   *wallVerts, FSurfaceInfo *pSurf, FBITFIELD blend, boolean fogwall, INT32 lightlevel, extracolormap_t *wallcolormap);

#define MAX_TRANSPARENTWALL 256

typedef struct
{
	extrasubsector_t *xsub;
	fixed_t fixedheight;
	INT32 lightlevel;
	lumpnum_t lumpnum;
	INT32 alpha;
	sector_t *FOFSector;
	FBITFIELD blend;
	boolean fogplane;
	extracolormap_t *planecolormap;
	INT32 drawcount;
} planeinfo_t;

static size_t numplanes = 0; // a list of transparent floors to be drawn
static planeinfo_t *planeinfo = NULL;

#ifndef SORTING
size_t numfloors = 0;
#else
//static floorinfo_t *floorinfo = NULL;
//static size_t numfloors = 0;
//Hurdler: 3D water sutffs
typedef struct gr_drawnode_s
{
	planeinfo_t *plane;
	wallinfo_t *wall;
	gr_vissprite_t *sprite;

//	struct gr_drawnode_s *next;
//	struct gr_drawnode_s *prev;
} gr_drawnode_t;

static INT32 drawcount = 0;

#define MAX_TRANSPARENTFLOOR 512

// This will likely turn into a copy of HWR_Add3DWater and replace it.
void HWR_AddTransparentFloor(lumpnum_t lumpnum, extrasubsector_t *xsub,
	fixed_t fixedheight, INT32 lightlevel, INT32 alpha, sector_t *FOFSector, FBITFIELD blend, boolean fogplane, extracolormap_t *planecolormap)
{
	if (!(numplanes % MAX_TRANSPARENTFLOOR))
	{
		planeinfo = Z_Realloc(planeinfo,
			(numplanes + MAX_TRANSPARENTFLOOR) * sizeof *planeinfo, PU_STATIC, NULL);
	}

	planeinfo[numplanes].fixedheight = fixedheight;
	planeinfo[numplanes].lightlevel = lightlevel;
	planeinfo[numplanes].lumpnum = lumpnum;
	planeinfo[numplanes].xsub = xsub;
	planeinfo[numplanes].alpha = alpha;
	planeinfo[numplanes].FOFSector = FOFSector;
	planeinfo[numplanes].blend = blend;
	planeinfo[numplanes].fogplane = fogplane;
	planeinfo[numplanes].planecolormap = planecolormap;
	planeinfo[numplanes].drawcount = drawcount++;
	numplanes++;
}

//
// HWR_CreateDrawNodes
// Creates and sorts a list of drawnodes for the scene being rendered.
static void HWR_CreateDrawNodes(void)
{
	UINT32 i = 0, p = 0, prev = 0, loop;
	const fixed_t pviewz = dup_viewz;

	// Dump EVERYTHING into a huge drawnode list. Then we'll sort it!
	// Could this be optimized into _AddTransparentWall/_AddTransparentPlane?
	// Hell yes! But sort algorithm must be modified to use a linked list.
	gr_drawnode_t *sortnode = Z_Calloc((sizeof(planeinfo_t)*numplanes)
									+ (sizeof(wallinfo_t)*numwalls)
									,PU_STATIC, NULL);
	// todo:
	// However, in reality we shouldn't be re-copying and shifting all this information
	// that is already lying around. This should all be in some sort of linked list or lists. -Jazz
	size_t *sortindex = Z_Calloc(sizeof(size_t) * (numplanes + numwalls), PU_STATIC, NULL);

	// If true, swap the draw order.
	boolean shift = false;

	for (i = 0; i < numplanes; i++, p++)
	{
		sortnode[p].plane = &planeinfo[i];
		sortindex[p] = p;
	}

	for (i = 0; i < numwalls; i++, p++)
	{
		sortnode[p].wall = &wallinfo[i];
		sortindex[p] = p;
	}

	// p is the number of stuff to sort

	// Add the 3D floors, thicksides, and masked textures...
	// Instead of going through drawsegs, we need to iterate
	// through the lists of masked textures and
	// translucent ffloors being drawn.

	// This is a bubble sort! Wahoo!

	// Stuff is sorted:
	//      sortnode[sortindex[0]]   = farthest away
	//      sortnode[sortindex[p-1]] = closest
	// "i" should be closer. "prev" should be further.
	// The lower drawcount is, the further it is from the screen.

	for (loop = 0; loop < p; loop++)
	{
		for (i = 1; i < p; i++)
		{
			prev = i-1;
			if (sortnode[sortindex[i]].plane)
			{
				// What are we comparing it with?
				if (sortnode[sortindex[prev]].plane)
				{
					// Plane (i) is further away than plane (prev)
					if (ABS(sortnode[sortindex[i]].plane->fixedheight - pviewz) > ABS(sortnode[sortindex[prev]].plane->fixedheight - pviewz))
						shift = true;
				}
				else if (sortnode[sortindex[prev]].wall)
				{
					// Plane (i) is further than wall (prev)
					if (sortnode[sortindex[i]].plane->drawcount > sortnode[sortindex[prev]].wall->drawcount)
						shift = true;
				}
			}
			else if (sortnode[sortindex[i]].wall)
			{
				// What are we comparing it with?
				if (sortnode[sortindex[prev]].plane)
				{
					// Wall (i) is further than plane(prev)
					if (sortnode[sortindex[i]].wall->drawcount > sortnode[sortindex[prev]].plane->drawcount)
						shift = true;
				}
				else if (sortnode[sortindex[prev]].wall)
				{
					// Wall (i) is further than wall (prev)
					if (sortnode[sortindex[i]].wall->drawcount > sortnode[sortindex[prev]].wall->drawcount)
						shift = true;
				}
			}

			if (shift)
			{
				size_t temp;

				temp = sortindex[prev];
				sortindex[prev] = sortindex[i];
				sortindex[i] = temp;

				shift = false;
			}

		} //i++
	} // loop++

	HWD.pfnSetTransform(&atransform);
	// Okay! Let's draw it all! Woo!
	for (i = 0; i < p; i++)
	{
		if (sortnode[sortindex[i]].plane)
		{
			// We aren't traversing the BSP tree, so make gr_frontsector null to avoid crashes.
			gr_frontsector = NULL;

			if (!(sortnode[sortindex[i]].plane->blend & PF_NoTexture))
				HWR_GetFlat(sortnode[sortindex[i]].plane->lumpnum);
			HWR_RenderPlane(NULL, sortnode[sortindex[i]].plane->xsub, sortnode[sortindex[i]].plane->fixedheight, sortnode[sortindex[i]].plane->blend, sortnode[sortindex[i]].plane->lightlevel,
				sortnode[sortindex[i]].plane->lumpnum, sortnode[sortindex[i]].plane->FOFSector, sortnode[sortindex[i]].plane->alpha, sortnode[sortindex[i]].plane->fogplane, sortnode[sortindex[i]].plane->planecolormap);
		}
		else if (sortnode[sortindex[i]].wall)
		{
			if (!(sortnode[sortindex[i]].wall->blend & PF_NoTexture))
				HWR_GetTexture(sortnode[sortindex[i]].wall->texnum);
			HWR_RenderWall(sortnode[sortindex[i]].wall->wallVerts, &sortnode[sortindex[i]].wall->Surf, sortnode[sortindex[i]].wall->blend, sortnode[sortindex[i]].wall->fogwall,
				sortnode[sortindex[i]].wall->lightlevel, sortnode[sortindex[i]].wall->wallcolormap);
		}
	}

	numwalls = 0;
	numplanes = 0;

	// No mem leaks, please.
	Z_Free(sortnode);
	Z_Free(sortindex);
}

#endif

// --------------------------------------------------------------------------
//  Draw all vissprites
// --------------------------------------------------------------------------
#ifdef SORTING //Why weren't we using this before? -Jazz
static void HWR_DrawSprites(void)
{
	if (gr_vissprite_p > gr_vissprites)
	{
		gr_vissprite_t *spr;

		// draw all vissprites back to front
		for (spr = gr_vsprsortedhead.next;
		     spr != &gr_vsprsortedhead;
		     spr = spr->next)
		{
#ifdef HWPRECIP
			if (spr->precip)
				HWR_DrawPrecipitationSprite(spr);
			else
#endif
				if (spr->mobj->skin)
				{
					if (!cv_grmd2.value || (cv_grmd2.value && md2_playermodels[(skin_t*)spr->mobj->skin-skins].notfound == true))
						HWR_DrawSprite(spr);
				}
				else if (!cv_grmd2.value || (cv_grmd2.value && md2_models[spr->mobj->sprite].notfound == true))
					HWR_DrawSprite(spr);
		}
	}
}
#endif
// --------------------------------------------------------------------------
//  Draw all MD2
// --------------------------------------------------------------------------
static void HWR_DrawMD2S(void)
{
	if (gr_vissprite_p > gr_vissprites)
	{
		gr_vissprite_t *spr;

		// draw all MD2 back to front
		for (spr = gr_vsprsortedhead.next;
			spr != &gr_vsprsortedhead;
			spr = spr->next)
		{
#ifdef HWPRECIP
			if (!spr->precip)
			{
#endif
				if (spr->mobj && spr->mobj->skin)
				{
					if ((md2_playermodels[(skin_t*)spr->mobj->skin-skins].notfound == false) && (md2_playermodels[(skin_t*)spr->mobj->skin-skins].scale > 0.0f))
						HWR_DrawMD2(spr);
				}
				else if (md2_models[spr->mobj->sprite].notfound == false && md2_models[spr->mobj->sprite].scale > 0.0f)
					HWR_DrawMD2(spr);
#ifdef HWPRECIP
			}
#endif
		}
	}
}

// --------------------------------------------------------------------------
// HWR_AddSprites
// During BSP traversal, this adds sprites by sector.
// --------------------------------------------------------------------------
static UINT8 sectorlight;
static void HWR_AddSprites(sector_t *sec)
{
	mobj_t *thing;
#ifdef HWPRECIP
	precipmobj_t *precipthing;
#endif
	fixed_t adx, ady, approx_dist;

	// BSP is traversed by subsector.
	// A sector might have been split into several
	//  subsectors during BSP building.
	// Thus we check whether its already added.
	if (sec->validcount == validcount)
		return;

	// Well, now it will be done.
	sec->validcount = validcount;

	// sprite lighting
	sectorlight = LightLevelToLum(sec->lightlevel & 0xff);

	// NiGHTS stages have a draw distance limit because of the
	// HUGE number of SPRiTES!
	if ((cv_limitdraw.value || maptol & TOL_NIGHTS)
	 && cv_grrenderquality.value != 3 && players[displayplayer].mo)
	{
		// Special function for precipitation Tails 08-18-2002
		for (thing = sec->thinglist; thing; thing = thing->snext)
		{
			if (!thing)
				continue;

			if (!(thing->flags2 & MF2_DONTDRAW))
			{
				adx = abs(players[displayplayer].mo->x - thing->x);
				ady = abs(players[displayplayer].mo->y - thing->y);

				// From _GG1_ p.428. Approx. eucledian distance fast.
				approx_dist = adx + ady - ((adx < ady ? adx : ady)>>1);

				if (approx_dist < LIMIT_DRAW_DIST)
					HWR_ProjectSprite(thing);
				else if (splitscreen && players[secondarydisplayplayer].mo)
				{
					adx = abs(players[secondarydisplayplayer].mo->x - thing->x);
					ady = abs(players[secondarydisplayplayer].mo->y - thing->y);

					// From _GG1_ p.428. Approx. eucledian distance fast.
					approx_dist = adx + ady - ((adx < ady ? adx : ady)>>1);

					if (approx_dist < LIMIT_DRAW_DIST)
						HWR_ProjectSprite(thing);
				}
			}
		}
	}
	else
	{
		// Handle all things in sector.
		for (thing = sec->thinglist; thing; thing = thing->snext)
		{
			if (!thing)
				continue;

			if (!(thing->flags2 & MF2_DONTDRAW))
				HWR_ProjectSprite(thing);

			if (cv_objectplace.value
			&& (!thing->flags2 & MF2_DONTDRAW))
				objectsdrawn++;

			if (!thing->snext)
				break;
		}
	}

#ifdef HWPRECIP
	if (playeringame[displayplayer] && players[displayplayer].mo)
	{
		for (precipthing = sec->preciplist; precipthing; precipthing = precipthing->snext)
		{
			if (!precipthing)
				continue;

			if (precipthing->invisible)
				continue;

			adx = abs(players[displayplayer].mo->x - precipthing->x);
			ady = abs(players[displayplayer].mo->y - precipthing->y);

			// From _GG1_ p.428. Approx. eucledian distance fast.
			approx_dist = adx + ady - ((adx < ady ? adx : ady)>>1);

			// Only draw the precipitation oh-so-far from the player.
			if (approx_dist < (cv_precipdist.value << FRACBITS))
				HWR_ProjectPrecipitationSprite(precipthing);
			else if (splitscreen && players[secondarydisplayplayer].mo)
			{
				adx = abs(players[secondarydisplayplayer].mo->x - precipthing->x);
				ady = abs(players[secondarydisplayplayer].mo->y - precipthing->y);

				// From _GG1_ p.428. Approx. eucledian distance fast.
				approx_dist = adx + ady - ((adx < ady ? adx : ady)>>1);

				if (approx_dist < (cv_precipdist.value << FRACBITS))
					HWR_ProjectPrecipitationSprite(precipthing);
			}
		}
	}
#endif
}

// --------------------------------------------------------------------------
// HWR_ProjectSprite
//  Generates a vissprite for a thing if it might be visible.
// --------------------------------------------------------------------------
// BP why not use xtoviexangle/viewangletox like in bsp ?....
static void HWR_ProjectSprite(mobj_t *thing)
{
	gr_vissprite_t *vis;
	float tr_x, tr_y;
	float tx, tz;
	float x1, x2;
	spritedef_t *sprdef;
	spriteframe_t *sprframe;
	size_t lumpoff;
	unsigned rot;
	boolean flip;
	angle_t ang;

	if (!thing)
		return;

	// transform the origin point
	tr_x = FIXED_TO_FLOAT(thing->x) - gr_viewx;
	tr_y = FIXED_TO_FLOAT(thing->y) - gr_viewy;

	// rotation around vertical axis
	tz = (tr_x * gr_viewcos) + (tr_y * gr_viewsin);

	// thing is behind view plane?
	if (tz < ZCLIP_PLANE)
		return;

	tx = (tr_x * gr_viewsin) - (tr_y * gr_viewcos);

	// decide which patch to use for sprite relative to player
	if ((unsigned)thing->sprite >= numsprites)
#ifdef RANGECHECK
		I_Error("HWR_ProjectSprite: invalid sprite number %i ",
		         thing->sprite);
#else
	{
		CONS_Printf("Warning: Mobj of type %i with invalid sprite data (%d) detected and removed.\n", thing->type, thing->sprite);
		if (thing->player)
			P_SetPlayerMobjState(thing, S_PLAY_STND);
		else
			P_SetMobjState(thing, S_DISS);
		return;
	}
#endif

	rot = thing->frame&FF_FRAMEMASK;

	//Fab : 02-08-98: 'skin' override spritedef currently used for skin
	if (thing->skin)
		sprdef = &((skin_t *)thing->skin)->spritedef;
	else
		sprdef = &sprites[thing->sprite];

	if (rot >= sprdef->numframes)
#ifdef RANGECHECK
		I_Error("HWR_ProjectSprite: invalid sprite frame %i : %i for %s",
		         thing->sprite, thing->frame, sprnames[thing->sprite]);
#else
	{
		CONS_Printf("Warning: Mobj of type %d with invalid sprite frame (%u/%s) of %s detected and removed.\n", thing->type, rot, sprdef->numframes, sprnames[thing->sprite]);
		if (thing->player)
			P_SetPlayerMobjState(thing, S_PLAY_STND);
		else
			P_SetMobjState(thing, S_DISS);
		return;
	}
#endif

	sprframe = &sprdef->spriteframes[thing->frame & FF_FRAMEMASK];

	if (!sprframe)
#ifdef PARANOIA //heretic hack
		I_Error("sprframes NULL for sprite %d\n", thing->sprite);
#else
		return;
#endif

	if (sprframe->rotate)
	{
		// choose a different rotation based on player view
		ang = R_PointToAngle(thing->x, thing->y); // uses viewx,viewy
		rot = (ang-thing->angle+ANGLE_202h)>>29;
		//Fab: lumpid is the index for spritewidth,spriteoffset... tables
		lumpoff = sprframe->lumpid[rot];
		flip = (boolean)sprframe->flip[rot];
	}
	else
	{
		// use single rotation for all views
		rot = 0;                        //Fab: for vis->patch below
		lumpoff = sprframe->lumpid[0];     //Fab: see note above
		flip = (boolean)sprframe->flip[0];
	}


	// calculate edges of the shape
	tx -= FIXED_TO_FLOAT(spritecachedinfo[lumpoff].offset);

	// project x
	x1 = gr_windowcenterx + (tx * gr_centerx / tz);

	//faB : tr_x doesnt matter
	// hurdler: it's used in cliptosolidsegs
	tr_x = x1;

	x1 = tx;

	tx += FIXED_TO_FLOAT(spritecachedinfo[lumpoff].width);
	x2 = gr_windowcenterx + (tx * gr_centerx / tz);

	//
	// store information in a vissprite
	//
	vis = HWR_NewVisSprite();
	vis->x1 = x1;
	vis->x2 = tx;
	vis->tz = tz;
	vis->patchlumpnum = sprframe->lumppat[rot];
	vis->flip = flip;
	vis->mobj = thing;

	//Hurdler: 25/04/2000: now support colormap in hardware mode
	if (thing->flags & MF_TRANSLATION)
	{
		// New colormap stuff for skins Tails 06-07-2002
#ifdef TRANSFIX
		if (vis->mobj->skin) // This thing is a player!
			vis->colormap = (UINT8 *)translationtables[(skin_t*)vis->mobj->skin-skins] - 256 + ((INT32)vis->mobj->color<<8);
#else
		if (vis->mobj->player) // This thing is a player!
			vis->colormap = (UINT8 *)translationtables[vis->mobj->player->skin] - 256 + ((INT32)vis->mobj->color<<8);
#endif
		else if ((vis->mobj->flags & MF_BOSS) && (vis->mobj->flags2 & MF2_FRET) && (leveltime & 1)) // Bosses "flash"
			vis->colormap = (UINT8 *)bosstranslationtables;
		else
			vis->colormap = (UINT8 *)defaulttranslationtables - 256 + ((INT32)vis->mobj->color<<8);
	}
	else
		vis->colormap = colormaps;

	// set top/bottom coords
	if (thing->flags & MF_HIRES)
		vis->ty = FIXED_TO_FLOAT(thing->z + spritecachedinfo[lumpoff].topoffset/2) - gr_viewz;
	else
		vis->ty = FIXED_TO_FLOAT(thing->z + spritecachedinfo[lumpoff].topoffset) - gr_viewz;

	//CONS_Printf("------------------\nH: sprite  : %d\nH: frame   : %x\nH: type    : %d\nH: sname   : %s\n\n",
	//            thing->sprite, thing->frame, thing->type, sprnames[thing->sprite]);

	if (thing->eflags & MFE_VERTICALFLIP)
		vis->vflip = true;
	else
		vis->vflip = false;

	vis->precip = false;
}

#ifdef HWPRECIP
// Precipitation projector for hardware mode
static void HWR_ProjectPrecipitationSprite(precipmobj_t *thing)
{
	gr_vissprite_t *vis;
	float tr_x, tr_y;
	float tx, tz;
	float x1, x2;
	spritedef_t *sprdef;
	spriteframe_t *sprframe;
	size_t lumpoff;
	unsigned rot = 0;
	boolean flip;

	// transform the origin point
	tr_x = FIXED_TO_FLOAT(thing->x) - gr_viewx;
	tr_y = FIXED_TO_FLOAT(thing->y) - gr_viewy;

	// rotation around vertical axis
	tz = (tr_x * gr_viewcos) + (tr_y * gr_viewsin);

	// thing is behind view plane?
	if (tz < ZCLIP_PLANE)
		return;

	tx = (tr_x * gr_viewsin) - (tr_y * gr_viewcos);

	// decide which patch to use for sprite relative to player
	if ((unsigned)thing->sprite >= numsprites)
#ifdef RANGECHECK
		I_Error("HWR_ProjectSprite: invalid sprite number %i ",
		        thing->sprite);
#else
		return;
#endif

	sprdef = &sprites[thing->sprite];

	if ((size_t)(thing->frame&FF_FRAMEMASK) >= sprdef->numframes)
#ifdef RANGECHECK
		I_Error("HWR_ProjectSprite: invalid sprite frame %i : %i for %s",
		        thing->sprite, thing->frame, sprnames[thing->sprite]);
#else
		return;
#endif

	sprframe = &sprdef->spriteframes[ thing->frame & FF_FRAMEMASK];

	// use single rotation for all views
	lumpoff = sprframe->lumpid[0];
	flip = (boolean)sprframe->flip[0];

	// calculate edges of the shape
	tx -= FIXED_TO_FLOAT(spritecachedinfo[lumpoff].offset);

	// project x
	x1 = gr_windowcenterx + (tx * gr_centerx / tz);

	//faB : tr_x doesnt matter
	// hurdler: it's used in cliptosolidsegs
	tr_x = x1;

	x1 = tx;

	tx += FIXED_TO_FLOAT(spritecachedinfo[lumpoff].width);
	x2 = gr_windowcenterx + (tx * gr_centerx / tz);

	//
	// store information in a vissprite
	//
	vis = HWR_NewVisSprite();
	vis->x1 = x1;
	vis->x2 = tx;
	vis->tz = tz;
	vis->patchlumpnum = sprframe->lumppat[rot];
	vis->flip = flip;
	vis->mobj = (mobj_t *)thing;

	vis->colormap = colormaps;

	// set top/bottom coords
	vis->ty = FIXED_TO_FLOAT(thing->z + spritecachedinfo[lumpoff].topoffset) - gr_viewz;

	vis->sectorlight = 0xff;
	vis->precip = true;
}
#endif

// ==========================================================================
//
// ==========================================================================
static void HWR_DrawSkyBackground(player_t *player)
{
	FOutVector v[4];
	angle_t angle;
	float f;

//  3--2
//  | /|
//  |/ |
//  0--1

	player = NULL;
	HWR_GetTexture(skytexture);

	//Hurdler: the sky is the only texture who need 4.0f instead of 1.0
	//         because it's called just after clearing the screen
	//         and thus, the near clipping plane is set to 3.99
	v[0].x = v[3].x = -4.0f;
	v[1].x = v[2].x =  4.0f;
	v[0].y = v[1].y = -4.0f;
	v[2].y = v[3].y =  4.0f;

	v[0].z = v[1].z = v[2].z = v[3].z = 4.0f;

	if (textures[skytexture]->width > 256)
		angle = (angle_t)((float)(dup_viewangle + gr_xtoviewangle[0])
						/((float)textures[skytexture]->width/256.0f))
							%(ANGLE_90-1);
	else
		angle = (dup_viewangle + gr_xtoviewangle[0])%(ANGLE_90-1);

	f = (float)((textures[skytexture]->width/2)
	            * FIXED_TO_FLOAT(finetangent[(2048
	 - ((INT32)angle>>(ANGLETOFINESHIFT + 1))) & FINEMASK]));

	v[0].sow = v[3].sow = 0.22f+(f)/(textures[skytexture]->width/2);
	v[2].sow = v[1].sow = 0.22f+(f+(127))/(textures[skytexture]->width/2);

	f = (float)((textures[skytexture]->height/2)
	            * FIXED_TO_FLOAT(finetangent[(2048
	 - ((INT32)aimingangle>>(ANGLETOFINESHIFT + 1))) & FINEMASK]));

	v[3].tow = v[2].tow = 0.22f+(f)/(textures[skytexture]->height/2);
	v[0].tow = v[1].tow = 0.22f+(f+(127))/(textures[skytexture]->height/2);

	HWD.pfnDrawPolygon(NULL, v, 4, 0);
}


// -----------------+
// HWR_ClearView : clear the viewwindow, with maximum z value
// -----------------+
static inline void HWR_ClearView(void)
{
	//  3--2
	//  | /|
	//  |/ |
	//  0--1

	/// \bug faB - enable depth mask, disable color mask

	HWD.pfnGClipRect((INT32)gr_viewwindowx,
	                 (INT32)gr_viewwindowy,
	                 (INT32)(gr_viewwindowx + gr_viewwidth),
	                 (INT32)(gr_viewwindowy + gr_viewheight),
	                 3.99f);
	HWD.pfnClearBuffer(false, true, 0);

	//disable clip window - set to full size
	// rem by Hurdler
	// HWD.pfnGClipRect(0, 0, vid.width, vid.height);
}


// -----------------+
// HWR_SetViewSize  : set projection and scaling values
// -----------------+
void HWR_SetViewSize(void)
{
	// setup view size
	gr_viewwidth = (float)vid.width;
	gr_viewheight = (float)vid.height;

	if (splitscreen)
		gr_viewheight /= 2;

	gr_centerx = gr_viewwidth / 2;
	gr_basecentery = gr_viewheight / 2; //note: this is (gr_centerx * gr_viewheight / gr_viewwidth)

	gr_viewwindowx = (vid.width - gr_viewwidth) / 2;
	gr_windowcenterx = (float)(vid.width / 2);
	if (gr_viewwidth == vid.width)
	{
		gr_baseviewwindowy = 0;
		gr_basewindowcentery = gr_viewheight / 2;               // window top left corner at 0,0
	}
	else
	{
		gr_baseviewwindowy = (vid.height-gr_viewheight) / 2;
		gr_basewindowcentery = (float)(vid.height / 2);
	}

	gr_pspritexscale = gr_viewwidth / BASEVIDWIDTH;
	gr_pspriteyscale = ((vid.height*gr_pspritexscale*BASEVIDWIDTH)/BASEVIDHEIGHT)/vid.width;
}

// ==========================================================================
//
// ==========================================================================
void HWR_RenderPlayerView(INT32 viewnumber, player_t *player)
{
	FRGBAFloat ClearColor;


	ClearColor.red = 0.0f;
	ClearColor.green = 0.0f;
	ClearColor.blue = 0.0f;
	ClearColor.alpha = 1.0f;

	if (viewnumber == 0) // Only do it if it's the first screen being rendered
		HWD.pfnClearBuffer(true, false, &ClearColor); // Clear the Color Buffer, stops HOMs. Also seems to fix the skybox issue on Intel

	const float fpov = FIXED_TO_FLOAT(cv_grfov.value+player->fovadd);
	{
		// do we really need to save player (is it not the same)?
		player_t *saved_player = stplyr;
		stplyr = player;
		ST_doPaletteStuff();
		stplyr = saved_player;
	}

	// note: sets viewangle, viewx, viewy, viewz
	R_SetupFrame(player);

	// copy view cam position for local use
	dup_viewx = viewx;
	dup_viewy = viewy;
	dup_viewz = viewz;
	dup_viewangle = viewangle;

	// set window position
	gr_centery = gr_basecentery;
	gr_viewwindowy = gr_baseviewwindowy;
	gr_windowcentery = gr_basewindowcentery;
	if (splitscreen && viewnumber == 1)
	{
		gr_viewwindowy += (vid.height/2);
		gr_windowcentery += (vid.height/2);
	}

	// check for new console commands.
	NetUpdate();

	gr_viewx = FIXED_TO_FLOAT(dup_viewx);
	gr_viewy = FIXED_TO_FLOAT(dup_viewy);
	gr_viewz = FIXED_TO_FLOAT(dup_viewz);
	gr_viewsin = FIXED_TO_FLOAT(viewsin);
	gr_viewcos = FIXED_TO_FLOAT(viewcos);

	gr_viewludsin = FIXED_TO_FLOAT(FINECOSINE(aimingangle>>ANGLETOFINESHIFT));
	gr_viewludcos = FIXED_TO_FLOAT(-FINESINE(aimingangle>>ANGLETOFINESHIFT));

	//04/01/2000: Hurdler: added for T&L
	//                     It should replace all other gr_viewxxx when finished
	atransform.anglex = (float)(aimingangle>>ANGLETOFINESHIFT)*(360.0f/(float)FINEANGLES);
	atransform.angley = (float)(viewangle>>ANGLETOFINESHIFT)*(360.0f/(float)FINEANGLES);
	atransform.x      = gr_viewx;  // FIXED_TO_FLOAT(viewx)
	atransform.y      = gr_viewy;  // FIXED_TO_FLOAT(viewy)
	atransform.z      = gr_viewz;  // FIXED_TO_FLOAT(viewz)
	atransform.scalex = 1;
	atransform.scaley = ORIGINAL_ASPECT;
	atransform.scalez = 1;
	atransform.fovxangle = fpov; // Tails
	atransform.fovyangle = fpov; // Tails
	atransform.splitscreen = splitscreen;
	gr_fovlud = (float)(1.0l/tan((double)(fpov*M_PIl/360l)));

	//------------------------------------------------------------------------
	HWR_ClearView();

	/* // I don't think this is ever used.
	if (cv_grfog.value)
	{
		camera_t *camview;

		HWR_FoggingOn(); // First of all, turn it on, set the default user settings too

		if (splitscreen && player == &players[secondarydisplayplayer])
			camview = &camera2;
		else
			camview = &camera;

		if (viewsector)// && player->mo->subsector->sector->extra_colormap)
		{
			if (viewsector->ffloors)
			{
				ffloor_t *rover;

				for (rover = viewsector->ffloors; rover; rover = rover->next)
				{
					if (!(rover->flags & FF_EXISTS) || !(rover->flags & FF_SWIMMABLE) || rover->flags & FF_SOLID)
						continue;

					if (*rover->topheight <= camview->z
						|| *rover->bottomheight > (camview->z + (camview->height >> 1)))
						continue;

					if (camview->z + (camview->height >> 1) < *rover->topheight)
					{
						UINT32 sectorcolormap; // RGBA value of the sector's colormap
						RGBA_t rgbcolor; // Convert the value from UINT32 to RGA_t
						UINT32 fogvalue; // convert the color to FOG from RGBA to RGB

						if (!(rover->master->frontsector->extra_colormap)) // See if there's a colormap in this FOF
							continue;

						sectorcolormap = rover->master->frontsector->extra_colormap->rgba;

						rgbcolor.rgba = sectorcolormap;

						fogvalue = (rgbcolor.s.red*0x10000)+(rgbcolor.s.green*0x100)+rgbcolor.s.blue;

						HWD.pfnSetSpecialState(HWD_SET_FOG_COLOR, fogvalue);
						HWD.pfnSetSpecialState(HWD_SET_FOG_DENSITY, cv_grfogdensity.value+128);

					}

				}
			}

		}
		else // no custom fog?
		{
			HWD.pfnSetSpecialState(HWD_SET_FOG_COLOR, atohex(cv_grfogcolor.string));
			HWD.pfnSetSpecialState(HWD_SET_FOG_DENSITY, cv_grfogdensity.value);
		}

	}
	else
		HWD.pfnSetSpecialState(HWD_SET_FOG_MODE, 0); // Turn it off
		*/

	if (drawsky)
		HWR_DrawSkyBackground(player);

	//Hurdler: it doesn't work in splitscreen mode
	drawsky = splitscreen;

	HWR_ClearSprites();

#ifdef SORTING
	drawcount = 0;
#endif
	HWR_ClearClipSegs();

	//04/01/2000: Hurdler: added for T&L
	//                     Actually it only works on Walls and Planes
	HWD.pfnSetTransform(&atransform);

	validcount++;

	HWR_RenderBSPNode((INT32)numnodes-1);

	// Make a viewangle int so we can render things based on mouselook
	if (player == &players[consoleplayer])
		viewangle = localaiming;
	else if (splitscreen && player == &players[secondarydisplayplayer])
		viewangle = localaiming2;

	// Handle stuff when you are looking farther up or down.
	if ((aimingangle || cv_grfov.value+player->fovadd > 90*FRACUNIT))
	{
		dup_viewangle += ANGLE_90;
		HWR_ClearClipSegs();
		HWR_RenderBSPNode((INT32)numnodes-1); //left

		dup_viewangle += ANGLE_90;
		if (((INT32)aimingangle > ANGLE_45 || (INT32)aimingangle<-ANGLE_45))
		{
			HWR_ClearClipSegs();
			HWR_RenderBSPNode((INT32)numnodes-1); //back
		}

		dup_viewangle += ANGLE_90;
		HWR_ClearClipSegs();
		HWR_RenderBSPNode((INT32)numnodes-1); //right

		dup_viewangle += ANGLE_90;
	}

	// Check for new console commands.
	NetUpdate();

	// Draw MD2 and sprites
#ifdef SORTING
	HWR_SortVisSprites();
#endif
	HWR_DrawMD2S();

	// Draw the sprites like it was done previously without T&L
	HWD.pfnSetTransform(NULL);
#ifdef SORTING
	HWR_DrawSprites();
#endif
#ifdef NEWCORONAS
	//Hurdler: they must be drawn before translucent planes, what about gl fog?
	HWR_DrawCoronas();
#endif

#ifdef SORTING
	if (numplanes || numwalls) //Hurdler: render 3D water and transparent walls after everything
	{
		HWR_CreateDrawNodes();
		HWD.pfnSetTransform(NULL);
	}
#else
	if (numfloors || numwalls)
	{
		HWD.pfnSetTransform(&atransform);
		if (numfloors)
			HWR_Render3DWater();
		if (numwalls)
			HWR_RenderTransparentWalls();
		HWD.pfnSetTransform(NULL);
	}
#endif

	// put it off for menus etc
	if (cv_grfog.value)
		HWD.pfnSetSpecialState(HWD_SET_FOG_MODE, 0);

#ifdef SHUFFLE
	HWR_DoPostProcessor();
#endif
	// Check for new console commands.
	NetUpdate();

	//------------------------------------------------------------------------


	// added by Hurdler for correct splitscreen
	// moved here by hurdler so it works with the new near clipping plane
	HWD.pfnGClipRect(0, 0, vid.width, vid.height, NZCLIP_PLANE);
}

// ==========================================================================
//                                                                        FOG
// ==========================================================================

/// \author faB

static UINT32 atohex(const char *s)
{
	INT32 iCol;
	const char *sCol;
	char cCol;
	INT32 i;

	if (strlen(s)<6)
		return 0;

	iCol = 0;
	sCol = s;
	for (i = 0; i < 6; i++, sCol++)
	{
		iCol <<= 4;
		cCol = *sCol;
		if (cCol >= '0' && cCol <= '9')
			iCol |= cCol - '0';
		else
		{
			if (cCol >= 'F')
				cCol -= 'a' - 'A';
			if (cCol >= 'A' && cCol <= 'F')
				iCol = iCol | (cCol - 'A' + 10);
		}
	}
	//CONS_Printf("col %x\n", iCol);
	return iCol;
}

static void HWR_FoggingOn(void)
{
	HWD.pfnSetSpecialState(HWD_SET_FOG_COLOR, atohex(cv_grfogcolor.string));
	HWD.pfnSetSpecialState(HWD_SET_FOG_DENSITY, cv_grfogdensity.value);
	HWD.pfnSetSpecialState(HWD_SET_FOG_MODE, 1);
}

// ==========================================================================
//                                                         3D ENGINE COMMANDS
// ==========================================================================


static void CV_grFov_OnChange(void)
{
	if ((netgame || multiplayer) && !cv_debug && cv_grfov.value != 90*FRACUNIT)
		CV_Set(&cv_grfov, cv_grfov.defaultvalue);
}

static void Command_GrStats_f(void)
{
	Z_CheckHeap(9875); // debug

	CONS_Printf("Patch info headers: %7s kb\n", sizeu1(Z_TagUsage(PU_HWRPATCHINFO)>>10));
	CONS_Printf("3D Texture cache  : %7s kb\n", sizeu1(Z_TagUsage(PU_HWRCACHE)>>10));
	CONS_Printf("Plane polygon     : %7s kb\n", sizeu1(Z_TagUsage(PU_HWRPLANE)>>10));
}


// **************************************************************************
//                                                            3D ENGINE SETUP
// **************************************************************************

// --------------------------------------------------------------------------
// Add hardware engine commands & consvars
// --------------------------------------------------------------------------
//added by Hurdler: console varibale that are saved
void HWR_AddCommands(void)
{
	CV_RegisterVar(&cv_grmd2);
	CV_RegisterVar(&cv_grfov);
	CV_RegisterVar(&cv_grfogdensity);
	CV_RegisterVar(&cv_grfiltermode);
	CV_RegisterVar(&cv_granisotropicmode);
	CV_RegisterVar(&cv_grcorrecttricks);
	CV_RegisterVar(&cv_grsolvetjoin);
}

static inline void HWR_AddEngineCommands(void)
{
	// engine state variables
	CV_RegisterVar(&cv_grclipwalls);

	// engine development mode variables
	// - usage may vary from version to version..
	CV_RegisterVar(&cv_grbeta);

	// engine commands
	COM_AddCommand("gr_stats", Command_GrStats_f);
}


// --------------------------------------------------------------------------
// Setup the hardware renderer
// --------------------------------------------------------------------------
void HWR_Startup(void)
{
	static boolean startupdone = false;

	// setup GLPatch_t scaling
	gr_patch_scalex = (float)(1.0f / vid.width);
	gr_patch_scaley = (float)(1.0f / vid.height);

	// initalze light lut translation
	InitLumLut();

	// do this once
	if (!startupdone)
	{
		CONS_Printf("HWR_Startup()\n");
		HWR_InitPolyPool();
		// add console cmds & vars
		HWR_AddEngineCommands();
		HWR_InitTextureCache();

		// for test water translucent surface
		doomwaterflat  = W_CheckNumForName("FWATER1");
		if (doomwaterflat == LUMPERROR) // if FWATER1 not found (in doom shareware)
			doomwaterflat = W_GetNumForName("WATER0");

		HWR_InitMD2();
	}

	if (rendermode == render_opengl)
		textureformat = patchformat = GR_RGBA;

	startupdone = true;
}


// --------------------------------------------------------------------------
// Free resources allocated by the hardware renderer
// --------------------------------------------------------------------------
void HWR_Shutdown(void)
{
	CONS_Printf("HWR_Shutdown()\n");
	HWR_FreeExtraSubsectors();
	HWR_FreePolyPool();
	HWR_FreeTextureCache();
}

void transform(float *cx, float *cy, float *cz)
{
	float tr_x,tr_y;
	// translation
	tr_x = *cx - gr_viewx;
	tr_y = *cz - gr_viewy;
//	*cy = *cy;

	// rotation around vertical y axis
	*cx = (tr_x * gr_viewsin) - (tr_y * gr_viewcos);
	tr_x = (tr_x * gr_viewcos) + (tr_y * gr_viewsin);

	//look up/down ----TOTAL SUCKS!!!--- do the 2 in one!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	tr_y = *cy - gr_viewz;

	*cy = (tr_x * gr_viewludcos) + (tr_y * gr_viewludsin);
	*cz = (tr_x * gr_viewludsin) - (tr_y * gr_viewludcos);

	//scale y before frustum so that frustum can be scaled to screen height
	*cy *= ORIGINAL_ASPECT * gr_fovlud;
	*cx *= gr_fovlud;
}


//Hurdler: 3D Water stuff
#define MAX_3DWATER 512

#ifndef SORTING
static void HWR_Add3DWater(lumpnum_t lumpnum, extrasubsector_t *xsub,
	fixed_t fixedheight, INT32 lightlevel, INT32 alpha, sector_t *FOFSector)
{
	if (!(numfloors % MAX_3DWATER))
	{
		planeinfo = Z_Realloc(planeinfo,
			(numfloors + MAX_3DWATER) * sizeof *planeinfo, PU_STATIC, NULL);
	}
	planeinfo[numfloors].fixedheight = fixedheight;
	planeinfo[numfloors].lightlevel = lightlevel;
	planeinfo[numfloors].lumpnum = lumpnum;
	planeinfo[numfloors].xsub = xsub;
	planeinfo[numfloors].alpha = alpha;
	planeinfo[numfloors].FOFSector = FOFSector;
	numfloors++;
}
#endif

#define DIST_PLANE(i) ABS(planeinfo[(i)].fixedheight-dup_viewz)

#if 0
static void HWR_QuickSortPlane(INT32 start, INT32 finish)
{
	INT32 left = start;
	INT32 right = finish;
	INT32 starterval = (INT32)((right+left)/2); //pick a starter

	planeinfo_t temp;

	//'sort of sort' the two halves of the data list about starterval
	while (right > left);
	{
		while (DIST_PLANE(left) < DIST_PLANE(starterval)) left++; //attempt to find a bigger value on the left
		while (DIST_PLANE(right) > DIST_PLANE(starterval)) right--; //attempt to find a smaller value on the right

		if (left < right) //if we haven't gone too far
		{
			//switch them
			M_Memcpy(&temp, &planeinfo[left], sizeof (planeinfo_t));
			M_Memcpy(&planeinfo[left], &planeinfo[right], sizeof (planeinfo_t));
			M_Memcpy(&planeinfo[right], &temp, sizeof (planeinfo_t));
			//move the bounds
			left++;
			right--;
		}
	}

	if (start < right) HWR_QuickSortPlane(start, right);
	if (left < finish) HWR_QuickSortPlane(left, finish);
}
#endif

#ifndef SORTING
static void HWR_Render3DWater(void)
{
	size_t i;

	//bubble sort 3D Water for correct alpha blending
	{
		boolean permut = true;
		while (permut)
		{
			size_t j;
			for (j = 0, permut= false; j < numfloors-1; j++)
			{
				if (ABS(planeinfo[j].fixedheight-dup_viewz) < ABS(planeinfo[j+1].fixedheight-dup_viewz))
				{
					planeinfo_t temp;
					M_Memcpy(&temp, &planeinfo[j+1], sizeof (planeinfo_t));
					M_Memcpy(&planeinfo[j+1], &planeinfo[j], sizeof (planeinfo_t));
					M_Memcpy(&planeinfo[j], &temp, sizeof (planeinfo_t));
					permut = true;
				}
			}
		}
	}
#if 0 //thanks epat, but it's goes looping forever on CTF map Silver Cascade Zone
	HWR_QuickSortPlane(0, numplanes-1);
#endif

	gr_frontsector = NULL; //Hurdler: gr_fronsector is no longer valid
	for (i = 0; i < numfloors; i++)
	{
		FBITFIELD PolyFlags = PF_Translucent | (planeinfo[i].alpha<<24);

		HWR_GetFlat(planeinfo[i].lumpnum);
		HWR_RenderPlane(NULL, planeinfo[i].xsub, planeinfo[i].fixedheight, PolyFlags, planeinfo[i].lightlevel, planeinfo[i].lumpnum,
			planeinfo[i].FOFSector, planeinfo[i].alpha, planeinfo[i].fogplane, planeinfo[i].planecolormap);
	}
	numfloors = 0;
}
#endif

static void HWR_AddTransparentWall(wallVert3D *wallVerts, FSurfaceInfo *pSurf, INT32 texnum, FBITFIELD blend, boolean fogwall, INT32 lightlevel, extracolormap_t *wallcolormap)
{
	if (!(numwalls % MAX_TRANSPARENTWALL))
	{
		wallinfo = Z_Realloc(wallinfo,
			(numwalls + MAX_TRANSPARENTWALL) * sizeof *wallinfo, PU_STATIC, NULL);
	}

	M_Memcpy(wallinfo[numwalls].wallVerts, wallVerts, sizeof (wallinfo[numwalls].wallVerts));
	M_Memcpy(&wallinfo[numwalls].Surf, pSurf, sizeof (FSurfaceInfo));
	wallinfo[numwalls].texnum = texnum;
	wallinfo[numwalls].blend = blend;
#ifdef SORTING
	wallinfo[numwalls].drawcount = drawcount++;
#endif
	wallinfo[numwalls].fogwall = fogwall;
	wallinfo[numwalls].lightlevel = lightlevel;
	wallinfo[numwalls].wallcolormap = wallcolormap;
	numwalls++;
}
#ifndef SORTING
static void HWR_RenderTransparentWalls(void)
{
	size_t i;

	/*{ // sorting is disbale for now, do it!
		INT32 permut = 1;
		while (permut)
		{
			INT32 j;
			for (j = 0, permut = 0; j < numwalls-1; j++)
			{
				if (ABS(wallinfo[j].fixedheight-dup_viewz) < ABS(wallinfo[j+1].fixedheight-dup_viewz))
				{
					wallinfo_t temp;
					M_Memcpy(&temp, &wallinfo[j+1], sizeof (wallinfo_t));
					M_Memcpy(&wallinfo[j+1], &wallinfo[j], sizeof (wallinfo_t));
					M_Memcpy(&wallinfo[j], &temp, sizeof (wallinfo_t));
					permut = 1;
				}
			}
		}
	}*/

	for (i = 0; i < numwalls; i++)
	{
		HWR_GetTexture(wallinfo[i].texnum);
		HWR_RenderWall(wallinfo[i].wallVerts, &wallinfo[i].Surf, wallinfo[i].blend, wallinfo[i].wall->fogwall, wallinfo[i].wall->lightlevel, wallinfo[i].wall->wallcolormap);
	}
	numwalls = 0;
}
#endif
static void HWR_RenderWall(wallVert3D   *wallVerts, FSurfaceInfo *pSurf, FBITFIELD blend, boolean fogwall, INT32 lightlevel, extracolormap_t *wallcolormap)
{
	FOutVector  trVerts[4];
	UINT8       i;
	FOutVector  *wv;

	// transform
	wv = trVerts;
	// it sounds really stupid to do this conversion with the new T&L code
	// we should directly put the right information in the right structure
	// wallVerts3D seems ok, doesn't need FOutVector
	// also remove the light copy
	for (i = 0; i < 4; i++, wv++, wallVerts++)
	{
		wv->sow = wallVerts->s;
		wv->tow = wallVerts->t;
		wv->x   = wallVerts->x;
		wv->y   = wallVerts->y;
		wv->z = wallVerts->z;
	}

	UINT8 alpha = pSurf->FlatColor.s.alpha; // retain the alpha

	// Lighting is done here instead so that fog isn't drawn incorrectly on transparent walls after sorting
	if (wallcolormap)
	{
		if (fogwall)
			pSurf->FlatColor.rgba = HWR_Lighting(LightLevelToLum(lightlevel), wallcolormap->rgba, wallcolormap->fadergba, true, false);
		else
			pSurf->FlatColor.rgba = HWR_Lighting(LightLevelToLum(lightlevel), wallcolormap->rgba, wallcolormap->fadergba, false, false);
	}
	else
	{
		if (fogwall)
			pSurf->FlatColor.rgba = HWR_Lighting(LightLevelToLum(lightlevel), NORMALFOG, FADEFOG, true, false);
		else
			pSurf->FlatColor.rgba = HWR_Lighting(LightLevelToLum(lightlevel), NORMALFOG, FADEFOG, false, false);
	}

	pSurf->FlatColor.s.alpha = alpha; // put the alpha back after lighting

	if (blend & PF_Environment)
		HWD.pfnDrawPolygon(pSurf, trVerts, 4, blend|PF_Modulated|PF_Clip|PF_Occlude); // PF_Occlude must be used for solid objects
	else
		HWD.pfnDrawPolygon(pSurf, trVerts, 4, blend|PF_Modulated|PF_Clip); // No PF_Occlude means overlapping (incorrect) transparency

#ifdef WALLSPLATS
	if (gr_curline->linedef->splats && cv_splats.value)
		HWR_DrawSegsSplats(pSurf);
#endif
}

void HWR_SetPaletteColor(INT32 palcolor)
{
	HWD.pfnSetSpecialState(HWD_SET_PALETTECOLOR, palcolor);
}

INT32 HWR_GetTextureUsed(void)
{
	return HWD.pfnGetTextureUsed();
}

#ifdef SHUFFLE
void HWR_DoPostProcessor(void)
{
	// Capture the screen for intermission and screen waving
	if(gamestate != GS_INTERMISSION)
		HWD.pfnMakeScreenTexture();

	if (postimgtype == postimg_water || postimgtype == postimg_heat)
	{
		// 10 by 10 grid. 2 coordinates (xy)
		float v[SCREENVERTS][SCREENVERTS][2];
		static double disStart = 0;
		UINT8 x, y;
		INT32 WAVELENGTH;
		INT32 AMPLITUDE;
		INT32 FREQUENCY;

		// Modifies the wave.
		if (postimgtype == postimg_water)
		{
			WAVELENGTH = 20; // Lower is longer
			AMPLITUDE = 20; // Lower is bigger
			FREQUENCY = 16; // Lower is faster
		}
		else
		{
			WAVELENGTH = 10; // Lower is longer
			AMPLITUDE = 30; // Lower is bigger
			FREQUENCY = 4; // Lower is faster
		}

		for (x = 0; x < SCREENVERTS; x++)
		{
			for (y = 0; y < SCREENVERTS; y++)
			{
				// Change X position based on its Y position.
				v[x][y][0] = (x/((float)(SCREENVERTS-1.0f)/9.0f))-4.5f + (float)sin((disStart+(y*WAVELENGTH))/FREQUENCY)/AMPLITUDE;
				v[x][y][1] = (y/((float)(SCREENVERTS-1.0f)/9.0f))-4.5f;
			}
		}
		HWD.pfnPostImgRedraw(v);
		disStart += 1;
	}
	else if (postimgtype == postimg_flip) //We like our screens inverted.
	{
		// 10 by 10 grid. 2 coordinates (xy)
		float v[SCREENVERTS][SCREENVERTS][2];
		UINT8 x, y;
		UINT8 flipy;

		for (x = 0; x < SCREENVERTS; x++)
		{
			for (y = 0, flipy = SCREENVERTS; y < SCREENVERTS; y++, flipy--)
			{
				// Flip the screen.
				v[x][y][0] = (x/((float)(SCREENVERTS-1.0f)/9.0f))-4.5f;
				v[x][y][1] = (flipy/((float)(SCREENVERTS-1.0f)/9.0f))-5.5f;
			}
		}
		HWD.pfnPostImgRedraw(v);
	}

	// Armageddon Blast Flash!
	// Could this even be considered postprocessor?
	if (stplyr->bonuscount)
	{
		FOutVector      v[4];
		INT32 flags;
		FSurfaceInfo Surf;

		// Overkill variables! :D
		v[0].x = v[2].y = v[3].x = v[3].y = -50.0f;
		v[0].y = v[1].x = v[1].y = v[2].x = 50.0f;
		v[0].z = v[1].z = v[2].z = v[3].z = 50.0f;

		flags = PF_Modulated | PF_Translucent | PF_Clip | PF_NoZClip | PF_NoDepthTest | PF_NoTexture;
		Surf.FlatColor.s.red = Surf.FlatColor.s.green = Surf.FlatColor.s.blue = 255;
		Surf.FlatColor.s.alpha = (UINT8)(stplyr->bonuscount*25);

		HWD.pfnDrawPolygon(&Surf, v, 4, flags);
	}
}

void HWR_StartScreenWipe(void)
{
/*	if(cv_debug)
//		CONS_Printf("In HWR_StartScreenWipe()\n");
*/
	HWD.pfnStartScreenWipe();
}

void HWR_EndScreenWipe(void)
{
	HWRWipeCounter = 1.0f;
	// Spammy!
	/*if(cv_debug)
	//	CONS_Printf("In HWR_EndScreenWipe()\n");
*/
	HWD.pfnEndScreenWipe();
}

// Prepare the screen for fading to black.
void HWR_PrepFadeToBlack(void)
{
	FOutVector      v[4];
	INT32 flags;
	FSurfaceInfo Surf;

	// Overkill variables! :D
	v[0].x = v[2].y = v[3].x = v[3].y = -50.0f;
	v[0].y = v[1].x = v[1].y = v[2].x = 50.0f;
	v[0].z = v[1].z = v[2].z = v[3].z = 50.0f;

	flags = PF_Modulated | PF_Translucent | PF_Clip | PF_NoZClip | PF_NoDepthTest | PF_NoTexture;
	Surf.FlatColor.s.red = Surf.FlatColor.s.green = Surf.FlatColor.s.blue = 0;
	Surf.FlatColor.s.alpha = 255;

	HWD.pfnDrawPolygon(&Surf, v, 4, flags);
}

void HWR_DrawIntermissionBG(void)
{
	HWD.pfnDrawIntermissionBG();
}

void HWR_DoScreenWipe(void)
{
	HWRWipeCounter -= 0.035f;
/*
	if(cv_debug)
		CONS_Printf("In HWR_DoScreenWipe(). Alpha =%f\n", HWRWipeCounter);
*/
	HWD.pfnDoScreenWipe(HWRWipeCounter);

	I_OsPolling();
	I_FinishUpdate();
}
#endif // SHUFFLE

#endif // HWRENDER
