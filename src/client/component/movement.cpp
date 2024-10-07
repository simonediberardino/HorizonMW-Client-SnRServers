#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "console.hpp"
#include "gsc/script_extension.hpp"

#include "game/dvars.hpp"
#include "game/game.hpp"

#include <utils/hook.hpp>
#include <utils/vector.hpp>

// https://github.com/xoxor4d/iw3xo-dev/blob/develop/src/components/modules/movement.cpp :)

namespace movement
{
	enum
	{
		CF_BIT_NOCLIP = (1 << 0),
		CF_BIT_UFO = (1 << 1),
		CF_BIT_FROZEN = (1 << 2),
		CF_BIT_DISABLE_USABILITY = (1 << 3),
		CF_BIT_NO_KNOCKBACK = (1 << 4),
	};

	enum
	{
		PWF_RELOAD = 1 << 0,
		PWF_USING_OFFHAND = 1 << 1,
		PWF_HOLDING_BREATH = 1 << 2,
		PWF_FRIENDLY_FIRE = 1 << 3,
		PWF_ENEMY_FIRE = 1 << 4,
		PWF_NO_ADS = 1 << 5,
		PWF_USING_NIGHTVISION = 1 << 6,
		PWF_DISABLE_WEAPONS = 1 << 7,
		PWF_TRIGGER_LEFT_FIRE = 1 << 8,
		PWF_TRIGGER_DOUBLE_FIRE = 1 << 9,
		PWF_USING_RECOILSCALE = 1 << 10,
		PWF_DISABLE_WEAPON_SWAPPING = 1 << 11,
		PWF_DISABLE_OFFHAND_WEAPONS = 1 << 12,
		PWF_SWITCHING_TO_RIOTSHIELD = 1 << 13,
		// IW5 flags backported
		PWF_DISABLE_WEAPON_PICKUP = 1 << 16
	};

	namespace
	{
		utils::hook::detour pm_airmove_hook;
		utils::hook::detour pm_is_ads_allowed_hook;
		utils::hook::detour pm_weapon_process_hand_hook;
		utils::hook::detour pm_weapon_fire_weapon_hook;

		game::dvar_t* pm_cs_airAccelerate;
		game::dvar_t* pm_cs_airSpeedCap;
		game::dvar_t* pm_cs_strafing;

		void pm_air_accelerate(game::vec3_t wishdir, float wishspeed, game::playerState_s* ps, game::pml_t* pml)
		{
			float wishspd = wishspeed, accelspeed, currentspeed, addspeed;

			auto accel = pm_cs_airAccelerate->current.value;
			auto airspeedcap = pm_cs_airSpeedCap->current.value;

			if (wishspd > airspeedcap)
			{
				wishspd = airspeedcap;
			}

			currentspeed = utils::vector::product(ps->velocity, wishdir);
			addspeed = wishspd - currentspeed;

			if (addspeed > 0)
			{
				accelspeed = pml->frametime * accel * wishspeed * 1.0f;

				if (accelspeed > addspeed)
				{
					accelspeed = addspeed;
				}

				for (auto i = 0; i < 3; i++)
				{
					ps->velocity[i] += wishdir[i] * accelspeed;
				}
			}
		}

		void pm_clip_velocity(game::vec3_t in, game::vec3_t normal, game::vec3_t out, float overbounce)
		{
			float backoff, change, angle, adjust;

			angle = normal[2];
			backoff = utils::vector::product(in, normal) * overbounce;

			for (auto i = 0; i < 3; i++)
			{
				change = normal[i] * backoff;
				out[i] = in[i] - change;
			}

			adjust = utils::vector::product(out, normal);

			if (adjust < 0)
			{
				game::vec3_t reduce{};

				utils::vector::scale(normal, adjust, reduce);
				utils::vector::subtract(out, reduce, out);
			}
		}

		void pm_try_playermove(game::pmove_t* pm, game::pml_t* pml)
		{
			const auto surf_slope = 0.7f;
			const auto ps = pm->ps;

			game::vec3_t end{};
			game::trace_t trace{};

			if (utils::vector::length(ps->velocity) == 0)
			{
				return;
			}

			utils::vector::ma(ps->origin, pml->frametime, ps->velocity, end);
			utils::hook::invoke<void>(0x2D14C0_b, pm, &trace, ps->origin, end, 
				&pm->bounds, ps->clientNum, pm->tracemask); // PM_playerTrace

			if (trace.fraction == 1)
			{
				return;
			}

			if (trace.normal[2] > surf_slope)
			{
				return;
			}

			pm_clip_velocity(ps->velocity, trace.normal, ps->velocity, 1.0f);
		}

