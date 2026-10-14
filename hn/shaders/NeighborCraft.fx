// Composites Minecraft (NeighborCraft mod) into Hello Neighbor. The NeighborCraft UE4SS mod (which is also a ReShade
// add-on) uploads Minecraft's latest frame into the MCWORLD / MCDEPTH / MCOVERLAY textures and sets the uniforms.
// Adapted from universal-modder's minecraft-gta5-passthrough MCPassthrough.fx (MIT, see THIRD_PARTY_NOTICES.md).
//
// world: premultiplied RGBA, drawn where Minecraft is nearer than the host depth buffer (both in metres).
// overlay: Minecraft's hand, HUD and screens, always on top.
#include "ReShade.fxh"

texture MCWorldTex : MCWORLD;
texture MCDepthTex : MCDEPTH;
texture MCOverlayTex : MCOVERLAY;
texture MCHandTex : MCHAND;
sampler sWorld { Texture = MCWorldTex; AddressU = CLAMP; AddressV = CLAMP; };
sampler sDepth { Texture = MCDepthTex; MinFilter = POINT; MagFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; };
sampler sOverlay { Texture = MCOverlayTex; AddressU = CLAMP; AddressV = CLAMP; };
sampler sHand { Texture = MCHandTex; AddressU = CLAMP; AddressV = CLAMP; };

// x = near, y = far, z = flags (1: [0,1] depth, 2: rows bottom-up, 4: reversed Z). Set by the add-on.
uniform float3 McPlanes = float3(0.05, 2048.0, 7.0);
// Hello Neighbor's camera near/far clip. Set by the add-on.
uniform float2 HostPlanes = float2(0.1, 10000000.0);

// Set true by the add-on only while it has uploaded a Minecraft frame; until then Hello Neighbor passes through untouched.
uniform bool McActive = false;
// Set by the add-on: HN's pause menu is up: Minecraft's world stays, its overlay (its own pause screen) and hand don't.
uniform bool McPaused = false;
// Set by the add-on: HN's receivers are drawn only into its scene capture (hidden from the player's camera, so a stand-in
// a frame off Minecraft's picture can't show), and the capture is bound to HNRECV: their light is read from it.
uniform bool RecvActive = false;
texture HNRecvTex : HNRECV;
sampler sRecv { Texture = HNRecvTex; MinFilter = POINT; MagFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; };

uniform bool HostReversedZ < ui_label = "Host depth is reversed (UE4: yes)"; > = true;
uniform float DepthBias < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.005; ui_label = "Depth bias (m)";
	ui_tooltip = "How far behind Hello Neighbor's surface Minecraft may still show (blocks resting on the ground)."; > = 0.04;
uniform float SlopeBias < ui_type = "drag"; ui_min = 0.0; ui_max = 40.0; ui_step = 0.1; ui_label = "Depth bias per grazing slope";
	ui_tooltip = "Extra depth bias where Hello Neighbor's surface is seen at a grazing angle (its depth changes fast down the screen): Minecraft ground laid into Hello Neighbor's (lava, netherrack) still shows far away."; > = 0.0;
uniform float MaxBias < ui_type = "drag"; ui_min = 0.1; ui_max = 10.0; ui_step = 0.1; ui_label = "Largest depth bias (m)"; > = 3.0;
uniform float HostDepthScale < ui_type = "drag"; ui_min = 0.5; ui_max = 2.0; ui_step = 0.001; ui_label = "Host depth scale";
	ui_tooltip = "Calibration: multiplies Hello Neighbor's linearised depth."; > = 1.0;
uniform float LightMatch < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Match Hello Neighbor lighting";
	ui_tooltip = "Relight Minecraft by the (blurred) Hello Neighbor picture around it: darker in shade, tinted by nearby light."; > = 0.8;
uniform float LightReference < ui_type = "drag"; ui_min = 0.1; ui_max = 1.0; ui_step = 0.01; ui_label = "Neutral brightness";
	ui_tooltip = "Hello Neighbor brightness at which Minecraft keeps its own colours."; > = 0.42;
uniform float LightTint < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Light colour"; > = 0.55;
uniform float LightBlur < ui_type = "drag"; ui_min = 1.0; ui_max = 6.0; ui_step = 0.1; ui_label = "Light blur (mip)"; > = 3.2;
uniform float GradeMatch < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Match Hello Neighbor colour grade";
	ui_tooltip = "Give Minecraft the colour of Hello Neighbor's whole picture (a red Nether night, an orange sunset), as Hello Neighbor's own grading does to its world."; > = 0.55;
uniform float HazeStart < ui_type = "drag"; ui_min = 0.0; ui_max = 200.0; ui_step = 1.0; ui_label = "Haze start (m)"; > = 15.0;
uniform float HazeDistance < ui_type = "drag"; ui_min = 10.0; ui_max = 1000.0; ui_step = 1.0; ui_label = "Haze distance (m)";
	ui_tooltip = "Far away, Minecraft fades into the Hello Neighbor picture around it (Hello Neighbor's haze and fog don't reach Minecraft otherwise)."; > = 160.0;
