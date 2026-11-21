/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#version 450

layout(push_constant) uniform pc
{
	ivec4 rgb_rect;
	ivec4 a_rect;
	vec4 scale;
	vec4 bias;
	// Active encoded width/height, excluding decoder padding.
	ivec4 encoded_size;
};

#ifdef VERT_SHADER

layout (location = 0) in vec2 vPosition;
layout (location = 1) in uvec2 vUV;

layout(location = 0) out vec4 outUV;

void main()
{
	gl_Position = vec4(vPosition, 0.0, 1.0);
	vec2 uv = vUV;
	outUV.xy = (uv + rgb_rect.xy) / rgb_rect.zw;
	outUV.zw = (uv + a_rect.xy) / a_rect.zw;
}
#endif

#ifdef FRAG_SHADER

layout(constant_id = 0) const int alpha = 1;
layout(constant_id = 1) const bool do_srgb = false;
layout(constant_id = 2) const bool manual_rgb = false;
layout(constant_id = 3) const bool manual_a = false;

layout(set = 0, binding = 0) uniform sampler2D rgb[alpha + 1];

layout(location = 0) in vec4 inUV;

layout(location = 0) out vec4 outColor;

float sRGB_to_linear(float x)
{
	if (x <= 0.04045)
		return x / 12.92;
	return pow((x + 0.055) / 1.055, 2.4);
}

vec4 sRGB_to_linear_rgba(vec4 x)
{
	return vec4(
	        sRGB_to_linear(x.r),
	        sRGB_to_linear(x.g),
	        sRGB_to_linear(x.b),
	        x.a);
}

// Use sample instructions rather than texelFetch: decoded YCbCr images require
// their immutable conversion sampler. Interpolate the converted RGB values when
// the decoder uses nearest filtering, without requiring YCbCr linear support.
vec4 sample_linear(sampler2D image, vec2 uv, vec2 size, vec2 first, vec2 last, bool manual_linear)
{
	vec2 p = clamp(uv * size - 0.5, first, last);
	if (!manual_linear)
		return textureLod(image, (p + 0.5) / size, 0.0);

	vec2 base = floor(p);
	vec2 f = fract(p);
	vec2 lo = (max(base, first) + 0.5) / size;
	vec2 hi = (min(base + 1.0, last) + 0.5) / size;
	return mix(
		mix(textureLod(image, lo, 0.0), textureLod(image, vec2(hi.x, lo.y), 0.0), f.x),
		mix(textureLod(image, vec2(lo.x, hi.y), 0.0), textureLod(image, hi, 0.0), f.x), f.y);
}

void main()
{
	vec2 first = vec2(rgb_rect.xy);
	outColor = sample_linear(rgb[0], inUV.xy, vec2(rgb_rect.zw), first, first + vec2(encoded_size.xy) - 1.0, manual_rgb);
	if (alpha == 1)
	{
		// Alpha is half resolution, with both eyes packed side by side.
		// Clamp to this eye's active texels, excluding decoder padding.
		vec2 a_first = vec2(a_rect.xy) * 0.5;
		outColor.a = sample_linear(rgb[1], inUV.zw, vec2(a_rect.zw) * 0.5,
		                           a_first, a_first + vec2(encoded_size.xy) * 0.5 - 1.0, manual_a).r;
	}

	if (do_srgb)
		outColor = sRGB_to_linear_rgba(outColor);

	if (alpha == 0)
	{
		outColor = outColor * scale + bias;
	}
	else if (outColor.a < 0.02)
	{
		outColor = vec4(0);
	}
	else
	{
		outColor.rgb /= outColor.a;
		outColor = outColor * scale + bias;
		outColor.rgb *= outColor.a;
	}
}

#endif