		void pm_airmove_stub(game::pmove_t* pm, game::pml_t* pml)
		{
			if (!pm_cs_strafing->current.enabled)
			{
				return pm_airmove_hook.invoke<void>(pm, pml);
			}

			const auto ps = pm->ps;

			ps->sprintState.sprintButtonUpRequired = 1;

			float fmove{}, smove{}, wishspeed{};
			game::vec3_t wishvel{}, wishdir{};

			fmove = pm->cmd.forwardmove;
			smove = pm->cmd.rightmove;

			pml->forward[2] = 0.0f;
			pml->right[2] = 0.0f;

			utils::vector::normalize(pml->forward);
			utils::vector::normalize(pml->right);

			for (auto i = 0; i < 2; i++)
			{
				wishvel[i] = pml->forward[i] * fmove + pml->right[i] * smove;
			}

			wishvel[2] = 0;

			utils::vector::copy(wishvel, wishdir);
			wishspeed = utils::vector::normalize(wishdir);

			if (wishspeed != 0 && (wishspeed > 320.0f))
			{
				utils::vector::scale(wishvel, 320.0f / wishspeed, wishvel);
				wishspeed = 320.0f;
			}

			pm_air_accelerate(wishdir, wishspeed, ps, pml);

			utils::hook::invoke<void>(0x2D3380_b, pm, pml, 1, 1); // PM_StepSlideMove

			pm_try_playermove(pm, pml);
		}


		// @credits: https://github.com/REVLIIS/IW4-mechanics-for-H2M
		bool check_for_righty_tighty(game::pmove_t* pm)
		{
			if ((pm->oldcmd.buttons & game::BUTTON_USERELOAD) == 0 && ((pm->cmd.buttons & game::BUTTON_USERELOAD) != 0) ||
				((pm->oldcmd.buttons & game::BUTTON_RELOAD) == 0 && ((pm->cmd.buttons & game::BUTTON_RELOAD) != 0)))
			{
				if ((pm->ps->sprintState.lastSprintEnd - pm->ps->sprintState.lastSprintStart) < 50) //Increase to make righty tighty easier
				{
					if (game::PM_Weapon_AllowReload(pm->ps, game::WEAPON_HAND_RIGHT) && !game::PM_Weapon_AllowReload(pm->ps, game::WEAPON_HAND_LEFT))
					{
						game::PM_SetReloadingState(pm->ps, game::WEAPON_HAND_RIGHT);
						return true;
					}
				}
			}

			return false;
		}

		bool check_for_wrist_twist(game::pmove_t* pm)
		{
			if ((pm->cmd.buttons & game::BUTTON_USERELOAD) == 0 && ((pm->oldcmd.buttons & game::BUTTON_USERELOAD) != 0) ||
				(pm->cmd.buttons & game::BUTTON_RELOAD) == 0 && ((pm->oldcmd.buttons & game::BUTTON_RELOAD) != 0))
			{
				//if we are allowed to reload our left gun, and NOT allowed to reload right gun, start wrist twist
				if (game::PM_Weapon_AllowReload(pm->ps, game::WEAPON_HAND_LEFT) && !game::PM_Weapon_AllowReload(pm->ps, game::WEAPON_HAND_RIGHT))
				{
					game::PM_SetReloadingState(pm->ps, game::WEAPON_HAND_LEFT);
					pm->ps->torsoAnim = 3181; //reload anim, overrides the reset in BG_ClearReloadAnim, makes it so the sprint anim is shown on 3rd person character 
					return true;
				}
			}

			return false;
		}

		void sprint_drop(game::pmove_t* pm)
		{
			game::playerState_s* ps = pm->ps;

			int handIndex = game::BG_PlayerLastWeaponHand(ps);

			for (int i = 0; i <= handIndex; i++)
			{
				if (i == game::WEAPON_HAND_LEFT && check_for_righty_tighty(pm)) 
				{
					continue;
				}

				ps->weapState[i].weaponState = game::WEAPON_SPRINT_DROP;
				ps->weapState[i].weaponTime = game::BG_SprintOutTime(ps->weapCommon.weapon, false, ps->weapCommon.lastWeaponHand == game::WEAPON_HAND_LEFT);
				ps->weapState[i].weaponDelay = 0;

				if (ps->pm_type != game::PM_DEAD && ps->pm_type != game::PM_DEAD_LINKED)
				{
					ps->weapState[i].weapAnim = ps->weapState[i].weaponState & ANIM_TOGGLEBIT | game::WEAP_ANIM_SPEED_RELOAD;
				}
			}
		}