uniform float HazeStrength < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Haze strength"; > = 0.85;
uniform float EdgeSoftness < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Soften Minecraft's edges";
	ui_tooltip = "Anti-alias where Minecraft meets Hello Neighbor (Hello Neighbor's own anti-aliasing ran before Minecraft was added)."; > = 1.0;
uniform float BloomStrength < ui_type = "drag"; ui_min = 0.0; ui_max = 2.0; ui_step = 0.01; ui_label = "Minecraft glow";
	ui_tooltip = "Bright Minecraft things (lava, fire, the portal) glow over Hello Neighbor's picture, as Hello Neighbor's own lights do."; > = 0.55;
uniform float BloomThreshold < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Glow threshold"; > = 0.62;
// Set by the add-on: the camera shake, applied to the finished picture so Hello Neighbor and Minecraft shake together
// (x, y: fraction of the screen height; z: roll, radians), and the nether portal's warp (0..1) while passing through.
uniform float3 Shake = float3(0.0, 0.0, 0.0);
uniform float PortalWarp = 0.0;
uniform float Timer < source = "timer"; >;
uniform int DebugView < ui_type = "combo"; ui_items = "Composite\0Host depth (1 m bands)\0Minecraft depth (1 m bands)\0Depth difference\0"; > = 0;
uniform bool Reproject < ui_label = "Re-project to the host camera"; ui_tooltip = "Rotate Minecraft's (slightly older) frame onto Hello Neighbor's current camera."; > = true;
uniform float PosePrediction < ui_type = "drag"; ui_min = -2.0; ui_max = 3.0; ui_step = 0.05; ui_label = "Pose prediction (frames)";
	ui_tooltip = "Extrapolate Hello Neighbor's camera rotation by this many frames before re-projecting."; > = 0.0;

// Set by the add-on: rows of R_mc^T R_gta (Hello Neighbor camera ray -> Minecraft camera ray), and
// x = tan(Hello Neighbor vertical fov / 2), y = tan(Minecraft vertical fov / 2), z = Minecraft aspect.
uniform float3 WarpRow0 = float3(1.0, 0.0, 0.0);
uniform float3 WarpRow1 = float3(0.0, 1.0, 0.0);
uniform float3 WarpRow2 = float3(0.0, 0.0, 1.0);
uniform float3 WarpTan = float3(0.7, 0.7, 1.7777);
// Set by the add-on: Hello Neighbor's camera position relative to Minecraft's, in Minecraft's camera space.
uniform float3 WarpT = float3(0.0, 0.0, 0.0);

// Hello Neighbor's picture at quarter size with mips: its blurred levels stand in for the light around each pixel.
texture HostLightTex { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = RGBA8; MipLevels = 7; };
sampler sHostLight { Texture = HostLightTex; AddressU = CLAMP; AddressV = CLAMP; };
// The same picture's brightness split by what it is (x: luma where HN's own surface is, y: 1 there; z: luma where a
// stand-in is, w: 1 there): how bright HN's real surfaces around a block are (surround_cap, hand_light) and the
// stand-ins' light, blurred over stand-ins only (received_luma).
texture HostSurroundTex { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = RGBA16F; MipLevels = 7; };
sampler sHostSurround { Texture = HostSurroundTex; AddressU = CLAMP; AddressV = CLAMP; };

// Set by the add-on: the colour of Hello Neighbor's sun (its directional light's LightColor), which tints Minecraft's
// surfaces. It used to be estimated from the picture, which is mostly the surfaces' own colours: blocks went green
// in front of grass and beige in front of sky, changing with where one looked.
uniform float3 HostLightColour = float3(1.0, 1.0, 1.0);
// Set by the add-on: the direction towards Hello Neighbor's sun in the camera Minecraft rendered with, and whether it
// shines (0: night, no sun). Minecraft's own fixed face shading is off (ClientLevelLightingMixin): each face is lit by
// its angle to this sun.
uniform float3 SunDirMc = float3(0.0, 1.0, 0.0);
uniform float SunStrength = 0.0;
uniform float ReceiverMatch < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Light from Hello Neighbor's stand-ins";
	ui_tooltip = "Where Hello Neighbor draws a grey stand-in under a Minecraft block or Steve, take the light it gets (its sun, its shadow edges)."; > = 1.0;
uniform float ReceiverReference < ui_type = "drag"; ui_min = 0.05; ui_max = 1.0; ui_step = 0.01; ui_label = "Stand-in brightness = full light";
	ui_tooltip = "How bright the grey stand-in looks where Minecraft should keep its own brightness (measured: ~0.88 in sun, ~0.5 in shade)."; > = 0.8;
uniform float SunContrast < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Sun on Minecraft's faces";
	ui_tooltip = "How much brighter Minecraft's faces turned to Hello Neighbor's sun are than those turned away."; > = 0.75;
