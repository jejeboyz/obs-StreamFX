// Copyright (c) 2020 Michael Fabian Dirks <info@xaymar.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "filter-virtual-greenscreen.hpp"
#include "obs/gs/gs-helper.hpp"
#include "plugin.hpp"
#include "util/util-logging.hpp"

#include "warning-disable.hpp"
#include <algorithm>
#include "warning-enable.hpp"

#ifdef _DEBUG
#define ST_PREFIX "<%s> "
#define D_LOG_ERROR(x, ...) P_LOG_ERROR(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_WARNING(x, ...) P_LOG_WARN(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_INFO(x, ...) P_LOG_INFO(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_DEBUG(x, ...) P_LOG_DEBUG(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#else
#define ST_PREFIX "<filter::virtual_greenscreen> "
#define D_LOG_ERROR(...) P_LOG_ERROR(ST_PREFIX __VA_ARGS__)
#define D_LOG_WARNING(...) P_LOG_WARN(ST_PREFIX __VA_ARGS__)
#define D_LOG_INFO(...) P_LOG_INFO(ST_PREFIX __VA_ARGS__)
#define D_LOG_DEBUG(...) P_LOG_DEBUG(ST_PREFIX __VA_ARGS__)
#endif

#define ST_I18N "Filter.VirtualGreenscreen"
#define ST_KEY_PROVIDER "Provider"
#define ST_I18N_PROVIDER ST_I18N "." ST_KEY_PROVIDER
#define ST_I18N_PROVIDER_NVIDIA_GREENSCREEN ST_I18N_PROVIDER ".NVIDIA.Greenscreen"

#ifdef ENABLE_FILTER_VIRTUAL_GREENSCREEN_NVIDIA
#define ST_KEY_NVIDIA_GREENSCREEN "NVIDIA.Greenscreen"
#define ST_I18N_NVIDIA_GREENSCREEN ST_I18N "." ST_KEY_NVIDIA_GREENSCREEN
#define ST_KEY_NVIDIA_GREENSCREEN_MODE ST_KEY_NVIDIA_GREENSCREEN ".Mode"
#define ST_I18N_NVIDIA_GREENSCREEN_MODE ST_I18N_NVIDIA_GREENSCREEN ".Mode"
#define ST_I18N_NVIDIA_GREENSCREEN_MODE_PERFORMANCE ST_I18N_NVIDIA_GREENSCREEN_MODE ".Performance"
#define ST_I18N_NVIDIA_GREENSCREEN_MODE_QUALITY ST_I18N_NVIDIA_GREENSCREEN_MODE ".Quality"
#endif

using streamfx::filter::virtual_greenscreen::virtual_greenscreen_factory;
using streamfx::filter::virtual_greenscreen::virtual_greenscreen_instance;
using streamfx::filter::virtual_greenscreen::virtual_greenscreen_provider;

static constexpr std::string_view HELP_URL = "https://github.com/Xaymar/obs-StreamFX/wiki/Filter-Virtual-Greenscreen";

/** Priority of providers for automatic selection if more than one is available.
 * 
 */
static virtual_greenscreen_provider provider_priority[] = {
	virtual_greenscreen_provider::NVIDIA_GREENSCREEN,
};

const char* streamfx::filter::virtual_greenscreen::cstring(virtual_greenscreen_provider provider)
{
	switch (provider) {
	case virtual_greenscreen_provider::INVALID:
		return "N/A";
	case virtual_greenscreen_provider::AUTOMATIC:
		return D_TRANSLATE(S_STATE_AUTOMATIC);
	case virtual_greenscreen_provider::NVIDIA_GREENSCREEN:
		return D_TRANSLATE(ST_I18N_PROVIDER_NVIDIA_GREENSCREEN);
	default:
		throw std::runtime_error("Missing Conversion Entry");
	}
}

std::string streamfx::filter::virtual_greenscreen::string(virtual_greenscreen_provider provider)
{
	return cstring(provider);
}