		void sprint_raise(game::pmove_t* pm)
		{
			game::playerState_s* ps = pm->ps;
			int handIndex = game::BG_PlayerLastWeaponHand(ps);

			for (int i = 0; i <= handIndex; i++)
			{
				ps->weapState[i].weaponState = game::WEAPON_SPRINT_RAISE;
				ps->weapState[i].weaponTime = game::BG_SprintInTime(ps->weapCommon.weapon, false, ps->weapCommon.lastWeaponHand == game::WEAPON_HAND_LEFT);
				ps->weapState[i].weaponDelay = 0;

				if (ps->pm_type != game::PM_DEAD && ps->pm_type != game::PM_DEAD_LINKED)
				{
					ps->weapState[i].weapAnim = ps->weapState[i].weaponState & ANIM_TOGGLEBIT | game::WEAP_ANIM_FAST_RELOAD_END;
				}

				if (ps->weapCommon.lastWeaponHand == game::WEAPON_HAND_LEFT)
				{
					if (i == game::WEAPON_HAND_RIGHT)
					{
						check_for_righty_tighty(pm);
					}
					else if (i == game::WEAPON_HAND_LEFT)
					{
						check_for_wrist_twist(pm);
					}
				}
			}
		}

		//reversed from iw4
		utils::hook::detour pm_weapon_check_for_sprint_hook;
		void pm_weapon_check_for_sprint_stub(game::pmove_t* pm)
		{
			if (!pm->cmd.weapon.data) 
			{
				return;
			}

			int weaponStateRight = pm->ps->weapState[game::WEAPON_HAND_RIGHT].weaponState;
			int weaponStateLeft = pm->ps->weapState[game::WEAPON_HAND_LEFT].weaponState;

			if (weaponStateRight != game::WEAPON_FIRING && weaponStateRight != game::WEAPON_RECHAMBERING && weaponStateRight != game::WEAPON_MELEE_WAIT_FOR_RESULT && weaponStateRight != game::WEAPON_MELEE_FIRE && weaponStateRight != game::WEAPON_MELEE_END)
			{
				if (weaponStateLeft != game::WEAPON_FIRING && weaponStateLeft != game::WEAPON_RECHAMBERING
					&& weaponStateLeft != game::WEAPON_MELEE_WAIT_FOR_RESULT && weaponStateLeft != game::WEAPON_MELEE_FIRE && weaponStateLeft != game::WEAPON_MELEE_END
					&& weaponStateRight != game::WEAPON_RAISING && weaponStateRight != game::WEAPON_RAISING_ALTSWITCH
					&& weaponStateRight != game::WEAPON_DROPPING && weaponStateRight != game::WEAPON_DROPPING_QUICK && weaponStateRight != game::WEAPON_DROPPING_ALT
					&& weaponStateRight != game::WEAPON_OFFHAND_INIT && weaponStateRight != game::WEAPON_OFFHAND_PREPARE && weaponStateRight != game::WEAPON_OFFHAND_HOLD && weaponStateRight != game::WEAPON_OFFHAND_HOLD_PRIMED && weaponStateRight != game::WEAPON_OFFHAND_END
					)
				{
					if (((pm->ps->pm_flags & game::PMF_SPRINTING) != 0) && (weaponStateRight != game::WEAPON_SPRINT_RAISE && weaponStateRight != game::WEAPON_SPRINT_LOOP && weaponStateRight != game::WEAPON_SPRINT_DROP))
					{
						sprint_raise(pm);
					}
					else if (((pm->ps->pm_flags & game::PMF_SPRINTING) == 0) && (weaponStateRight == game::WEAPON_SPRINT_RAISE || weaponStateRight == game::WEAPON_SPRINT_LOOP))
					{
						sprint_drop(pm);
					}
				}
			}
		}