uniform float SurroundReference < ui_type = "drag"; ui_min = 0.05; ui_max = 1.0; ui_step = 0.01; ui_label = "Surroundings brightness = full light";
	ui_tooltip = "Minecraft's light is never more than HN's own surfaces around it allow: the brightest one / this (0 = off)."; > = 0.3;

// The composited world (Hello Neighbor + Minecraft, no overlay) and, per pixel: Minecraft's coverage, its depth and Hello Neighbor's.
texture CompositeTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };
sampler sComposite { Texture = CompositeTex; AddressU = CLAMP; AddressV = CLAMP; };
texture McInfoTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA16F; };
sampler sMcInfo { Texture = McInfoTex; MinFilter = POINT; MagFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; };
// Minecraft's bright parts at quarter size with mips (the glow).
texture McBrightTex { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = RGBA16F; MipLevels = 6; };
sampler sMcBright { Texture = McBrightTex; AddressU = CLAMP; AddressV = CLAMP; };

float luma(float3 c)
{
	return dot(c, float3(0.2126, 0.7152, 0.0722));
}

/// The light's colour, normalised to luma 1 (Hello Neighbor's sun; white until the add-on sets it).
float3 light_colour()
{
	return HostLightColour / max(luma(HostLightColour), 1e-3);
}

float mc_linear(float d)
{
	const float n = McPlanes.x, f = McPlanes.y;
	if (d <= 0.0)
		return 1e9;
	return n * f / (n + d * (f - n));
}

float host_linear(float d)
{
	const float n = HostPlanes.x, f = HostPlanes.y;
	float z;
	if (HostReversedZ)
		z = d > 0.0 ? n * f / (n + d * (f - n)) : 1e9;
	else
		z = d < 1.0 ? n * f / (f - d * (f - n)) : 1e9;
	return z * HostDepthScale;
}

/// Hello Neighbor's picture for the light estimates, and its brightness with the stand-ins left out: a stand-in is where
/// HN's depth is Minecraft's (the frame as Minecraft drew it; a pixel of camera lag at an edge only blurs the average).
void PS_Light(float4 pos : SV_Position, float2 uv : TEXCOORD, out float4 light : SV_Target0, out float4 surround : SV_Target1)
{
	const float3 c = tex2D(ReShade::BackBuffer, uv).rgb;
	light = float4(c, 1.0);
	float own = 1.0;
	if (McActive)
	{
		const float2 ouv = float2(uv.x, 1.0 - uv.y);
		const float zm = mc_linear(tex2Dlod(sDepth, float4(ouv, 0, 0)).r);
		const float zh = host_linear(tex2Dlod(ReShade::DepthBuffer, float4(uv, 0, 0)).x);
		if (tex2Dlod(sWorld, float4(ouv, 0, 0)).a > 0.0 && abs(zh - zm) < 0.1 + 0.02 * zm)
			own = 0.0;
	}
	surround = float4(luma(c) * own, own, luma(c) * (1.0 - own), 1.0 - own);
}

/// The most light Minecraft's surface may take at this point: what HN's own surfaces within ~1.2 m of it (near in depth,
/// stand-ins left out) show, over SurroundReference; 1e3 (no limit) where too few of them are seen. Unreal lights the
/// movable stand-ins from its coarse indirect-light grid, which in a dark corridor still holds the light of the rooms
/// around it: in the user's nightmare corridor the stand-in read ~0.39 (outdoor shade: 0.46) while HN's walls and floor
/// there read 0.03-0.08, and blocks and Steve came out lit as if in daylight (outofplace.png).
float surround_cap(float2 uv, float zm)
{
	if (zm < 0.0 || SurroundReference <= 0.0)
		return 1e3;
	const float r = clamp(1.2 / (max(zm, 0.3) * 2.0 * WarpTan.x), 0.02, 0.35);
	// the brightest of them, not their average: Steve's (or a block's) own shadow covers much of the ground beside him in a
	// low sun, and the average put him in the dark in full sunlight (the user, 2026-10-13); one lit surface near means
	// light is there, and only where everything around is dark does the limit bite. A soft brightest (the 4th-power
	// mean), over coarse samples: the plain brightest of 12 points jumped from pixel to pixel as a bright spot came into
	// or left a pixel's points, a blotchy ("warbled") darkness over Steve in a dark room (the user, warbleddarkness2.png)
	float sum4 = 0.0, weight = 0.0, seen = 0.0;
	[unroll] for (int i = 0; i < 12; ++i)
	{
		const float a = i * 2.39996 + 0.7;
		const float rr = r * sqrt((i + 0.5) / 12.0);
		const float2 t = uv + float2(cos(a) * BUFFER_HEIGHT * BUFFER_RCP_WIDTH, sin(a)) * rr;
		const float zt = host_linear(tex2Dlod(ReShade::DepthBuffer, float4(t, 0, 2.0)).x);
		const float w = exp(-abs(zt - zm) / (0.5 + 0.1 * zm));
		const float2 s = tex2Dlod(sHostSurround, float4(t, 0, 3.0)).xy;
		const float l = s.x / max(s.y, 1e-3);
		sum4 += l * l * l * l * s.y * w;
		weight += s.y * w;
		seen += s.y * w;
	}
	const float cap = sqrt(sqrt(sum4 / max(weight, 1e-4))) / SurroundReference;
	return lerp(1e3, cap, saturate(seen / 2.0));
}