//------------------------------------------------------------------------------
// Instance
//------------------------------------------------------------------------------
virtual_greenscreen_instance::virtual_greenscreen_instance(obs_data_t* data, obs_source_t* self)
	: obs::source_instance(data, self),

	  _size(1, 1), _provider(virtual_greenscreen_provider::INVALID),
	  _provider_ui(virtual_greenscreen_provider::INVALID), _provider_ready(false), _provider_lock(), _provider_task(),
	  _effect(), _channel0_sampler(), _channel1_sampler(), _input(), _output_color(), _output_alpha(), _dirty(true)
{
	D_LOG_DEBUG("Initializating... (Addr: 0x%" PRIuPTR ")", this);

	{
		::streamfx::obs::gs::context gctx;

		// Create the render target for the input buffering.
		_input = std::make_shared<::streamfx::obs::gs::rendertarget>(GS_RGBA_UNORM, GS_ZS_NONE);
		_input->render(1, 1); // Preallocate the RT on the driver and GPU.
		_output_color = _input->get_texture();
		_output_alpha = _input->get_texture();

		// Load the required effect.
		{
			std::filesystem::path file = ::streamfx::data_file_path("effects/virtual-greenscreen.effect");
			try {
				_effect = std::make_shared<::streamfx::obs::gs::effect>(file);
			} catch (...) {
				D_LOG_ERROR("Failed to load '%s'.", file.generic_u8string().c_str());
			}
		}

		// Create Samplers
		_channel0_sampler = std::make_shared<::streamfx::obs::gs::sampler>();
		_channel0_sampler->set_filter(gs_sample_filter::GS_FILTER_LINEAR);
		_channel0_sampler->set_address_mode_u(GS_ADDRESS_CLAMP);
		_channel0_sampler->set_address_mode_v(GS_ADDRESS_CLAMP);
		_channel1_sampler = std::make_shared<::streamfx::obs::gs::sampler>();
		_channel1_sampler->set_filter(gs_sample_filter::GS_FILTER_LINEAR);
		_channel1_sampler->set_address_mode_u(GS_ADDRESS_CLAMP);
		_channel1_sampler->set_address_mode_v(GS_ADDRESS_CLAMP);
	}

	if (data) {
		load(data);
	}
}

virtual_greenscreen_instance::~virtual_greenscreen_instance()
{
	D_LOG_DEBUG("Finalizing... (Addr: 0x%" PRIuPTR ")", this);

	{ // Unload the underlying effect ASAP.
		std::unique_lock<std::mutex> ul(_provider_lock);

		// De-queue the underlying task.
		if (_provider_task) {
			streamfx::threadpool()->pop(_provider_task);
			_provider_task->await_completion();
			_provider_task.reset();
		}

		// TODO: Make this asynchronous.
		switch (_provider) {
#ifdef ENABLE_FILTER_VIRTUAL_GREENSCREEN_NVIDIA
		case virtual_greenscreen_provider::NVIDIA_GREENSCREEN:
			nvvfxgs_unload();
			break;
#endif
		default:
			break;
		}
	}
}

void virtual_greenscreen_instance::load(obs_data_t* data)
{
	update(data);
}

void virtual_greenscreen_instance::migrate(obs_data_t* data, uint64_t version) {}

void virtual_greenscreen_instance::update(obs_data_t* data)
{
	// Check if the user changed which Denoising provider we use.
	virtual_greenscreen_provider provider =
		static_cast<virtual_greenscreen_provider>(obs_data_get_int(data, ST_KEY_PROVIDER));
	if (provider == virtual_greenscreen_provider::AUTOMATIC) {
		provider = virtual_greenscreen_factory::get()->find_ideal_provider();
	}

	// Check if the provider was changed, and if so switch.
	if (provider != _provider) {
		_provider_ui = provider;
		switch_provider(provider);
	}

	if (_provider_ready) {
		std::unique_lock<std::mutex> ul(_provider_lock);

		switch (_provider) {
#ifdef ENABLE_FILTER_VIRTUAL_GREENSCREEN_NVIDIA
		case virtual_greenscreen_provider::NVIDIA_GREENSCREEN:
			nvvfxgs_update(data);
			break;
#endif
		default:
			break;
		}
	}
}

void streamfx::filter::virtual_greenscreen::virtual_greenscreen_instance::properties(obs_properties_t* properties)
{
	switch (_provider_ui) {
#ifdef ENABLE_FILTER_VIRTUAL_GREENSCREEN_NVIDIA
	case virtual_greenscreen_provider::NVIDIA_GREENSCREEN:
		nvvfxgs_properties(properties);
		break;
#endif
	default:
		break;
	}
}

uint32_t streamfx::filter::virtual_greenscreen::virtual_greenscreen_instance::get_width()
{
	return std::max<uint32_t>(_size.first, 1);
}

