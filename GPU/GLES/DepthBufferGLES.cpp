// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <algorithm>

#include "Common/GPU/OpenGL/GLFeatures.h"
#include "Core/ConfigValues.h"
#include "Core/Reporting.h"
#include "GPU/Common/GPUStateUtils.h"
#include "GPU/GLES/DrawEngineGLES.h"
#include "GPU/GLES/FramebufferManagerGLES.h"
#include "GPU/GLES/ShaderManagerGLES.h"
#include "GPU/GLES/TextureCacheGLES.h"

static const char *depth_dl_fs = R"(
#ifdef GL_ES
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
#endif
#if __VERSION__ >= 130
#define varying in
#define texture2D texture
#define gl_FragColor fragColor0
out vec4 fragColor0;
#endif
varying vec2 v_texcoord;
uniform vec4 u_depthFactor;
uniform vec4 u_depthShift;
uniform vec4 u_depthTo8;
uniform sampler2D tex;
void main() {
  float depth = texture2D(tex, v_texcoord).r;
  // At this point, clamped maps [0, 1] to [0, 65535].
  float clamped = clamp((depth + u_depthFactor.x) * u_depthFactor.y, 0.0, 1.0);

  vec4 enc = u_depthShift * clamped;
  enc = floor(mod(enc, 256.0)) * u_depthTo8;
  // Let's ignore the bits outside 16 bit precision.
  gl_FragColor = enc.yzww;
}
)";

static const char *depth_vs = R"(
#ifdef GL_ES
precision highp float;
#endif
#if __VERSION__ >= 130
#define attribute in
#define varying out
#endif
attribute vec2 a_position;
varying vec2 v_texcoord;
void main() {
  v_texcoord = a_position * 2.0;
  gl_Position = vec4(v_texcoord * 2.0 - vec2(1.0, 1.0), 0.0, 1.0);
}
)";

struct DepthUB {
	float u_depthFactor[4];
	float u_depthShift[4];
	float u_depthTo8[4];
};

const UniformBufferDesc depthUBDesc{ sizeof(DepthUB), {
	{ "u_depthFactor", -1, -1, UniformType::FLOAT4, 0 },
	{ "u_depthShift", -1, -1, UniformType::FLOAT4, 16 },
	{ "u_depthTo8", -1, -1, UniformType::FLOAT4, 32 },
} };

static bool SupportsDepthTexturing() {
	if (gl_extensions.IsGLES) {
		return gl_extensions.OES_packed_depth_stencil && (gl_extensions.OES_depth_texture || gl_extensions.GLES3);
	}
	return gl_extensions.VersionGEThan(3, 0);
}