/// The light of the room around the camera, for Minecraft's first-person hand (nothing of HN's is beside it on screen):
/// HN's own surfaces within a few metres of the camera, over the whole picture, the soft brightest of them (as
/// surround_cap) over SurroundReference; 1e3 (no limit) when nothing of HN's is that near (outdoors, open ground far).
/// The hand used to take neutral light everywhere: full brightness in a dark room (the user, 2026-10-13).
float hand_light()
{
	if (SurroundReference <= 0.0)
		return 1e3;
	float sum4 = 0.0, weight = 0.0;
	[unroll] for (int i = 0; i < 6; ++i)
		[unroll] for (int j = 0; j < 4; ++j)
		{
			const float2 t = float2(0.08 + 0.168 * i, 0.12 + 0.25 * j);
			const float zt = host_linear(tex2Dlod(ReShade::DepthBuffer, float4(t, 0, 3.0)).x);
			const float w = saturate((6.0 - zt) / 3.0); // 0-3 m: fully, out to 6 m: less
			const float2 s = tex2Dlod(sHostSurround, float4(t, 0, 4.0)).xy;
			const float l = s.x / max(s.y, 1e-3);
			sum4 += l * l * l * l * s.y * w;
			weight += s.y * w;
		}
	const float cap = sqrt(sqrt(sum4 / max(weight, 1e-4))) / SurroundReference;
	return lerp(1e3, cap, saturate(weight / 3.0));
}

/// The stand-in's brightness here, blurred a little over stand-in pixels only: Unreal's shadow maps speckle on Steve's
/// small boxes, and a single pixel's reading passed that on; where too little of a stand-in is around, the pixel itself.
float received_luma(float2 uv, float3 host)
{
	const float2 s = tex2Dlod(sHostSurround, float4(uv, 0, 1.0)).zw;
	return lerp(luma(host), s.x / max(s.y, 1e-3), saturate(s.y * 2.0 - 0.4));
}

/// The light on a receiver from HN's scene capture of them (RecvActive), as the screen's brightness, or -1 where the
/// capture has no receiver at Minecraft's surface. The capture is the same view (HN's camera, its field of view) in
/// HDR (RGB) with its depth in centimetres (A): a receiver is where that depth is Minecraft's. HDR to the screen: HN's
/// own surfaces within ~1.2 m around it are in both pictures (where the capture's depth is the screen's), and their
/// brightness on screen over their brightness in the capture is the exposure and tone curve at this brightness.
float capture_received(float2 uv, float zm)
{
	const float4 c = tex2Dlod(sRecv, float4(uv, 0, 0));
	if (abs(c.a * 0.01 - zm) > 0.06 + 0.01 * zm)
		return -1.0;
	const float r = clamp(1.2 / (max(zm, 0.3) * 2.0 * WarpTan.x), 0.02, 0.35);
	float screen = 0.0, hdr = 0.0;
	[unroll] for (int i = 0; i < 12; ++i)
	{
		const float a = i * 2.39996 + 0.7;
		const float rr = r * sqrt((i + 0.5) / 12.0);
		const float2 t = uv + float2(cos(a) * BUFFER_HEIGHT * BUFFER_RCP_WIDTH, sin(a)) * rr;
		const float4 ct = tex2Dlod(sRecv, float4(t, 0, 0));
		const float zt = host_linear(tex2Dlod(ReShade::DepthBuffer, float4(t, 0, 0)).x);
		if (abs(ct.a * 0.01 - zt) < 0.05 + 0.01 * zt) // the same HN surface in both (not a receiver, not covered)
		{
			screen += luma(tex2Dlod(ReShade::BackBuffer, float4(t, 0, 0)).rgb);
			hdr += luma(ct.rgb);
		}
	}
	if (hdr <= 1e-5)
		return -1.0;
	return luma(c.rgb) * screen / hdr;
}

