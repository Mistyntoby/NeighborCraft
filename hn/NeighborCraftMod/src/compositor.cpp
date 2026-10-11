#include "compositor.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <d3d11.h>
#include <reshade.hpp>

using namespace reshade::api;

namespace
{
	constexpr const wchar_t *kMappingName = L"Local\\NeighborCraftFrame";
	constexpr uint32_t kMagic = 0x5450434D; // "MCPT"
	constexpr int kHeader = 4096;
	constexpr int kSlotDesc = 256;
	constexpr int kSlotDescBytes = 128;
	constexpr const char *kEffect = "NeighborCraft.fx";

	std::atomic<bool> g_registered{false};
	std::atomic<bool> g_active{false};
	std::atomic<float> g_hostNear{0.15f};
	std::atomic<float> g_hostFar{10000.0f};
	std::atomic<uint32_t> g_bbWidth{0}, g_bbHeight{0};
	std::atomic<bool> g_cameraLocked{false};
	std::atomic<float> g_lookLight{-1.0f}, g_lookBias{-1.0f}, g_lookSlope{-1.0f};
	std::atomic<float> g_shakeX{0.0f}, g_shakeY{0.0f}, g_shakeRoll{0.0f}, g_portalWarp{0.0f};
	std::atomic<float> g_lightR{1.0f}, g_lightG{1.0f}, g_lightB{1.0f};
	float g_savedLight = -1.0f, g_savedBias = -1.0f, g_savedSlope = -1.0f;

	struct Pose
	{
		float yaw = 0, pitch = 0, roll = 0, fov = 70;
		double x = 0, y = 0, z = 0;
		bool valid = false;
	};
	std::mutex g_poseLock;
	// GTA's recent camera poses, newest last. The script reads the camera GTA is about to render (the next frame),
	// while the picture being presented was rendered with the one before: re-project to that (g_poseLag back).
	Pose g_hostPoses[4];
	unsigned g_hostPoseCount = 0;
	std::atomic<int> g_poseLag{0}; // measured: 0 matches best (third and first person)
	Pose g_mcPose;

	HANDLE g_mapping = nullptr;
	const uint8_t *g_view = nullptr;
	int32_t g_slots = 0;
	int64_t g_stride = 0;
	int64_t g_lastPublish = -1;
	DWORD g_nextOpenAttempt = 0;

	struct Layer
	{
		resource tex = {0};
		resource_view srv = {0};
	};
	Layer g_world, g_depth, g_overlay, g_hand;
	uint32_t g_width = 0, g_height = 0;
	bool g_hasFrame = false;
	float g_mcNear = 0.05f, g_mcFar = 2048.0f;
	int32_t g_mcFlags = 7;

	template <typename T>
	T read(const uint8_t *p)
	{
		T v;
		std::memcpy(&v, p, sizeof(T));
		return v;
	}

