#pragma once

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>
#include <string_view>

namespace components::muzzle_sound_classifier
{
	struct result
	{
		bool candidate = false;
		bool accepted = false;
		bool hard_reject = false;
		bool strict_reject = false;
		const char* reason = "not a weapon sound";
	};

	inline result classify(const std::string_view sound_name, const bool strict_fire_tokens,
		const bool reject_weapon_handling, const bool allow_weapon_family_fallback)
	{
		std::string s(sound_name);
		std::transform(s.begin(), s.end(), s.begin(), [](const unsigned char c)
		{
			return static_cast<char>(std::tolower(c));
		});
		std::replace(s.begin(), s.end(), '\\', '/');

		auto contains_any = [&](std::initializer_list<const char*> needles) -> bool
		{
			for (const char* needle : needles)
			{
				if (needle && *needle && s.find(needle) != std::string::npos) {
					return true;
				}
			}
			return false;
		};

		const bool weapon_namespace = contains_any({
			"weapons/", "/weapon/", "weapon_", "weapon.", "gunfire/", "/gunfire/"
		});
		if (!weapon_namespace) {
			return {};
		}

		result out = {};
		out.candidate = true;

		// Delayed acoustic layers are never the primary muzzle event, even when their
		// filenames contain a fire token such as rifle_fire_tail.
		if (contains_any({
			"tail", "reverb", "echo", "distant", "_far", "/far",
			"roomtone", "room_tone", "reflection"
		}))
		{
			out.hard_reject = true;
			out.reason = "acoustic tail/reverb";
			return out;
		}

		// Evaluate positive evidence before ambiguous handling words. This is the key
		// recovery for valid names such as pump_shotgun_fire and bolt_rifle_fire.
		if (contains_any({
			"/gunfire/", "gunfire", "gun_fire", "weapon_fire",
			"/fire_", "/fire.", "_fire", "fire_", ".fire",
			"/shoot_", "/shoot.", "_shoot", "shoot_", ".shoot",
			"single_shot", "burst_shot", "shot_", "shot1", "shot2", "shot3",
			"shot.wav", "shot.mp3", ".single", "_single"
		}))
		{
			out.accepted = true;
			out.reason = "explicit fire/gunfire token";
			return out;
		}

		if (contains_any({
			"dryfire", "dry_fire", "dry-fire", "/empty", "_empty", "empty_",
			"/click", "_click", "click_", "reload", "pickup", "pick_up", "pick-up",
			"deploy", "draw", "equip", "unequip", "holster", "drop",
			"melee", "swing", "impact", "hit_world", "hitwall", "ricochet",
			"foley", "cloth", "zoom", "scope"
		}))
		{
			out.hard_reject = true;
			out.reason = "non-fire weapon event";
			return out;
		}

		const bool handling_event = contains_any({
			"raise", "lower", "aim", "ads", "shoulder", "ready", "select",
			"bolt", "pump", "cock", "slide", "lever", "charge", "charging",
			"magazine", "mag_in", "mag_out", "/mag_", "shell", "handle",
			"handling", "grab", "mechanical", "mech_"
		});
		if (reject_weapon_handling && handling_event)
		{
			out.hard_reject = true;
			out.reason = "weapon handling";
			return out;
		}

		const bool firearm_family = contains_any({
			"pistol", "magnum", "smg", "uzi", "mp5", "rifle", "ak47", "scar", "sg552",
			"shotgun", "autoshotgun", "chrome_shotgun", "pump_shotgun",
			"sniper", "hunting_rifle", "military_sniper", "awp", "scout",
			"machinegun", "minigun", "m60", "50cal", "grenade_launcher"
		});

		if (firearm_family && allow_weapon_family_fallback && !strict_fire_tokens)
		{
			out.accepted = true;
			out.reason = "balanced weapon-family fallback";
			return out;
		}

		if (strict_fire_tokens)
		{
			out.strict_reject = true;
			out.reason = "strict mode: no fire token";
			return out;
		}

		out.reason = firearm_family ? "weapon-family fallback disabled" : "not a firearm shot";
		return out;
	}
}