/// Hello Neighbor's light where a Minecraft surface is: from Hello Neighbor's surfaces near it in depth (the floor a block
/// stands on, the wall beside it), never from the picture behind it. It used to be the blurred picture at the Minecraft
/// pixel itself, which is what lies behind the block or hand: a block in front of a bright window came out bright, in
/// front of a dark doorway dark, the hand took the colour of whatever was behind it, and colour that follows the
/// background reads as see-through or reflective. Twelve taps on a disc ~0.6 m wide around it (golden-angle spiral),
/// each weighted by how close its Hello Neighbor depth is to the Minecraft surface's; with too few near surfaces (the
/// hand, a block against the sky) neutral light instead.
float3 host_light_near(float2 uv, float zm)
{
	// with nothing of HN's near it: neutral light (gain 1, no colour). It was the picture's average, which is whatever
	// fills the screen: the hand and a wall in front of a lawn went green, in front of the sky bright and beige,
	// changing as one looked up and down (the user)
	const float3 global = LightReference;
	if (zm < 0.0)
		return global; // the first-person hand: nothing of HN's is beside it (HN's held object, 25 cm off, is not its light)
	const float r = clamp(0.6 / (max(zm, 0.3) * 2.0 * WarpTan.x), 0.01, 0.25); // 0.6 m as a fraction of the screen height
	float3 sum = 0.0;
	float weight = 0.0;
	[unroll] for (int i = 0; i < 12; ++i)
	{
		const float a = i * 2.39996 + 0.3;
		const float rr = r * sqrt((i + 0.5) / 12.0);
		const float2 t = uv + float2(cos(a) * BUFFER_HEIGHT * BUFFER_RCP_WIDTH, sin(a)) * rr;
		const float zt = host_linear(tex2Dlod(ReShade::DepthBuffer, float4(t, 0, 0)).x);
		const float w = exp(-abs(zt - zm) / (0.25 + 0.1 * zm));
		sum += tex2Dlod(sHostLight, float4(t, 0, 1.5)).rgb * w;
		weight += w;
	}
	return lerp(global, sum / max(weight, 1e-4), saturate(weight / 3.0));
}

/// Minecraft renders at full daylight, so treat its colour as albedo and light it with Hello Neighbor's light near it; then give it
/// Hello Neighbor's colour grade (the whole picture's colour) and, far away, Hello Neighbor's haze (fade into the picture around it).
float3 relight(float3 c, float2 uv, float zm, float ndl)
{
	const float3 L = host_light_near(uv, zm);
	const float lum = luma(L);
	float gain = clamp(lum / LightReference, 0.42, 1.3);
	// the face's angle to the sun (ndl: its normal against the sun's direction; < -1.5: unknown), where the sun reaches:
	// Hello Neighbor's surfaces around it are lit (in a shadow or indoors they aren't, and neither is the face). A face
	// turned to the sun up to ~1.25x, one turned away down to ~0.65x of the light around it
	if (ndl > -1.5 && SunStrength > 0.0)
	{
		const float sunlit = SunStrength * saturate((lum / LightReference - 0.55) / 0.45);
		gain *= lerp(1.0, lerp(1.0 - 0.35 * SunContrast, 1.0 + 0.25 * SunContrast, saturate(ndl)), sunlit);
	}
	gain = min(gain, zm < 0.0 ? max(hand_light(), 0.12) : surround_cap(uv, zm)); // never brighter than HN's own surfaces around it allow
	// the light's colour: the whole picture's grey surfaces (a sunset's orange, a night's blue), never the surfaces
	// around the block. Around a block, the colour is mostly theirs (a lawn's green, a red house's red), and taken as
	// the light's it painted blocks with their surroundings (the user's cobblestone: green below, pink above); a local
	// estimate from grey surfaces only still fell back to the grass where nothing grey was near. The light's
	// strength stays local (gain, above): shade and bright spots still show on blocks.
	const float3 tint = clamp(lerp(1.0, light_colour(), LightTint), 0.55, 1.5);
	c = lerp(c, c * gain * tint, LightMatch);
	// the picture's grade, gentled (Hello Neighbor's own grade keeps some colour in everything) and kept off lights (bright
	// things keep their colour: fire, lava, the portal)
	// (the grade's colour from the picture's grey surfaces too, not its average colour: a screen full of green isn't a
	// green grade)
	const float3 grade = 1.0; // no colour grade from the picture (it is the surfaces' colours, not a grade)
	c = lerp(c, c * grade, GradeMatch * (1.0 - saturate((max(c.r, max(c.g, c.b)) - 0.45) * 2.5)));
	const float h = zm < 0.0 ? 0.0 : saturate((1.0 - exp(-max(zm - HazeStart, 0.0) / HazeDistance)) * HazeStrength);
	const float3 H = tex2Dlod(sHostLight, float4(uv, 0, LightBlur + 1.0)).rgb;
	return lerp(c, H, h);
}


float3 bands(float z)
{
	const float t = frac(z);
	return lerp(float3(0.1, 0.1, 0.1), float3(1.0, 0.85, 0.3), step(0.5, t)) * saturate(1.5 - z / 100.0);
}