	bool open_mapping()
	{
		if (g_view != nullptr)
			return true;
		if (GetTickCount() < g_nextOpenAttempt)
			return false;
		g_nextOpenAttempt = GetTickCount() + 1000;
		g_mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, kMappingName);
		if (g_mapping == nullptr)
			return false;
		const auto *header = static_cast<const uint8_t *>(MapViewOfFile(g_mapping, FILE_MAP_READ, 0, 0, kHeader));
		if (header == nullptr || read<uint32_t>(header) != kMagic)
		{
			if (header != nullptr)
				UnmapViewOfFile(header);
			CloseHandle(g_mapping);
			g_mapping = nullptr;
			return false;
		}
		g_slots = read<int32_t>(header + 12);
		g_stride = read<int64_t>(header + 16);
		UnmapViewOfFile(header);
		g_view = static_cast<const uint8_t *>(MapViewOfFile(g_mapping, FILE_MAP_READ, 0, 0, static_cast<SIZE_T>(kHeader + g_stride * g_slots)));
		if (g_view == nullptr)
		{
			CloseHandle(g_mapping);
			g_mapping = nullptr;
			return false;
		}
		reshade::log::message(reshade::log::level::info, "NeighborCraft: connected to Minecraft's frame export");
		return true;
	}

	void destroy_layers(device *dev)
	{
		for (Layer *layer : {&g_world, &g_depth, &g_overlay, &g_hand})
		{
			if (layer->srv.handle != 0)
				dev->destroy_resource_view(layer->srv);
			if (layer->tex.handle != 0)
				dev->destroy_resource(layer->tex);
			*layer = Layer();
		}
		g_width = g_height = 0;
		g_hasFrame = false;
	}

	bool create_layer(device *dev, Layer &layer, uint32_t w, uint32_t h, format fmt)
	{
		if (!dev->create_resource(
				resource_desc(w, h, 1, 1, fmt, 1, memory_heap::default_, resource_usage::shader_resource | resource_usage::copy_dest),
				nullptr, resource_usage::shader_resource, &layer.tex))
			return false;
		return dev->create_resource_view(layer.tex, resource_usage::shader_resource, resource_view_desc(fmt), &layer.srv);
	}

	void bind(effect_runtime *runtime)
	{
		runtime->update_texture_bindings("MCWORLD", g_world.srv, g_world.srv);
		runtime->update_texture_bindings("MCDEPTH", g_depth.srv, g_depth.srv);
		runtime->update_texture_bindings("MCOVERLAY", g_overlay.srv, g_overlay.srv);
		runtime->update_texture_bindings("MCHAND", g_hand.srv, g_hand.srv);
	}

	/// Upload the newest published Minecraft frame, if there is one we haven't shown yet.
	void upload(effect_runtime *runtime)
	{
		const int64_t published = read<int64_t>(g_view + 32);
		if (published == g_lastPublish)
			return;
		const int32_t slot = read<int32_t>(g_view + 40);
		if (slot < 0 || slot >= g_slots)
			return;
		const uint8_t *desc = g_view + kSlotDesc + kSlotDescBytes * slot;
		const int64_t seq = read<int64_t>(desc);
		if (seq & 1)
			return;
		const uint32_t w = read<uint32_t>(desc + 24), h = read<uint32_t>(desc + 28);
		if (w == 0 || h == 0)
			return;
		device *dev = runtime->get_device();
		if (w != g_width || h != g_height)
		{
			destroy_layers(dev);
			if (!create_layer(dev, g_world, w, h, format::r8g8b8a8_unorm) ||
				!create_layer(dev, g_depth, w, h, format::r32_float) ||
				!create_layer(dev, g_overlay, w, h, format::r8g8b8a8_unorm) ||
				!create_layer(dev, g_hand, w, h, format::r8g8b8a8_unorm))
			{
				destroy_layers(dev);
				return;
			}
			g_width = w;
			g_height = h;
			bind(runtime);
		}
		const uint8_t *base = g_view + kHeader + g_stride * slot;
		const size_t layer = size_t(w) * h * 4;
		subresource_data data;
		data.row_pitch = w * 4;
		data.slice_pitch = static_cast<uint32_t>(layer);
		data.data = const_cast<uint8_t *>(base);
		dev->update_texture_region(data, g_world.tex, 0);
		data.data = const_cast<uint8_t *>(base + layer);
		dev->update_texture_region(data, g_depth.tex, 0);
		data.data = const_cast<uint8_t *>(base + 2 * layer);
		dev->update_texture_region(data, g_overlay.tex, 0);
		data.data = const_cast<uint8_t *>(base + 3 * layer);
		dev->update_texture_region(data, g_hand.tex, 0);
		if (read<int64_t>(desc) != seq)
			return; // Minecraft rewrote the slot mid-copy: show the next one instead
		g_lastPublish = published;
		g_mcNear = read<float>(desc + 32);
		g_mcFar = read<float>(desc + 36);
		g_mcFlags = read<int32_t>(desc + 44);
		g_mcPose.fov = read<float>(desc + 40);
		g_mcPose.yaw = read<float>(desc + 72);
		g_mcPose.pitch = read<float>(desc + 76);
		g_mcPose.roll = read<float>(desc + 80);
		g_mcPose.x = read<double>(desc + 48);
		g_mcPose.y = read<double>(desc + 56);
		g_mcPose.z = read<double>(desc + 64);
		g_mcPose.valid = true;
		g_hasFrame = true;
	}

	/// Camera-to-world rotation, as Minecraft builds it: rotationYXZ(pi - yaw, -pitch, roll) (camera looks down -z).
	void camera_rotation(const Pose &p, float m[3][3])
	{
		const float d2r = 3.14159265f / 180.0f;
		const float a = 3.14159265f - p.yaw * d2r, b = -p.pitch * d2r, c = p.roll * d2r;
		const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b), cc = std::cos(c), sc = std::sin(c);
		// Ry(a) * Rx(b) * Rz(c)
		const float ry[3][3] = {{ca, 0, sa}, {0, 1, 0}, {-sa, 0, ca}};
		const float rx[3][3] = {{1, 0, 0}, {0, cb, -sb}, {0, sb, cb}};
		const float rz[3][3] = {{cc, -sc, 0}, {sc, cc, 0}, {0, 0, 1}};
		float t[3][3] = {};
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
				for (int k = 0; k < 3; ++k)
					t[i][j] += ry[i][k] * rx[k][j];
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
			{
				m[i][j] = 0;
				for (int k = 0; k < 3; ++k)
					m[i][j] += t[i][k] * rz[k][j];
			}
	}

	/// Rows of R_mc^T * R_host: turns a ray in GTA's camera space into Minecraft's camera space.
	void warp_matrix(const Pose &host, const Pose &mc, float out[3][3])
	{
		float rh[3][3], rm[3][3];
		camera_rotation(host, rh);
		camera_rotation(mc, rm);
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
			{
				out[i][j] = 0;
				for (int k = 0; k < 3; ++k)
					out[i][j] += rm[k][i] * rh[k][j];
			}
	}

	/// Reload MCPassthrough.fx when the file changes (ReShade doesn't watch it), so tweaks don't need a GTA restart.
	void watch_effect_file(effect_runtime *runtime)
	{
		static DWORD next = 0;
		static FILETIME last = {};
		static wchar_t path[MAX_PATH] = {};
		if (GetTickCount() < next)
			return;
		next = GetTickCount() + 1000;
		if (path[0] == 0)
		{
			GetModuleFileNameW(nullptr, path, MAX_PATH);
			wchar_t *slash = wcsrchr(path, L'\\');
			if (slash)
				wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"reshade-shaders\\Shaders\\NeighborCraft.fx");
		}
		WIN32_FILE_ATTRIBUTE_DATA info;
		if (!GetFileAttributesExW(path, GetFileExInfoStandard, &info))
			return;
		if (last.dwLowDateTime != 0 && CompareFileTime(&info.ftLastWriteTime, &last) != 0)
			runtime->reload_effect_next_frame(kEffect);
		last = info.ftLastWriteTime;
	}

	// ---- Minecraft under HN's UI ---------------------------------------------------------------------------------
	// ReShade renders effects at present, on top of everything, so Minecraft covered HN's pause menu, its blur and the
	// level fades. Instead, the effects run when HN starts drawing its UI: Unreal draws the finished 3D frame into the
	// back buffer with one full-screen pass, then the UI (Slate/UMG) into the same back buffer. So the first draw into
	// the back buffer is the scene; at the second one the effects run (Minecraft composited into the scene), and the
	// UI then draws over them. A frame with no UI draw falls back to ReShade's own rendering at present.

	effect_runtime *g_runtime = nullptr;
	resource_view g_boundRtv = {0};
	bool g_boundIsBackBuffer = false;
	bool g_sceneDrawn = false;
	bool g_renderedThisFrame = false;
	int g_backBufferDraws = 0; // diagnostics

	/// Everything a D3D11 draw depends on: Unreal's RHI caches the state it set, so what ReShade's passes change must be
	/// put back exactly (same idea as the state backup in Dear ImGui's D3D11 backend).
	struct D3D11StateBackup
	{
		UINT scissorCount = 0, viewportCount = 0;
		D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
		D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
		ID3D11RasterizerState *rs = nullptr;
		ID3D11BlendState *blend = nullptr;
		FLOAT blendFactor[4] = {};
		UINT sampleMask = 0;
		ID3D11DepthStencilState *depth = nullptr;
		UINT stencilRef = 0;
		ID3D11RenderTargetView *rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
		ID3D11DepthStencilView *dsv = nullptr;
		ID3D11ShaderResourceView *psSrvs[16] = {}, *vsSrvs[16] = {};
		ID3D11SamplerState *psSamplers[16] = {};
		ID3D11Buffer *psCbs[14] = {}, *vsCbs[14] = {};
		ID3D11PixelShader *ps = nullptr;
		ID3D11VertexShader *vs = nullptr;
		ID3D11GeometryShader *gs = nullptr;
		D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		ID3D11Buffer *ib = nullptr, *vbs[4] = {};
		UINT vbStrides[4] = {}, vbOffsets[4] = {};
		DXGI_FORMAT ibFormat = DXGI_FORMAT_UNKNOWN;
		UINT ibOffset = 0;
		ID3D11InputLayout *layout = nullptr;

		void capture(ID3D11DeviceContext *c)
		{
			scissorCount = viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
			c->RSGetScissorRects(&scissorCount, scissors);
			c->RSGetViewports(&viewportCount, viewports);
			c->RSGetState(&rs);
			c->OMGetBlendState(&blend, blendFactor, &sampleMask);
			c->OMGetDepthStencilState(&depth, &stencilRef);
			c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
			c->PSGetShaderResources(0, 16, psSrvs);
			c->VSGetShaderResources(0, 16, vsSrvs);
			c->PSGetSamplers(0, 16, psSamplers);
			c->PSGetConstantBuffers(0, 14, psCbs);
			c->VSGetConstantBuffers(0, 14, vsCbs);
			c->PSGetShader(&ps, nullptr, nullptr);
			c->VSGetShader(&vs, nullptr, nullptr);
			c->GSGetShader(&gs, nullptr, nullptr);
			c->IAGetPrimitiveTopology(&topology);
			c->IAGetIndexBuffer(&ib, &ibFormat, &ibOffset);
			c->IAGetVertexBuffers(0, 4, vbs, vbStrides, vbOffsets);
			c->IAGetInputLayout(&layout);
		}

		template <typename T>
		static void release(T *&p)
		{
			if (p)
				p->Release();
			p = nullptr;
		}

		void restore(ID3D11DeviceContext *c)
		{
			c->RSSetScissorRects(scissorCount, scissors);
			c->RSSetViewports(viewportCount, viewports);
			c->RSSetState(rs);
			c->OMSetBlendState(blend, blendFactor, sampleMask);
			c->OMSetDepthStencilState(depth, stencilRef);
			c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
			c->PSSetShaderResources(0, 16, psSrvs);
			c->VSSetShaderResources(0, 16, vsSrvs);
			c->PSSetSamplers(0, 16, psSamplers);
			c->PSSetConstantBuffers(0, 14, psCbs);
			c->VSSetConstantBuffers(0, 14, vsCbs);
			c->PSSetShader(ps, nullptr, 0);
			c->VSSetShader(vs, nullptr, 0);
			c->GSSetShader(gs, nullptr, 0);
			c->IASetPrimitiveTopology(topology);
			c->IASetIndexBuffer(ib, ibFormat, ibOffset);
			c->IASetVertexBuffers(0, 4, vbs, vbStrides, vbOffsets);
			c->IASetInputLayout(layout);
			release(rs);
			release(blend);
			release(depth);
			for (auto &v : rtvs)
				release(v);
			release(dsv);
			for (auto &v : psSrvs)
				release(v);
			for (auto &v : vsSrvs)
				release(v);
			for (auto &v : psSamplers)
				release(v);
			for (auto &v : psCbs)
				release(v);
			for (auto &v : vsCbs)
				release(v);
			release(ps);
			release(vs);
			release(gs);
			release(ib);
			for (auto &v : vbs)
				release(v);
			release(layout);
		}
	};

	void on_init_effect_runtime(effect_runtime *runtime)
	{
		g_runtime = runtime;
	}

	void on_bind_render_targets(command_list *cmd_list, uint32_t count, const resource_view *rtvs, resource_view)
	{
		g_boundRtv = count != 0 ? rtvs[0] : resource_view{0};
		g_boundIsBackBuffer = g_runtime != nullptr && g_boundRtv.handle != 0 &&
							  cmd_list->get_device()->get_resource_from_view(g_boundRtv) == g_runtime->get_current_back_buffer();
	}

	/// Before every draw: at the second draw into the back buffer (HN's UI begins), composite Minecraft first.
	bool before_draw(command_list *cmd_list)
	{
		if (!g_boundIsBackBuffer)
			return false;
		++g_backBufferDraws;
		if (g_renderedThisFrame || g_runtime == nullptr || g_runtime->get_device()->get_api() != device_api::d3d11)
			return false;
		if (!g_sceneDrawn)
		{
			g_sceneDrawn = true; // the scene's own pass into the back buffer: let it draw
			return false;
		}
		g_renderedThisFrame = true;
		auto *context = reinterpret_cast<ID3D11DeviceContext *>(cmd_list->get_native());
		D3D11StateBackup state;
		state.capture(context);
		g_runtime->render_effects(cmd_list, g_boundRtv, g_boundRtv);
		state.restore(context);
		return false;
	}

	bool on_draw(command_list *cmd_list, uint32_t, uint32_t, uint32_t, uint32_t)
	{
		return before_draw(cmd_list);
	}

	bool on_draw_indexed(command_list *cmd_list, uint32_t, uint32_t, uint32_t, int32_t, uint32_t)
	{
		return before_draw(cmd_list);
	}

	bool on_draw_indirect(command_list *cmd_list, indirect_command type, resource, uint64_t, uint32_t, uint32_t)
	{
		return type != indirect_command::dispatch && before_draw(cmd_list);
	}

	void on_present(effect_runtime *runtime)
	{
		watch_effect_file(runtime); // every frame, even when the effect failed to compile
		g_runtime = runtime; // registered after ReShade created its runtime: init_effect_runtime never came
		// what the hook saw, now and then (ReShade.log)
		static int frames = 0, early = 0;
		++frames;
		early += g_renderedThisFrame ? 1 : 0;
		if (frames == 600)
		{
			char text[256];
			snprintf(text, sizeof(text), "NeighborCraft: composited before HN's UI in %d of %d frames (back buffer draws: %d); Minecraft frame %ux%u fov %.2f, HN back buffer %ux%u",
				early, frames, g_backBufferDraws, g_width, g_height, g_mcPose.fov, g_bbWidth.load(), g_bbHeight.load());
			reshade::log::message(reshade::log::level::info, text);
			frames = early = 0;
		}
		// the next frame starts: look for its scene pass and its first UI draw again
		g_sceneDrawn = false;
		g_renderedThisFrame = false;
		g_backBufferDraws = 0;
	}

	void on_begin_effects(effect_runtime *runtime, command_list *, resource_view, resource_view)
	{
		g_runtime = runtime;
		uint32_t bw = 0, bh = 0;
		runtime->get_screenshot_width_and_height(&bw, &bh);
		g_bbWidth = bw;
		g_bbHeight = bh;
		bool on = g_active && open_mapping();
		if (on)
			upload(runtime);
		on = on && g_hasFrame;
		// The technique stays enabled (preset); McActive gates it, so GTA passes through untouched until a
		// Minecraft frame is here. (Toggling techniques from inside this callback crashes ReShade.)
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "McActive"); v.handle != 0)
			runtime->set_uniform_value_bool(v, on);
		if (!on)
			return;
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "McPlanes"); v.handle != 0)
			runtime->set_uniform_value_float(v, g_mcNear, g_mcFar, float(g_mcFlags));
		// a scene's look overrides the preset's light matching and depth bias; the preset's values come back after
		auto look = [&](const char *name, float want, float &saved) {
			const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, name);
			if (v.handle == 0)
				return;
			if (want >= 0.0f)
			{
				if (saved < 0.0f)
					runtime->get_uniform_value_float(v, &saved, 1);
				runtime->set_uniform_value_float(v, want);
			}
			else if (saved >= 0.0f)
			{
				runtime->set_uniform_value_float(v, saved);
				saved = -1.0f;
			}
		};
		look("LightMatch", g_lookLight.load(), g_savedLight);
		look("DepthBias", g_lookBias.load(), g_savedBias);
		look("SlopeBias", g_lookSlope.load(), g_savedSlope);
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "Shake"); v.handle != 0)
			runtime->set_uniform_value_float(v, g_shakeX.load(), g_shakeY.load(), g_shakeRoll.load());
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "PortalWarp"); v.handle != 0)
			runtime->set_uniform_value_float(v, g_portalWarp.load());
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "HostLightColour"); v.handle != 0)
			runtime->set_uniform_value_float(v, g_lightR.load(), g_lightG.load(), g_lightB.load());
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "HostPlanes"); v.handle != 0)
			runtime->set_uniform_value_float(v, g_hostNear.load(), g_hostFar.load());

		// Re-projection from Minecraft's pose to GTA's latest (extrapolated by the effect's PosePrediction frames).
		Pose host, prev;
		{
			std::lock_guard<std::mutex> lock(g_poseLock);
			const unsigned lag = unsigned(std::clamp(g_poseLag.load(), 0, 2));
			if (g_hostPoseCount > lag)
				host = g_hostPoses[(g_hostPoseCount - 1 - lag) & 3];
			if (g_hostPoseCount > lag + 1)
				prev = g_hostPoses[(g_hostPoseCount - 2 - lag) & 3];
		}
		float predict = 0.0f;
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "PosePrediction"); v.handle != 0)
			runtime->get_uniform_value_float(v, &predict, 1);
		const bool warp = host.valid && g_mcPose.valid && !g_cameraLocked;
		if (warp && prev.valid && predict != 0.0f)
		{
			auto delta = [](float a, float b) { float d = std::fmod(a - b + 540.0f, 360.0f) - 180.0f; return d; };
			host.yaw += delta(host.yaw, prev.yaw) * predict;
			host.pitch += (host.pitch - prev.pitch) * predict;
			host.roll += (host.roll - prev.roll) * predict;
			host.x += (host.x - prev.x) * predict;
			host.y += (host.y - prev.y) * predict;
			host.z += (host.z - prev.z) * predict;
		}
		float m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
		float t[3] = {0, 0, 0};
		if (warp)
		{
			warp_matrix(host, g_mcPose, m);
			// T = R_mc^T (host position - Minecraft's camera position), in Minecraft's camera space
			float rm[3][3];
			camera_rotation(g_mcPose, rm);
			const float d[3] = {float(host.x - g_mcPose.x), float(host.y - g_mcPose.y), float(host.z - g_mcPose.z)};
			for (int i = 0; i < 3; ++i)
				t[i] = rm[0][i] * d[0] + rm[1][i] * d[1] + rm[2][i] * d[2];
		}
		const char *rows[3] = {"WarpRow0", "WarpRow1", "WarpRow2"};
		for (int i = 0; i < 3; ++i)
			if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, rows[i]); v.handle != 0)
				runtime->set_uniform_value_float(v, m[i][0], m[i][1], m[i][2]);
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "WarpT"); v.handle != 0)
			runtime->set_uniform_value_float(v, t[0], t[1], t[2]);
		const float d2r = 3.14159265f / 180.0f;
		const float tanHost = std::tan((warp ? host.fov : g_mcPose.fov) * d2r * 0.5f), tanMc = std::tan(g_mcPose.fov * d2r * 0.5f);
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "WarpTan"); v.handle != 0)
			runtime->set_uniform_value_float(v, tanHost, tanMc, float(g_width) / float(g_height));
	}

	void on_reloaded_effects(effect_runtime *runtime)
	{
		if (g_width != 0)
			bind(runtime);
	}

	void on_destroy_effect_runtime(effect_runtime *runtime)
	{
		if (g_runtime == runtime)
			g_runtime = nullptr;
		destroy_layers(runtime->get_device());
	}
}