		/*
			This detour fixes an issue when you're on servers and trying to wrist twist with +usereload
			I added an additional check to see if you're pressing the usereload button when the sprint raise event is happening,
			so if your connection isn't perfect you dont stop halfway trough a wrist twist
			Not fully reversed yet, but wasn't needed for v1.0.0.
		*/
		utils::hook::detour pm_sprint_ending_buttons_hook;
		bool pm_sprint_ending_buttons_stub(game::playerState_s* ps, int8_t forwardSpeed, int buttons)
		{
			if ((ps->pm_flags & (game::POF_PLAYER | game::POF_THERMAL_VISION_OVERLAY_FOF | game::POF_THERMAL_VISION)) != 0 || forwardSpeed <= 105)
			{
				return true;
			}

			int v5 = game::BG_HasPerk(ps->perks, game::PERK_BALLCARRIER);
			int v6 = ((0xCF0D - (v5 != 0)) & ~0x230) | (game::BUTTON_USERELOAD | game::BUTTON_RELOAD);

			if (!game::BG_HasPerk(ps->perks, game::PERK_RESISTEXPLOSION))
				v6 = (0xCF0D - (v5 != 0)) | (game::BUTTON_USERELOAD | game::BUTTON_RELOAD);

			int weaponState = ps->weapState[0].weaponState;

			if ((v6 & buttons) != 0)
			{
				if (ps->weapCommon.lastWeaponHand == game::WEAPON_HAND_LEFT && (buttons & game::BUTTON_USERELOAD) == 0 && weaponState == game::WEAPON_SPRINT_RAISE) //+usereload high ping fix
					return false;

				return true;
			}

			bool is_in_melee_or_nade_throw = (weaponState - game::WEAPON_MELEE_WAIT_FOR_RESULT) <= (game::WEAPON_OFFHAND_END - game::WEAPON_MELEE_WAIT_FOR_RESULT);
			bool is_in_nightvision_equip = (weaponState - game::WEAPON_NIGHTVISION_WEAR) <= (game::WEAPON_NIGHTVISION_REMOVE - game::WEAPON_NIGHTVISION_WEAR);
			bool is_in_blast_or_hybrid_scope = (weaponState - game::WEAPON_BLAST_IMPACT) <= (game::WEAPON_HEAT_COOLDOWN_START - game::WEAPON_BLAST_IMPACT);

			return is_in_melee_or_nade_throw || is_in_nightvision_equip || is_in_blast_or_hybrid_scope;
		}

		utils::hook::detour begin_weapon_change_hook;
		void begin_weapon_change_stub(game::pmove_t* pm, game::Weapon new_weap, bool is_new_alt, bool quick, unsigned int* holdrand)
		{
			auto stall_anim = false;

			auto right_anim = pm->ps->weapState[game::WEAPON_HAND_RIGHT].weapAnim;
			auto left_anim = pm->ps->weapState[game::WEAPON_HAND_LEFT].weapAnim;

			// (WEAP_ANIM_IDLE | ANIM_TOGGLEBIT)	| WEAP_ANIM_EMPTY_IDLE
			// WEAP_ANIM_EMPTY_IDLE					| (WEAP_ANIM_IDLE | ANIM_TOGGLEBIT)

			// Check if the player is sprinting while changing weapons by verifying if the sprint button is pressed in the current command.
			auto is_sprinting_during_weap_change = (pm->cmd.buttons & game::BUTTON_SPRINT) != 0;

			auto should_sprint_stall = (is_sprinting_during_weap_change || pm->ps->sprintState.lastSprintStart > pm->ps->sprintState.lastSprintEnd);
			// Patoke @todo: make it so this will only stall u if u sprinted beforehand
			//	the sprint action is slightly delayed after u shoot, since the shooting animation is being played, running won't replace this one until it's done
			//	meaning the sprint stall still takes effect even if the current weapon animation isn't supposed to be running
			//	the check still takes place but our current animation isn't overriden, meaning we stall our animations while another one is playing
			auto should_still_stall = ((right_anim == game::WEAP_ANIM_EMPTY_IDLE && left_anim == (game::WEAP_ANIM_IDLE | ANIM_TOGGLEBIT)) ||
				(left_anim == game::WEAP_ANIM_EMPTY_IDLE && right_anim == (game::WEAP_ANIM_IDLE | ANIM_TOGGLEBIT)));

#ifdef _DEBUG
			if (should_sprint_stall || should_still_stall)
			{
				console::debug("%d | %d", right_anim, left_anim);
				stall_anim = true;
			}
#else
			if (should_sprint_stall || should_still_stall)
			{
				stall_anim = true;
			}
#endif

			begin_weapon_change_hook.invoke<void>(pm, new_weap, is_new_alt, quick, holdrand);

			if (stall_anim)
			{
				pm->ps->weapState[game::WEAPON_HAND_RIGHT].weapAnim = right_anim;
				pm->ps->weapState[game::WEAPON_HAND_LEFT].weapAnim = left_anim;
			}
		}