uint32_t streamfx::filter::virtual_greenscreen::virtual_greenscreen_instance::get_height()
{
	return std::max<uint32_t>(_size.second, 1);
}

void virtual_greenscreen_instance::video_tick(float_t time)
{
	auto target = obs_filter_get_target(_self);
	auto width  = obs_source_get_base_width(target);
	auto height = obs_source_get_base_height(target);
	_size       = {width, height};

	// Allow the provider to restrict the size.
	if (target && _provider_ready) {
		std::unique_lock<std::mutex> ul(_provider_lock);

		switch (_provider) {
#ifdef ENABLE_FILTER_VIRTUAL_GREENSCREEN_NVIDIA
		case virtual_greenscreen_provider::NVIDIA_GREENSCREEN:
			nvvfxgs_size();
			break;
#endif
		default:
			break;
		}
	}

	_dirty = true;
}

void virtual_greenscreen_instance::video_render(gs_effect_t* effect)
{
	auto parent = obs_filter_get_parent(_self);
	auto target = obs_filter_get_target(_self);
	auto width  = obs_source_get_base_width(target);
	auto height = obs_source_get_base_height(target);
	vec4 blank  = vec4{0, 0, 0, 0};

	// Ensure we have the bare minimum of valid information.
	target = target ? target : parent;
	effect = effect ? effect : obs_get_base_effect(OBS_EFFECT_DEFAULT);

	// Skip the filter if:
	// - The Provider isn't ready yet.
	// - We don't have a target.
	// - The width/height of the next filter in the chain is empty.
	if (!_provider_ready || !target || (width == 0) || (height == 0)) {
		obs_source_skip_video_filter(_self);
		return;
	}

#ifdef ENABLE_PROFILING
	::streamfx::obs::gs::debug_marker profiler0{::streamfx::obs::gs::debug_color_source,
												"StreamFX Virtual Green-Screen"};
	::streamfx::obs::gs::debug_marker profiler0_0{::streamfx::obs::gs::debug_color_gray, "'%s' on '%s'",
												  obs_source_get_name(_self), obs_source_get_name(parent)};
#endif

	if (_dirty) {
		// Lock the provider from being changed.
		std::unique_lock<std::mutex> ul(_provider_lock);

		{ // Capture the incoming frame.
#ifdef ENABLE_PROFILING
			::streamfx::obs::gs::debug_marker profiler1{::streamfx::obs::gs::debug_color_capture, "Capture"};
#endif
			if (obs_source_process_filter_begin(_self, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) {
				auto op = _input->render(_size.first, _size.second);

				// Matrix
				gs_matrix_push();
				gs_ortho(0., 1., 0., 1., 0., 1.);

				// Clear the buffer
				gs_clear(GS_CLEAR_COLOR | GS_CLEAR_DEPTH, &blank, 0, 0);

				// Set GPU state
				gs_blend_state_push();
				gs_enable_color(true, true, true, true);
				gs_enable_blending(false);
				gs_enable_depth_test(false);
				gs_enable_stencil_test(false);
				gs_set_cull_mode(GS_NEITHER);

				// Render
#ifdef ENABLE_PROFILING
				::streamfx::obs::gs::debug_marker profiler2{::streamfx::obs::gs::debug_color_capture, "Storage"};
#endif
				obs_source_process_filter_end(_self, obs_get_base_effect(OBS_EFFECT_DEFAULT), 1, 1);

				// Reset GPU state
				gs_blend_state_pop();
				gs_matrix_pop();
			} else {
				obs_source_skip_video_filter(_self);
				return;
			}

			_output_color = _input->get_texture();
			_output_alpha = _output_color;
		}

		try { // Process the captured input with the provider.
#ifdef ENABLE_PROFILING
			::streamfx::obs::gs::debug_marker profiler1{::streamfx::obs::gs::debug_color_convert, "Process"};
#endif
			switch (_provider) {
#ifdef ENABLE_FILTER_VIRTUAL_GREENSCREEN_NVIDIA
			case virtual_greenscreen_provider::NVIDIA_GREENSCREEN:
				nvvfxgs_process(_output_color, _output_alpha);
				break;
#endif
			default:
				break;
			}
		} catch (...) {
			obs_source_skip_video_filter(_self);
			return;
		}

		_dirty = false;
	}

	{ // Draw the result for the next filter to use.
#ifdef ENABLE_PROFILING
		::streamfx::obs::gs::debug_marker profiler1{::streamfx::obs::gs::debug_color_render, "Render"};
#endif
		if (_effect->has_parameter("InputA", ::streamfx::obs::gs::effect_parameter::type::Texture)) {
			_effect->get_parameter("InputA").set_texture(_output_color);
		}
		if (_effect->has_parameter("InputB", ::streamfx::obs::gs::effect_parameter::type::Texture)) {
			_effect->get_parameter("InputB").set_texture(_output_alpha);
		}
		if (_effect->has_parameter("Threshold", ::streamfx::obs::gs::effect_parameter::type::Float)) {
			_effect->get_parameter("Threshold").set_float(.666667);
		}
		if (_effect->has_parameter("ThresholdRange", ::streamfx::obs::gs::effect_parameter::type::Float)) {
			_effect->get_parameter("ThresholdRange").set_float(.333333);
		}
		while (gs_effect_loop(_effect->get_object(), "DrawAlphaThreshold")) {
			gs_draw_sprite(nullptr, 0, _size.first, _size.second);
		}
	}
}

struct switch_provider_data_t {
	virtual_greenscreen_provider provider;
};

void streamfx::filter::virtual_greenscreen::virtual_greenscreen_instance::switch_provider(
	virtual_greenscreen_provider provider)
{
	std::unique_lock<std::mutex> ul(_provider_lock);

	// Safeguard against calls made from unlocked memory.
	if (provider == _provider) {
		return;
	}

	// This doesn't work correctly.
	// - Need to allow multiple switches at once because OBS is weird.
	// - Doesn't guarantee that the task is properly killed off.

	// Log information.
	D_LOG_INFO("Instance '%s' is switching provider from '%s' to '%s'.", obs_source_get_name(_self), cstring(_provider),
			   cstring(provider));

	// If there is an existing task, attempt to cancel it.
	if (_provider_task) {
		// De-queue it.
		streamfx::threadpool()->pop(_provider_task);

		// Await the death of the task itself.
		_provider_task->await_completion();

		// Clear any memory associated with it.
		_provider_task.reset();
	}

	// Build data to pass into the task.
	auto spd      = std::make_shared<switch_provider_data_t>();
	spd->provider = _provider;
	_provider     = provider;

	// Then spawn a new task to switch provider.
	_provider_task = streamfx::threadpool()->push(
		std::bind(&virtual_greenscreen_instance::task_switch_provider, this, std::placeholders::_1), spd);
}

void streamfx::filter::virtual_greenscreen::virtual_greenscreen_instance::task_switch_provider(
	util::threadpool::task_data_t data)
{
	std::shared_ptr<switch_provider_data_t> spd = std::static_pointer_cast<switch_provider_data_t>(data);

	// Mark the provider as no longer ready.
	_provider_ready = false;

	// Lock the provider from being used.
	std::unique_lock<std::mutex> ul(_provider_lock);

	try {
		// Unload the previous provider.
		switch (spd->provider) {
#ifdef ENABLE_FILTER_VIRTUAL_GREENSCREEN_NVIDIA
		case virtual_greenscreen_provider::NVIDIA_GREENSCREEN:
			nvvfxgs_unload();
			break;
#endif
		default:
			break;
		}

		// Load the new provider.
		switch (_provider) {
#ifdef ENABLE_FILTER_VIRTUAL_GREENSCREEN_NVIDIA
		case virtual_greenscreen_provider::NVIDIA_GREENSCREEN:
			nvvfxgs_load();
			{
				auto data = obs_source_get_settings(_self);
				nvvfxgs_update(data);
				obs_data_release(data);
			}
			break;
#endif
		default:
			break;
		}

		// Log information.
		D_LOG_INFO("Instance '%s' switched provider from '%s' to '%s'.", obs_source_get_name(_self),
				   cstring(spd->provider), cstring(_provider));

		// Set the new provider as valid.
		_provider_ready = true;
	} catch (std::exception const& ex) {
		// Log information.
		D_LOG_ERROR("Instance '%s' failed switching provider with error: %s", obs_source_get_name(_self), ex.what());
	}
}

#ifdef ENABLE_FILTER_VIRTUAL_GREENSCREEN_NVIDIA
void streamfx::filter::virtual_greenscreen::virtual_greenscreen_instance::nvvfxgs_load()
{
	_nvidia_fx = std::make_shared<::streamfx::nvidia::vfx::greenscreen>();
}

void streamfx::filter::virtual_greenscreen::virtual_greenscreen_instance::nvvfxgs_unload()
{
	_nvidia_fx.reset();
}

void streamfx::filter::virtual_greenscreen::virtual_greenscreen_instance::nvvfxgs_size()
{
	if (!_nvidia_fx) {
		return;
	}

	_nvidia_fx->size(_size);
}

void streamfx::filter::virtual_greenscreen::virtual_greenscreen_instance::nvvfxgs_process(
	std::shared_ptr<::streamfx::obs::gs::texture>& color, std::shared_ptr<::streamfx::obs::gs::texture>& alpha)
{
	if (!_nvidia_fx) {
		return;
	}

	alpha = _nvidia_fx->process(_input->get_texture());
	color = _nvidia_fx->get_color();
}

void streamfx::filter::virtual_greenscreen::virtual_greenscreen_instance::nvvfxgs_properties(obs_properties_t* props)
{
	obs_properties_t* grp = obs_properties_create();
	obs_properties_add_group(props, ST_KEY_NVIDIA_GREENSCREEN, D_TRANSLATE(ST_I18N_NVIDIA_GREENSCREEN),
							 OBS_GROUP_NORMAL, grp);

	{
		auto p =
			obs_properties_add_list(grp, ST_KEY_NVIDIA_GREENSCREEN_MODE, D_TRANSLATE(ST_I18N_NVIDIA_GREENSCREEN_MODE),
									OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(p, D_TRANSLATE(ST_I18N_NVIDIA_GREENSCREEN_MODE_PERFORMANCE),
								  static_cast<int64_t>(::streamfx::nvidia::vfx::greenscreen_mode::PERFORMANCE));
		obs_property_list_add_int(p, D_TRANSLATE(ST_I18N_NVIDIA_GREENSCREEN_MODE_QUALITY),
								  static_cast<int64_t>(::streamfx::nvidia::vfx::greenscreen_mode::QUALITY));
	}
}

void streamfx::filter::virtual_greenscreen::virtual_greenscreen_instance::nvvfxgs_update(obs_data_t* data)
{
	if (!_nvidia_fx)
		return;

	_nvidia_fx->set_mode(
		static_cast<::streamfx::nvidia::vfx::greenscreen_mode>(obs_data_get_int(data, ST_KEY_NVIDIA_GREENSCREEN_MODE)));
}

#endif

//------------------------------------------------------------------------------
// Factory
//------------------------------------------------------------------------------
virtual_greenscreen_factory::~virtual_greenscreen_factory() {}

virtual_greenscreen_factory::virtual_greenscreen_factory()
{
	bool any_available = false;

	// 1. Try and load any configured providers.
#ifdef ENABLE_FILTER_VIRTUAL_GREENSCREEN_NVIDIA
	try {
		// Load CVImage and Video Effects SDK.
		_nvcuda           = ::streamfx::nvidia::cuda::obs::get();
		_nvcvi            = ::streamfx::nvidia::cv::cv::get();
		_nvvfx            = ::streamfx::nvidia::vfx::vfx::get();
		_nvidia_available = true;
		any_available |= _nvidia_available;
	} catch (const std::exception& ex) {
		_nvidia_available = false;
		_nvvfx.reset();
		_nvcvi.reset();
		_nvcuda.reset();
		D_LOG_WARNING("Failed to make NVIDIA Greenscreen available due to error: %s", ex.what());
	} catch (...) {
		_nvidia_available = false;
		_nvvfx.reset();
		_nvcvi.reset();
		_nvcuda.reset();
		D_LOG_WARNING("Failed to make NVIDIA Greenscreen available.", nullptr);
	}
#endif

	// 2. Check if any of them managed to load at all.
	if (!any_available) {
		D_LOG_ERROR("All supported Virtual Greenscreen providers failed to initialize, disabling effect.", 0);
		return;
	}

	// 3. In any other case, register the filter!
	_info.id           = S_PREFIX "filter-virtual-greenscreen";
	_info.type         = OBS_SOURCE_TYPE_FILTER;
	_info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW /*| OBS_SOURCE_SRGB*/;

	support_size(true);
	finish_setup();
}

const char* virtual_greenscreen_factory::get_name()
{
	return D_TRANSLATE(ST_I18N);
}

void virtual_greenscreen_factory::get_defaults2(obs_data_t* data)
{
	obs_data_set_default_int(data, ST_KEY_PROVIDER, static_cast<int64_t>(virtual_greenscreen_provider::AUTOMATIC));

#ifdef ENABLE_FILTER_VIRTUAL_GREENSCREEN_NVIDIA
	obs_data_set_default_int(data, ST_KEY_NVIDIA_GREENSCREEN_MODE,
							 static_cast<int64_t>(::streamfx::nvidia::vfx::greenscreen_mode::QUALITY));
#endif
}

static bool modified_provider(obs_properties_t* props, obs_property_t*, obs_data_t* settings) noexcept
{
	try {
		return true;
	} catch (const std::exception& ex) {
		DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
		return false;
	} catch (...) {
		DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
		return false;
	}
}

obs_properties_t* virtual_greenscreen_factory::get_properties2(virtual_greenscreen_instance* data)
{
	obs_properties_t* pr = obs_properties_create();

#ifdef ENABLE_FRONTEND
	{
		obs_properties_add_button2(pr, S_MANUAL_OPEN, D_TRANSLATE(S_MANUAL_OPEN),
								   virtual_greenscreen_factory::on_manual_open, nullptr);
	}
#endif

	if (data) {
		data->properties(pr);
	}

	{ // Advanced Settings
		auto grp = obs_properties_create();
		obs_properties_add_group(pr, S_ADVANCED, D_TRANSLATE(S_ADVANCED), OBS_GROUP_NORMAL, grp);

		{
			auto p = obs_properties_add_list(grp, ST_KEY_PROVIDER, D_TRANSLATE(ST_I18N_PROVIDER), OBS_COMBO_TYPE_LIST,
											 OBS_COMBO_FORMAT_INT);
			obs_property_set_modified_callback(p, modified_provider);
			obs_property_list_add_int(p, D_TRANSLATE(S_STATE_AUTOMATIC),
									  static_cast<int64_t>(virtual_greenscreen_provider::AUTOMATIC));
			obs_property_list_add_int(p, D_TRANSLATE(ST_I18N_PROVIDER_NVIDIA_GREENSCREEN),
									  static_cast<int64_t>(virtual_greenscreen_provider::NVIDIA_GREENSCREEN));
		}
	}

	return pr;
}

#ifdef ENABLE_FRONTEND
bool virtual_greenscreen_factory::on_manual_open(obs_properties_t* props, obs_property_t* property, void* data)
{
	try {
		streamfx::open_url(HELP_URL);
		return false;
	} catch (const std::exception& ex) {
		D_LOG_ERROR("Failed to open manual due to error: %s", ex.what());
		return false;
	} catch (...) {
		D_LOG_ERROR("Failed to open manual due to unknown error.", "");
		return false;
	}
}
#endif

bool streamfx::filter::virtual_greenscreen::virtual_greenscreen_factory::is_provider_available(
	virtual_greenscreen_provider provider)
{
	switch (provider) {
#ifdef ENABLE_FILTER_VIRTUAL_GREENSCREEN_NVIDIA
	case virtual_greenscreen_provider::NVIDIA_GREENSCREEN:
		return _nvidia_available;
#endif
	default:
		return false;
	}
}

virtual_greenscreen_provider streamfx::filter::virtual_greenscreen::virtual_greenscreen_factory::find_ideal_provider()
{
	for (auto v : provider_priority) {
		if (virtual_greenscreen_factory::get()->is_provider_available(v)) {
			return v;
			break;
		}
	}
	return virtual_greenscreen_provider::AUTOMATIC;
}

std::shared_ptr<virtual_greenscreen_factory> _video_superresolution_factory_instance = nullptr;

void virtual_greenscreen_factory::initialize()
{
	try {
		if (!_video_superresolution_factory_instance)
			_video_superresolution_factory_instance = std::make_shared<virtual_greenscreen_factory>();
	} catch (const std::exception& ex) {
		D_LOG_ERROR("Failed to initialize due to error: %s", ex.what());
	} catch (...) {
		D_LOG_ERROR("Failed to initialize due to unknown error.", "");
	}
}

void virtual_greenscreen_factory::finalize()
{
	_video_superresolution_factory_instance.reset();
}

std::shared_ptr<virtual_greenscreen_factory> virtual_greenscreen_factory::get()
{
	return _video_superresolution_factory_instance;
}