bool FramebufferManagerGLES::ReadbackDepthbufferSync(Draw::Framebuffer *fbo, int x, int y, int w, int h, uint16_t *pixels, int pixelsStride) {
	using namespace Draw;

	if (!fbo) {
		ERROR_LOG_REPORT_ONCE(vfbfbozero, SCEGE, "ReadbackDepthbufferSync: bad fbo");
		return false;
	}
	// Old desktop GL can download depth, but not upload.
	if (gl_extensions.IsGLES && !SupportsDepthTexturing()) {
		return false;
	}

	// Pixel size always 4 here because we always request float or RGBA.
	const u32 bufSize = w * h * 4;
	if (!convBuf_ || convBufSize_ < bufSize) {
		delete[] convBuf_;
		convBuf_ = new u8[bufSize];
		convBufSize_ = bufSize;
	}

	const bool useColorPath = gl_extensions.IsGLES;
	bool format16Bit = false;

	if (useColorPath) {
		if (!depthReadbackPipeline_) {
			const ShaderLanguageDesc &shaderLanguageDesc = draw_->GetShaderLanguageDesc();

			ShaderModule *depthReadbackFs = draw_->CreateShaderModule(ShaderStage::Fragment, shaderLanguageDesc.shaderLanguage, (const uint8_t *)depth_dl_fs, strlen(depth_dl_fs), "depth_dl_fs");
			ShaderModule *depthReadbackVs = draw_->CreateShaderModule(ShaderStage::Vertex, shaderLanguageDesc.shaderLanguage, (const uint8_t *)depth_vs, strlen(depth_vs), "depth_vs");
			_assert_(depthReadbackFs && depthReadbackVs);

			InputLayoutDesc desc = {
				{
					{ 8, false },
				},
				{
					{ 0, SEM_POSITION, DataFormat::R32G32_FLOAT, 0 },
				},
			};
			InputLayout *inputLayout = draw_->CreateInputLayout(desc);

			BlendState *blendOff = draw_->CreateBlendState({ false, 0xF });
			DepthStencilState *stencilIgnore = draw_->CreateDepthStencilState({});
			RasterState *rasterNoCull = draw_->CreateRasterState({});

			PipelineDesc depthReadbackDesc{
				Primitive::TRIANGLE_LIST,
				{ depthReadbackVs, depthReadbackFs },
				inputLayout, stencilIgnore, blendOff, rasterNoCull, &depthUBDesc,
			};
			depthReadbackPipeline_ = draw_->CreateGraphicsPipeline(depthReadbackDesc, "depth_dl");
			_assert_(depthReadbackPipeline_);

			rasterNoCull->Release();
			blendOff->Release();
			stencilIgnore->Release();
			inputLayout->Release();

			depthReadbackFs->Release();
			depthReadbackVs->Release();

			SamplerStateDesc descNearest{};
			depthReadbackSampler_ = draw_->CreateSamplerState(descNearest);
		}

		shaderManager_->DirtyLastShader();
		auto *blitFBO = GetTempFBO(TempFBO::COPY, fbo->Width(), fbo->Height());
		draw_->BindFramebufferAsRenderTarget(blitFBO, { RPAction::DONT_CARE, RPAction::DONT_CARE, RPAction::DONT_CARE }, "ReadbackDepthbufferSync");
		Draw::Viewport viewport = { 0.0f, 0.0f, (float)fbo->Width(), (float)fbo->Height(), 0.0f, 1.0f };
		draw_->SetViewports(1, &viewport);

		draw_->BindFramebufferAsTexture(fbo, TEX_SLOT_PSP_TEXTURE, FB_DEPTH_BIT, 0);
		draw_->BindSamplerStates(TEX_SLOT_PSP_TEXTURE, 1, &depthReadbackSampler_);

		// We must bind the program after starting the render pass.
		draw_->SetScissorRect(0, 0, w, h);
		draw_->BindPipeline(depthReadbackPipeline_);

		DepthUB ub{};

		if (!gstate_c.Supports(GPU_SUPPORTS_ACCURATE_DEPTH)) {
			// Don't scale anything, since we're not using factors outside accurate mode.
			ub.u_depthFactor[0] = 0.0f;
			ub.u_depthFactor[1] = 1.0f;
		} else {
			const float factor = DepthSliceFactor();
			ub.u_depthFactor[0] = -0.5f * (factor - 1.0f) * (1.0f / factor);
			ub.u_depthFactor[1] = factor;
		}
		static constexpr float shifts[] = { 16777215.0f, 16777215.0f / 256.0f, 16777215.0f / 65536.0f, 16777215.0f / 16777216.0f };
		memcpy(ub.u_depthShift, shifts, sizeof(shifts));
		static constexpr float to8[] = { 1.0f / 255.0f, 1.0f / 255.0f, 1.0f / 255.0f, 1.0f / 255.0f };
		memcpy(ub.u_depthTo8, to8, sizeof(to8));

		draw_->UpdateDynamicUniformBuffer(&ub, sizeof(ub));

		// Fullscreen triangle coordinates.
		static const float positions[6] = {
			0.0, 0.0,
			1.0, 0.0,
			0.0, 1.0,
		};
		draw_->DrawUP(positions, 3);

		draw_->CopyFramebufferToMemorySync(blitFBO, FB_COLOR_BIT, x, y, w, h, DataFormat::R8G8B8A8_UNORM, convBuf_, w, "ReadbackDepthbufferSync");

		textureCache_->ForgetLastTexture();
		// TODO: Use 4444 so we can copy lines directly (instead of 32 -> 16 on CPU)?
		format16Bit = true;
	} else {
		draw_->CopyFramebufferToMemorySync(fbo, FB_DEPTH_BIT, x, y, w, h, DataFormat::D32F, convBuf_, w, "ReadbackDepthbufferSync");
		format16Bit = false;
	}

	if (format16Bit) {
		// In this case, we used the shader to apply depth scale factors.
		uint16_t *dest = pixels;
		const u32_le *packed32 = (u32_le *)convBuf_;
		for (int yp = 0; yp < h; ++yp) {
			for (int xp = 0; xp < w; ++xp) {
				dest[xp] = packed32[xp] & 0xFFFF;
			}
			dest += pixelsStride;
			packed32 += w;
		}
	} else {
		// TODO: Apply this in the shader?  May have precision issues if it becomes important to match.
		// We downloaded float values directly in this case.
		uint16_t *dest = pixels;
		const GLfloat *packedf = (GLfloat *)convBuf_;
		DepthScaleFactors depthScale = GetDepthScaleFactors();
		for (int yp = 0; yp < h; ++yp) {
			for (int xp = 0; xp < w; ++xp) {
				float scaled = depthScale.Apply(packedf[xp]);
				if (scaled <= 0.0f) {
					dest[xp] = 0;
				} else if (scaled >= 65535.0f) {
					dest[xp] = 65535;
				} else {
					dest[xp] = (int)scaled;
				}
			}
			dest += pixelsStride;
			packedf += w;
		}
	}

	gstate_c.Dirty(DIRTY_ALL_RENDER_STATE);
	return true;
}