		inline bool is_previous_anim(int anim)
		{
			return	(anim == game::WEAP_ANIM_IDLE || anim == game::WEAP_ANIM_FAST_RELOAD_END ||
					anim == (game::WEAP_ANIM_IDLE | ANIM_TOGGLEBIT) || anim == (game::WEAP_ANIM_FAST_RELOAD_END | ANIM_TOGGLEBIT));
		}

		utils::hook::detour start_weapon_anim_hook;
		void start_weapon_anim_stub(uint64_t local_client_num, game::Weapon weapon_idx, game::PlayerHandIndex player_hand_idx,
			game::weapAnimFiles_t blend_in_anim_index, game::weapAnimFiles_t blend_out_anim_index, float transition_time)
		{
			auto* cg_array = game::getCGArray();
			auto* playerstate = &cg_array[local_client_num].predictedPlayerState;

			auto should_sprint = (playerstate->sprintState.lastSprintStart < playerstate->sprintState.lastSprintEnd);

#ifdef _DEBUG
			// barrel rolls go from WEAP_ANIM_QUICK_RAISE/WEAP_ANIM_RAISE to WEAP_ANIM_IDLE
			if ((blend_out_anim_index == game::WEAP_ANIM_QUICK_RAISE || blend_out_anim_index == game::WEAP_ANIM_RAISE)
				&& is_previous_anim(playerstate->weapState[player_hand_idx].weapAnim) && should_sprint)
			{
				console::debug("[%d] should left hand flip! (out: %d, in: %d)", player_hand_idx, blend_out_anim_index, blend_in_anim_index);
			}
#endif

			if ((blend_out_anim_index == game::WEAP_ANIM_SPRINT_IN || blend_out_anim_index == game::WEAP_ANIM_SPRINT_LOOP) 
				&& is_previous_anim(playerstate->weapState[player_hand_idx].weapAnim) && should_sprint)
			{
				blend_out_anim_index = game::WEAP_ANIM_QUICK_DROP;
				transition_time = 0.5f;
			}

			start_weapon_anim_hook.invoke<void>(local_client_num, weapon_idx, player_hand_idx, blend_in_anim_index, blend_out_anim_index, transition_time);
		}
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			pm_airmove_hook.create(0x2C93B0_b, pm_airmove_stub);

			// fix moveSpeedScale not being used
			utils::hook::set<uint32_t>(0x4406FE_b, 0x1DC);

			pm_cs_airAccelerate = dvars::register_float("pm_cs_airAccelerate", 100.0f, 1.0f, 500.0f,
				game::DVAR_FLAG_REPLICATED | game::DVAR_FLAG_CHEAT,
				"Defines player acceleration mid-air");

			pm_cs_airSpeedCap = dvars::register_float("pm_cs_airSpeedCap", 30.0f, 1.0f, 500.0f,
				game::DVAR_FLAG_REPLICATED | game::DVAR_FLAG_CHEAT,
				"Maximum speed mid-air");

			pm_cs_strafing = dvars::register_bool("pm_cs_strafing", false,
				game::DVAR_FLAG_REPLICATED | game::DVAR_FLAG_CHEAT,
				"Enable CS like strafing");

			// solitude mechanics, makes things a bit more fluid
			begin_weapon_change_hook.create(0x2D57E0_b, begin_weapon_change_stub);

			// glides (thank you @girlmachinery for the help on this)
			start_weapon_anim_hook.create(0x1D5CA0_b, start_weapon_anim_stub);

			pm_weapon_check_for_sprint_hook.create(0x2D9A10_b, pm_weapon_check_for_sprint_stub);
			pm_sprint_ending_buttons_hook.create(0x2CEE40_b, pm_sprint_ending_buttons_stub);

			// force_play_weap_anim(anim_id, both_hands)
			gsc::method::add("force_play_weap_anim", [](const game::scr_entref_t ent, const gsc::function_args& args)
			{
				if (ent.classnum != 0)
				{
					throw std::runtime_error("invalid entity");
				}

				auto* client = game::g_entities[ent.entnum].client;
				if (client == nullptr)
				{
					throw std::runtime_error("not a player entity");
				}

				auto anim_id = args[0].as<int>();

				client->ps.weapState[game::WEAPON_HAND_RIGHT].weapAnim = anim_id;
				if (args[1].as<int>())
					client->ps.weapState[game::WEAPON_HAND_LEFT].weapAnim = anim_id;

				return scripting::script_value{};
			});
		}
	};
}

REGISTER_COMPONENT(movement::component)