namespace compositor
{
	bool try_register(void *module)
	{
		if (g_registered)
			return true;
		if (!reshade::register_addon(module))
			return false;
		reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin_effects);
		reshade::register_event<reshade::addon_event::reshade_present>(on_present);
		reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(on_reloaded_effects);
		reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
		reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
		reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_render_targets);
		reshade::register_event<reshade::addon_event::draw>(on_draw);
		reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
		reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(on_draw_indirect);
		g_registered = true;
		reshade::log::message(reshade::log::level::info, "NeighborCraft: registered");
		return true;
	}

	void unregister(void *module)
	{
		if (g_registered.exchange(false))
			reshade::unregister_addon(module);
	}

	void set_active(bool active)
	{
		g_active = active;
	}

	void set_host_planes(float near_clip, float far_clip)
	{
		g_hostNear = near_clip;
		g_hostFar = far_clip;
	}

	void backbuffer_size(int &width, int &height)
	{
		width = int(g_bbWidth.load());
		height = int(g_bbHeight.load());
	}

	void set_camera_locked(bool locked)
	{
		g_cameraLocked = locked;
	}

	void set_screen_fx(float shake_x, float shake_y, float shake_roll, float portal_warp)
	{
		g_shakeX = shake_x;
		g_shakeY = shake_y;
		g_shakeRoll = shake_roll;
		g_portalWarp = portal_warp;
	}

	void set_light_colour(float r, float g, float b)
	{
		g_lightR = r;
		g_lightG = g;
		g_lightB = b;
	}

	void set_look(float light_match, float depth_bias, float slope_bias)
	{
		g_lookLight = light_match;
		g_lookBias = depth_bias;
		g_lookSlope = slope_bias;
	}

	void set_host_pose(float yaw, float pitch, float roll, float fov, double x, double y, double z)
	{
		std::lock_guard<std::mutex> lock(g_poseLock);
		g_hostPoses[g_hostPoseCount & 3] = {yaw, pitch, roll, fov, x, y, z, true};
		++g_hostPoseCount;
	}

	void set_pose_lag(int frames)
	{
		g_poseLag = frames;
	}
}