void PS_Composite(float4 pos : SV_Position, float2 uv : TEXCOORD, out float4 outColor : SV_Target0, out float4 outInfo : SV_Target1)
{
	const float3 host = tex2D(ReShade::BackBuffer, uv).rgb;
	outInfo = 0.0;
	outColor = float4(host, 1.0);
	if (!McActive)
		return;
	const float2 ouv = float2(uv.x, 1.0 - uv.y); // Minecraft's rows are bottom-up

	// Where this Hello Neighbor pixel's view ray lands in Minecraft's frame.
	float2 muv = ouv;
	bool inside = true;
	const float zh = host_linear(tex2Dlod(ReShade::DepthBuffer, float4(uv, 0, 0)).x);
	// how far behind Hello Neighbor's surface Minecraft may still show: more where that surface is seen at a grazing angle
	const float allow = min(DepthBias + SlopeBias * abs(ddy(zh)), max(MaxBias, DepthBias));
	float zm = 1e9;
	if (Reproject)
	{
		// March this Hello Neighbor pixel's ray (in Hello Neighbor's current camera) from near to far, moving each point into the
		// camera Minecraft rendered with (rotation + how far the camera moved since). The first point that is
		// behind Minecraft's surface there is where Minecraft is seen: refine it and sample. Thin, close things
		// (Steve in third person) are found even when the camera has swung sideways.
		const float3 ray = float3((uv.x * 2.0 - 1.0) * WarpTan.x * BUFFER_WIDTH * BUFFER_RCP_HEIGHT, (1.0 - uv.y * 2.0) * WarpTan.x, -1.0);
		const float2 mcScale = float2(WarpTan.y * WarpTan.z, WarpTan.y);
		const float zNear = 0.2, zFar = max(min(zh + allow, 400.0), 0.5);
		float2 ndc = 0.0;
		inside = false;
		[loop] for (int i = 0; i < 24; ++i)
		{
			const float z = zNear * pow(zFar / zNear, i / 23.0);
			const float3 p = ray * z;
			const float3 pm = float3(dot(WarpRow0, p), dot(WarpRow1, p), dot(WarpRow2, p)) + WarpT;
			if (pm.z > -1e-3)
				continue;
			const float2 n = pm.xy / -pm.z / mcScale;
			if (any(abs(n) > 1.0))
				continue;
			const float zmv = mc_linear(tex2Dlod(sDepth, float4(n * 0.5 + 0.5, 0, 0)).r);
			if (zmv > -pm.z + 0.03)
				continue; // still in front of whatever Minecraft drew there
			// crossed Minecraft's surface: settle on it (fixed-point on the surface point's Hello Neighbor depth)
			float zg = z;
			ndc = n;
			[loop] for (int k = 0; k < 3; ++k)
			{
				const float3 pk = ray * zg;
				const float3 mk = float3(dot(WarpRow0, pk), dot(WarpRow1, pk), dot(WarpRow2, pk)) + WarpT;
				if (mk.z > -1e-3)
					break;
				const float2 nk = mk.xy / -mk.z / mcScale;
				if (any(abs(nk) > 1.0))
					break;
				const float zk = mc_linear(tex2Dlod(sDepth, float4(nk * 0.5 + 0.5, 0, 0)).r);
				if (zk > 1e8)
					break;
				ndc = nk;
				const float3 q = float3(nk * mcScale, -1.0) * zk - WarpT;
				zg = -(q.x * WarpRow0.z + q.y * WarpRow1.z + q.z * WarpRow2.z);
			}
			muv = ndc * 0.5 + 0.5;
			zm = zg;
			inside = true;
			break;
		}
	}
	const float4 world = inside ? tex2D(sWorld, muv) : 0.0;
	if (!Reproject)
		zm = mc_linear(tex2D(sDepth, muv).r);

	outInfo = float4(0.0, 0.0, zh, 0.0);
	if (DebugView == 1)
	{
		outColor = float4(bands(zh), 1.0);
		return;
	}
	if (DebugView == 2)
	{
		outColor = float4(world.a > 0.0 ? bands(zm) : host * 0.3, 1.0);
		return;
	}
	if (DebugView == 3)
	{
		outColor = float4(world.a > 0.0 ? float3(saturate((zh - zm) * 0.5 + 0.5), saturate(-(zh - zm) * 0.5 + 0.5), 0.0) : host * 0.3, 1.0);
		return;
	}

	const float visible = zm < zh + allow ? 1.0 : 0.0;
	const float cover = world.a * visible;
	// the face's normal, from Minecraft's depth around this point (the neighbour on the side with the nearer depth, so
	// a block's edge doesn't bend it), in Minecraft's camera space; facing the camera
	float ndl = -2.0;
	if (cover > 0.0)
	{
		const float2 texel = 1.0 / tex2Dsize(sDepth, 0);
		const float2 mcScale = float2(WarpTan.y * WarpTan.z, WarpTan.y);
		const float3 pc = float3((muv * 2.0 - 1.0) * mcScale, -1.0) * mc_linear(tex2Dlod(sDepth, float4(muv, 0, 0)).r);
		const float2 ur = muv + float2(texel.x, 0), ul = muv - float2(texel.x, 0), uu = muv + float2(0, texel.y), ud = muv - float2(0, texel.y);
		const float3 pr = float3((ur * 2.0 - 1.0) * mcScale, -1.0) * mc_linear(tex2Dlod(sDepth, float4(ur, 0, 0)).r);
		const float3 pl = float3((ul * 2.0 - 1.0) * mcScale, -1.0) * mc_linear(tex2Dlod(sDepth, float4(ul, 0, 0)).r);
		const float3 pu = float3((uu * 2.0 - 1.0) * mcScale, -1.0) * mc_linear(tex2Dlod(sDepth, float4(uu, 0, 0)).r);
		const float3 pd = float3((ud * 2.0 - 1.0) * mcScale, -1.0) * mc_linear(tex2Dlod(sDepth, float4(ud, 0, 0)).r);
		const float3 dx = abs(pr.z - pc.z) < abs(pc.z - pl.z) ? pr - pc : pc - pl;
		const float3 dy = abs(pu.z - pc.z) < abs(pc.z - pd.z) ? pu - pc : pc - pd;
		float3 n = cross(dx, dy);
		if (dot(n, n) > 1e-12 && abs(pc.z) < 1e8)
		{
			n = normalize(n);
			if (dot(n, pc) > 0.0)
				n = -n;
			ndl = dot(n, normalize(SunDirMc));
		}
	}
	float3 lit = relight(world.rgb, uv, zm, ndl);
	// Hello Neighbor's own light on this very point: where Hello Neighbor's surface is at Minecraft's depth, it is the
	// grey stand-in HN draws under blocks and models (bridge.cpp receiver_material), lit by HN's sun with HN's shadows:
	// a roof's shadow edge crosses the block as it crosses the ground. Its brightness only (a stand-in can coincide
	// with a coloured HN surface: a block sunk level with the grass); the colour stays the sun's
	// the receiver's light: from HN's scene capture when the receivers are drawn only there, else from the screen where
	// HN's surface is at Minecraft's depth (the receiver drawn in the player's view)
	const float captured = RecvActive && cover > 0.0 ? capture_received(uv, zm) : -1.0;
	if (cover > 0.0 && ReceiverMatch > 0.0 && (captured >= 0.0 || (!RecvActive && abs(zh - zm) < 0.06 + 0.01 * zm)))
	{
		const float3 tint = clamp(lerp(1.0, light_colour(), LightTint), 0.55, 1.5);
		// compressed (^0.6): the grey stand-in under dim indoor light came out much darker than HN's own bright-textured
		// walls beside it (the user: Steve too dark in a lit room); edges keep their contrast, dim light lifts
		// and never more than HN's own surfaces around it show (surround_cap: the stand-in's light can be the indirect
		// grid's, brighter than the room)
		const float g = min(clamp(pow(max((captured >= 0.0 ? captured : received_luma(uv, host)) / ReceiverReference, 0.0), 0.6), 0.2, 1.35), surround_cap(uv, zm));
		const float3 received = world.rgb * tint * g;
		const float h = saturate((1.0 - exp(-max(zm - HazeStart, 0.0) / HazeDistance)) * HazeStrength);
		lit = lerp(lit, lerp(received, tex2Dlod(sHostLight, float4(uv, 0, LightBlur + 1.0)).rgb, h), ReceiverMatch);
	}
	outColor = float4(lit * visible + host * (1.0 - cover), 1.0);
	outInfo = float4(cover, cover > 0.0 ? zm : 0.0, zh, 0.0);
}

