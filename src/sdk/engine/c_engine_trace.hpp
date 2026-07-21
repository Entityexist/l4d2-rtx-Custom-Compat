#pragma once

#define ENGINE_TRACE_CLIENT_VERSION_004 "EngineTraceClient004"
#define ENGINE_TRACE_CLIENT_VERSION_003 "EngineTraceClient003"

namespace sdk
{
	enum trace_type_t
	{
		TRACE_EVERYTHING = 0,
		TRACE_WORLD_ONLY,
		TRACE_ENTITIES_ONLY,
		TRACE_EVERYTHING_FILTER_PROPS,
	};

	class trace_filter
	{
	public:
		virtual bool should_hit_entity(void* entity, int contents_mask) = 0;
		virtual trace_type_t get_trace_type() const = 0;
	};

	class trace_filter_skip_entity final : public trace_filter
	{
	public:
		explicit trace_filter_skip_entity(const void* skip) : m_skip(skip) {}

		bool should_hit_entity(void* entity, int) override { return entity != m_skip; }
		trace_type_t get_trace_type() const override { return TRACE_EVERYTHING_FILTER_PROPS; }

	private:
		const void* m_skip = nullptr;
	};

	struct base_trace
	{
		Vector startpos = {};
		Vector endpos = {};
		components::cplane_t plane = {};
		float fraction = 1.0f;
		int contents = 0;
		unsigned short disp_flags = 0;
		bool all_solid = false;
		bool start_solid = false;
	};

	struct game_trace : base_trace
	{
		float fraction_left_solid = 0.0f;
		components::csurface_t surface = {};
		int hit_group = 0;
		short physics_bone = 0;
		unsigned short world_surface_index = 0;
		void* entity = nullptr;
		int hit_box = 0;

		bool did_hit() const { return fraction < 1.0f || all_solid || start_solid; }
	};

#if defined(_M_IX86)
	static_assert(sizeof(base_trace) == 56u, "L4D2 CBaseTrace ABI mismatch");
	static_assert(sizeof(game_trace) == 84u, "L4D2 CGameTrace ABI mismatch");
	static_assert(sizeof(components::Ray_t) == 80u, "L4D2 Ray_t ABI mismatch");
#endif

	class engine_trace
	{
	public:
		static void configure_trace_ray_slot(const std::size_t slot)
		{
			s_trace_ray_slot = slot;
		}

		static std::size_t trace_ray_slot()
		{
			return s_trace_ray_slot;
		}

		void trace_ray(const components::Ray_t& ray, const unsigned int mask, trace_filter* filter, game_trace* trace)
		{
			using fn = void(__thiscall*)(engine_trace*, const components::Ray_t&, unsigned int, trace_filter*, game_trace*);
			// L4D2 EngineTraceClient003 inserts one client-only method before TraceRay.
			// Binary audit of the 2026-06-30 engine.dll places TraceRay at slot 5.
			// Slot 4 is ClipRayToCollideable and interprets the filter as ICollideable,
			// which caused the Light Studio MMB crash by calling vtable slot 7.
			return (*reinterpret_cast<fn**>(this))[s_trace_ray_slot](this, ray, mask, filter, trace);
		}

	private:
		inline static std::size_t s_trace_ray_slot = 5u;
	};

	inline components::Ray_t make_ray(const Vector& start, const Vector& end)
	{
		components::Ray_t ray = {};
		ray.m_Start = start;
		ray.m_Delta = end - start;
		ray.m_StartOffset = Vector();
		ray.m_Extents = Vector();
		ray.m_pWorldAxisTransform = nullptr;
		ray.m_IsRay = true;
		ray.m_IsSwept = ray.m_Delta.LengthSqr() > 0.0f;
		return ray;
	}
}
