#include <DXL2_System/system.h>
#include <SDL.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>

namespace DXL2_System
{
	static u64 s_time;
	static f64 s_freq;
	static f64 s_refreshRate;

	static f64 s_dt = 1.0 / 60.0;		// This is just to handle the first frame, so any reasonable value will work.
	static const f64 c_maxDt = 0.05;	// 20 fps

	static bool s_synced = false;

	void init(f32 refreshRate, bool synced)
	{
		s_time = SDL_GetPerformanceCounter();
		s_freq = 1.0 / (f64)SDL_GetPerformanceFrequency();
		s_refreshRate = f64(refreshRate);
		s_synced = synced;
	}

	void shutdown()
	{
	}

	void update()
	{
		// This assumes that SDL_GetPerformanceCounter() is monotonic.
		// However if errors do occur, the dt clamp later should limit the side effects.
		const u64 curTime = SDL_GetPerformanceCounter();
		const u64 uDt = curTime - s_time;
		s_time = curTime;

		// Delta time since the previous frame.
		f64 dt = f64(uDt) * s_freq;

		// If vsync is enabled, then round up to the nearest vsync interval as our delta time.
		// Ideally, if we are hitting the correct framerate, this should always be 
		// 1 vsync interval.
		if (s_synced && s_refreshRate > 0.0f)
		{
			dt = floor(dt * s_refreshRate + 0.5) / s_refreshRate;
		}

		// Next make sure that if the current fps is too low, that the game just slows down.
		// This avoids the "spiral of death" when using fixed time steps and avoids issues
		// during loading spikes.
		// This caps the low end framerate before slowdown to 20 fps.
		s_dt = std::min(dt, c_maxDt);
	}

	// Timing
	// Return the delta time.
	f64 getDeltaTime()
	{
		return s_dt;
	}
}