/// Minecraft's bright parts (lava, fire, the portal, blazes): what glows.
float4 PS_Bright(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	const float cover = tex2D(sMcInfo, uv).x;
	const float3 c = tex2D(sComposite, uv).rgb;
	const float l = luma(c);
	return float4(c * (cover * saturate((l - BloomThreshold) / max(1.0 - BloomThreshold, 1e-3)) / max(l, 1e-3)), 1.0);
}

/// The finished picture: the world (shaken as one: Hello Neighbor and Minecraft together), Minecraft's edges softened,
/// Minecraft's glow, the portal's warp, and Minecraft's hand and HUD on
/// top (neither shaken nor warped).
float3 PS_Final(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	if (!McActive)
		return tex2D(ReShade::BackBuffer, uv).rgb;
	const float aspect = BUFFER_WIDTH * BUFFER_RCP_HEIGHT;
	const float t = Timer * 0.001;
	// shake: rotate and move the picture a little, zoomed in just enough that no edge shows
	const float amount = length(Shake.xy) + abs(Shake.z) * 0.5;
	const float zoom = 1.0 + amount * 2.2 + PortalWarp * 0.06;
	float2 d = (uv - 0.5) * float2(aspect, 1.0) / zoom;
	// the portal: a swirl and waves, strongest in the middle
	if (PortalWarp > 0.0)
	{
		const float r = length(d);
		const float swirl = PortalWarp * 1.1 * exp(-r * 2.2) * sin(t * 1.7 + 1.0);
		const float cs = cos(swirl), sn = sin(swirl);
		d = float2(d.x * cs - d.y * sn, d.x * sn + d.y * cs);
		d += PortalWarp * 0.014 * float2(sin(d.y * 38.0 + t * 7.0), cos(d.x * 31.0 + t * 6.0));
	}
	const float cr = cos(Shake.z), sr = sin(Shake.z);
	d = float2(d.x * cr - d.y * sr, d.x * sr + d.y * cr) + Shake.xy;
	const float2 wuv = d / float2(aspect, 1.0) + 0.5;

	const float2 px = BUFFER_PIXEL_SIZE;
	float3 color = tex2D(sComposite, wuv).rgb;
	float4 info = tex2D(sMcInfo, wuv);
	// the stand-ins HN draws under Minecraft's blocks and models (receivers) only show as light: where one peeks past
	// Minecraft's picture (an edge's anti-aliasing, a frame of lag in a turn), this pixel is HN's surface at the depth
	// of a Minecraft surface right next to it: it takes that Minecraft pixel's colour (within 3 px)
	if (info.x < 0.5 && info.z < 200.0)
	{
		[unroll] for (int r = 1; r <= 3; ++r)
		{
			[unroll] for (int k = 0; k < 8; ++k)
			{
				const float a = k * 0.785398;
				const float2 o = float2(cos(a), sin(a)) * r * px;
				const float4 n = tex2Dlod(sMcInfo, float4(wuv + o, 0, 0));
				// from at the Minecraft surface to one block behind it: a stand-in's top face seen nearly edge-on recedes up
				// to a block behind the front face beside it, and the old +-5 cm left it as a pale line along the
				// top edge (the user, 2026-10-13)
				const float dz = info.z - n.y;
				if (info.x < 0.5 && n.x > 0.5 && dz > -0.05 - 0.01 * info.z && dz < 1.05 + 0.02 * info.z)
				{
					color = tex2Dlod(sComposite, float4(wuv + o, 0, 0)).rgb;
					info.x = 1.0;
				}
			}
		}
	}
	// soften where Minecraft meets Hello Neighbor (Hello Neighbor's anti-aliasing never saw Minecraft)
	if (EdgeSoftness > 0.0)
	{
		const float c1 = tex2D(sMcInfo, wuv + float2(px.x, 0)).x, c2 = tex2D(sMcInfo, wuv - float2(px.x, 0)).x;
		const float c3 = tex2D(sMcInfo, wuv + float2(0, px.y)).x, c4 = tex2D(sMcInfo, wuv - float2(0, px.y)).x;
		const float edge = saturate(abs(c1 - info.x) + abs(c2 - info.x) + abs(c3 - info.x) + abs(c4 - info.x));
		if (edge > 0.0)
		{
			const float3 n = tex2D(sComposite, wuv + float2(px.x, 0)).rgb + tex2D(sComposite, wuv - float2(px.x, 0)).rgb
				+ tex2D(sComposite, wuv + float2(0, px.y)).rgb + tex2D(sComposite, wuv - float2(0, px.y)).rgb;
			color = lerp(color, (color * 2.0 + n) / 6.0, edge * EdgeSoftness);
		}
	}
	// (no screen-space "contact shadow" any more: it darkened Hello Neighbor's pixels next to Minecraft's in the picture
	// and showed as a dark silhouette around blocks; shadows are Hello Neighbor's own now, cast by shadow-only
	// stand-ins in its engine, see bridge.cpp spawn_shadow_box)
	// Minecraft's glow
	if (BloomStrength > 0.0)
	{
		const float3 b = tex2Dlod(sMcBright, float4(wuv, 0, 1.0)).rgb * 0.5 + tex2Dlod(sMcBright, float4(wuv, 0, 2.5)).rgb * 0.8
			+ tex2Dlod(sMcBright, float4(wuv, 0, 4.0)).rgb;
		color += b * BloomStrength;
	}
	// the portal's purple
	if (PortalWarp > 0.0)
	{
		const float r = length((uv - 0.5) * float2(aspect, 1.0));
		const float3 purple = color * float3(0.72, 0.38, 1.25) + float3(0.12, 0.0, 0.22) * (0.6 + 0.4 * sin(t * 3.0 + r * 9.0));
		color = lerp(color, purple, saturate(PortalWarp * (0.55 + r * 0.6)));
	}
	// the first-person hand: lit like Minecraft's world in the scene (the host's light around it, its colour grade),
	// so it darkens indoors and takes the room's colour; its own layer, so the GUI above stays as it is
	const float4 hand = McPaused ? 0.0 : tex2D(sHand, float2(uv.x, 1.0 - uv.y));
	if (hand.a > 0.0)
		color = relight(hand.rgb, uv, -1.0, -2.0) + saturate(color) * (1.0 - hand.a);
	const float4 overlay = McPaused ? 0.0 : tex2D(sOverlay, float2(uv.x, 1.0 - uv.y)); // HUD and screens are screen-space: never shaken
	return overlay.rgb + saturate(color) * (1.0 - overlay.a);
}

technique NeighborCraft < ui_tooltip = "Minecraft passthrough: enabled automatically while the Minecraft link is up."; >
{
	pass Light
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_Light;
		RenderTarget0 = HostLightTex;
		RenderTarget1 = HostSurroundTex;
	}
	pass Composite
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_Composite;
		RenderTarget0 = CompositeTex;
		RenderTarget1 = McInfoTex;
	}
	pass Bright
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_Bright;
		RenderTarget = McBrightTex;
	}
	pass Final
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_Final;
	}
}
